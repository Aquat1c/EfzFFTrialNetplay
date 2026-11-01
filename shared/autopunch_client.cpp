#include "autopunch_client.h"

#include "autopunch_protocol.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace trialnet::autopunch
{

namespace
{

const char* FormatEndpoint(const sockaddr_in& endpoint, char* out, const std::size_t capacity)
{
    char ip[INET_ADDRSTRLEN] = {};
    InetNtopA(AF_INET, const_cast<IN_ADDR*>(&endpoint.sin_addr), ip, static_cast<DWORD>(sizeof(ip)));
    std::snprintf(out, capacity, "%s:%u", ip[0] != '\0' ? ip : "?", ntohs(endpoint.sin_port));
    return out;
}

}

bool Client::Attach(
    const SOCKET socket,
    const std::uint16_t localPort,
    const sockaddr_in* const target,
    const bool targetPortKnown,
    const LogCallback logger,
    void* const loggerContext)
{
    Detach();
    if (socket == INVALID_SOCKET || localPort == 0u)
    {
        return false;
    }

    socket_ = socket;
    localPort_ = localPort;
    logger_ = logger;
    loggerContext_ = loggerContext;
    passiveListener_ = true;
    if (target != nullptr && target->sin_family == AF_INET && target->sin_addr.s_addr != 0u)
    {
        configuredTarget_ = *target;
        activeTarget_ = *target;
        targetSet_ = true;
        targetPortKnown_ = targetPortKnown && target->sin_port != 0u;
        targetInternalPort_ = targetPortKnown_ ? ntohs(target->sin_port) : 0u;
        passiveListener_ = false;
    }
    return true;
}

void Client::Detach()
{
    socket_ = INVALID_SOCKET;
    coordinator_ = {};
    configuredTarget_ = {};
    activeTarget_ = {};
    localPort_ = 0;
    targetInternalPort_ = 0;
    targetSet_ = false;
    targetPortKnown_ = false;
    passiveListener_ = false;
    coordinatorResolved_ = false;
    active_ = false;
    assisted_ = false;
    startedMs_ = 0;
    lastResolveMs_ = 0;
    lastRegisterMs_ = 0;
    lastLookupMs_ = 0;
    lastDirectMs_ = 0;
    logger_ = nullptr;
    loggerContext_ = nullptr;
    stats_ = {};
}

void Client::Start(const std::uint32_t nowMs)
{
    if (socket_ == INVALID_SOCKET || localPort_ == 0u || active_)
    {
        return;
    }
    active_ = true;
    startedMs_ = nowMs;
    Log(
        "fallback started local_port=%u coordinator=%s:%u target_known=%d",
        localPort_,
        kCoordinatorHost,
        kCoordinatorPort,
        targetPortKnown_ ? 1 : 0);
    Service(nowMs);
}

void Client::Stop(const char* const reason)
{
    if (!active_)
    {
        return;
    }
    active_ = false;
    Log(
        "fallback stopped reason=%s registers=%u lookups=%u punches=%u mappings=%u",
        reason != nullptr && reason[0] != '\0' ? reason : "?",
        stats_.registersSent,
        stats_.lookupsSent,
        stats_.directPunchesSent,
        stats_.mappingsReceived);
}

bool Client::ResolveCoordinator(const std::uint32_t nowMs)
{
    if (coordinatorResolved_)
    {
        return true;
    }
    if (lastResolveMs_ != 0u && nowMs - lastResolveMs_ < kResolveRetryMs)
    {
        return false;
    }
    lastResolveMs_ = nowMs;

    coordinator_ = {};
    coordinator_.sin_family = AF_INET;
    coordinator_.sin_port = htons(kCoordinatorPort);
    if (InetPtonA(AF_INET, kCoordinatorIpv4, &coordinator_.sin_addr) != 1)
    {
        Log("invalid pinned coordinator address host=%s ip=%s", kCoordinatorHost, kCoordinatorIpv4);
        return false;
    }
    coordinatorResolved_ = true;
    char endpoint[64] = {};
    Log(
        "coordinator pinned %s -> %s",
        kCoordinatorHost,
        FormatEndpoint(coordinator_, endpoint, sizeof(endpoint)));
    return true;
}

bool Client::IsCoordinator(const sockaddr_in& source) const
{
    return coordinatorResolved_
        && source.sin_family == AF_INET
        && source.sin_addr.s_addr == coordinator_.sin_addr.s_addr
        && source.sin_port == coordinator_.sin_port;
}

bool Client::SendRaw(
    const sockaddr_in& destination,
    const void* const bytes,
    const std::size_t length)
{
    if (socket_ == INVALID_SOCKET || bytes == nullptr || length == 0u)
    {
        return false;
    }
    return sendto(
               socket_,
               static_cast<const char*>(bytes),
               static_cast<int>(length),
               0,
               reinterpret_cast<const sockaddr*>(&destination),
               sizeof(destination)) == static_cast<int>(length);
}

bool Client::SendRegister()
{
    const RegisterPacket packet = MakeRegister(localPort_);
    if (!SendRaw(coordinator_, packet.data(), packet.size()))
    {
        return false;
    }
    ++stats_.registersSent;
    return true;
}

bool Client::SendLookup()
{
    if (passiveListener_ || !targetSet_ || !targetPortKnown_)
    {
        return false;
    }
    const LookupPacket packet = MakeLookup(
        localPort_,
        configuredTarget_.sin_addr.s_addr,
        targetInternalPort_);
    if (!SendRaw(coordinator_, packet.data(), packet.size()))
    {
        return false;
    }
    ++stats_.lookupsSent;
    return true;
}

int Client::SendDirectBurst(const sockaddr_in& destination, const int count)
{
    static constexpr std::uint8_t kPunch = 0u;
    int sent = 0;
    for (int i = 0; i < count; ++i)
    {
        if (SendRaw(destination, &kPunch, sizeof(kPunch)))
        {
            ++sent;
        }
    }
    stats_.directPunchesSent += static_cast<std::uint32_t>(sent);
    return sent;
}

void Client::Service(const std::uint32_t nowMs)
{
    if (!active_ || socket_ == INVALID_SOCKET || !ResolveCoordinator(nowMs))
    {
        return;
    }

    if (lastRegisterMs_ == 0u || nowMs - lastRegisterMs_ >= kRegisterIntervalMs)
    {
        lastRegisterMs_ = nowMs;
        SendRegister();
    }

    if (passiveListener_ || !targetSet_ || !targetPortKnown_)
    {
        return;
    }

    if (lastLookupMs_ == 0u || nowMs - lastLookupMs_ >= kLookupIntervalMs)
    {
        lastLookupMs_ = nowMs;
        SendLookup();
    }
    if (lastDirectMs_ == 0u || nowMs - lastDirectMs_ >= kDirectPunchIntervalMs)
    {
        lastDirectMs_ = nowMs;
        SendDirectBurst(activeTarget_, 1);
    }
}

InboundResult Client::HandleInbound(
    const void* const bytes,
    const std::size_t length,
    const sockaddr_in& source,
    sockaddr_in* const outPeer)
{
    if (IsCoordinator(source))
    {
        Mapping mapping{};
        if (!ParseMapping(bytes, length, &mapping))
        {
            Log("ignored coordinator payload length=%zu", length);
            return InboundResult::Consumed;
        }
        if (!passiveListener_
            && (!targetSet_
                || mapping.hostNetworkOrder != configuredTarget_.sin_addr.s_addr
                || (targetPortKnown_ && mapping.internalPort != targetInternalPort_)))
        {
            Log(
                "ignored coordinator mapping internal=%u nat=%u host=0x%08X",
                mapping.internalPort,
                mapping.natPort,
                mapping.hostNetworkOrder);
            return InboundResult::Consumed;
        }

        sockaddr_in mapped{};
        mapped.sin_family = AF_INET;
        mapped.sin_addr.s_addr = mapping.hostNetworkOrder;
        mapped.sin_port = htons(mapping.natPort);

        // Safe: IsCoordinator() pins the source. Keep passiveListener_ set for later peers.
        if (passiveListener_)
        {
            configuredTarget_ = mapped;
            configuredTarget_.sin_port = htons(mapping.internalPort);
            targetSet_ = true;
            targetPortKnown_ = true;
            targetInternalPort_ = mapping.internalPort;
        }
        activeTarget_ = mapped;
        assisted_ = true;
        ++stats_.mappingsReceived;
        SendDirectBurst(mapped, 4);
        if (outPeer != nullptr)
        {
            *outPeer = mapped;
        }
        char endpoint[64] = {};
        Log(
            "coordinator mapping internal=%u public=%s",
            mapping.internalPort,
            FormatEndpoint(mapped, endpoint, sizeof(endpoint)));
        return InboundResult::PeerMapped;
    }

    if (!IsDirectPunch(bytes, length))
    {
        return InboundResult::NotHandled;
    }
    if (passiveListener_)
    {
        // An unauthenticated one-byte datagram must not choose a peer.
        ++stats_.directPunchesReceived;
        return InboundResult::Consumed;
    }
    if (targetSet_ && source.sin_addr.s_addr != configuredTarget_.sin_addr.s_addr)
    {
        return InboundResult::NotHandled;
    }

    assisted_ = assisted_ || active_;
    ++stats_.directPunchesReceived;
    if (targetSet_ && outPeer != nullptr)
    {
        *outPeer = source;
        return InboundResult::PeerObserved;
    }
    return InboundResult::Consumed;
}

void Client::Log(const char* const format, ...) const
{
    if (logger_ == nullptr || format == nullptr)
    {
        return;
    }
    char message[320] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    logger_(loggerContext_, message);
}

}
