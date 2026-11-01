#include "net_session.h"

#include "autopunch_protocol.h"
#include "session_log.h"
#include "util.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

namespace trial_caster
{
namespace
{

class WinSockInit
{
public:
    WinSockInit()
    {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    // Keep until ExitProcess: a resolver helper may outlive a cancelled caller.
    ~WinSockInit() = default;
};

WinSockInit g_winsock;

bool ResolveIpv4Target(
    const std::wstring& target,
    IN_ADDR* const out,
    const std::atomic<bool>* const cancel)
{
    if (out == nullptr || target.empty())
    {
        return false;
    }

    const std::wstring resolved = util::ResolveConnectHostIpv4(target, cancel);
    return !resolved.empty() && InetPtonW(AF_INET, resolved.c_str(), out) == 1;
}

}

const wchar_t* ConnectionMethodLabel(const ConnectionMethod method)
{
    return method == ConnectionMethod::AutoPunch ? L"AutoPunch" : L"Direct";
}

NetSession::NetSession(const CasterConfig& config)
    : config_(config)
    , packetResendCount_(config.packetResendCount)
{
    SessionLog::Step(
        L"net",
        L"NetSession created port=%u delay=%u target_fps=%u resend=%u",
        static_cast<unsigned>(config_.port),
        static_cast<unsigned>(config_.inputDelay),
        static_cast<unsigned>(config_.targetFps),
        static_cast<unsigned>(packetResendCount_));
}

void NetSession::SetLogger(NetLogFn logger)
{
    logger_ = std::move(logger);
}

void NetSession::AutopunchLog(void* const context, const char* const message)
{
    auto* const self = static_cast<NetSession*>(context);
    if (self == nullptr || message == nullptr)
    {
        return;
    }
    const std::wstring wide(message, message + std::strlen(message));
    self->Log(L"AutoPunch: %s", wide.c_str());
}

void NetSession::Log(const wchar_t* fmt, ...) const
{
    if (!logger_)
    {
        return;
    }
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    const int needed = _vscwprintf(fmt, copy);
    va_end(copy);
    if (needed <= 0)
    {
        va_end(args);
        return;
    }
    std::wstring msg(static_cast<std::size_t>(needed) + 1, L'\0');
    vswprintf_s(msg.data(), msg.size(), fmt, args);
    va_end(args);
    msg.resize(static_cast<std::size_t>(needed));
    SessionLog::Info(L"net", L"%s", msg.c_str());
    if (logger_)
    {
        logger_(msg);
    }
}

void NetSession::Close()
{
    cancelled_.store(true);
    autopunch_.Detach();
    if (socket_ != INVALID_SOCKET)
    {
        SessionLog::Step(L"net", L"Close socket");
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    hasPeer_ = false;
}

bool NetSession::SendTo(const sockaddr_in& address, const void* data, const std::size_t size, const std::uint8_t repeat)
{
    const char* bytes = static_cast<const char*>(data);
    for (std::uint8_t i = 0; i < std::max<std::uint8_t>(1, repeat); ++i)
    {
        for (;;)
        {
            const int sent = sendto(
                socket_,
                bytes,
                static_cast<int>(size),
                0,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address));
            if (sent == static_cast<int>(size))
            {
                break;
            }
            if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
            {
                Sleep(1);
                continue;
            }
            return false;
        }
    }
    return true;
}

bool NetSession::SendToPeer(const void* data, const std::size_t size)
{
    return hasPeer_ && SendTo(peer_, data, size, packetResendCount_);
}

int NetSession::ReceiveOnce(const DWORD timeoutMs, sockaddr_in* source)
{
    fd_set set{};
    FD_ZERO(&set);
    FD_SET(socket_, &set);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeoutMs / 1000);
    tv.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);
    const int ready = select(0, &set, nullptr, nullptr, &tv);
    if (ready <= 0)
    {
        return ready;
    }

    sockaddr_in from{};
    int fromLen = sizeof(from);
    const int received = recvfrom(
        socket_,
        reinterpret_cast<char*>(rx_.data()),
        proto::kMaxMessageSize,
        0,
        reinterpret_cast<sockaddr*>(&from),
        &fromLen);
    if (received == SOCKET_ERROR)
    {
        const int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEMSGSIZE)
        {
            return 0;
        }
        return -1;
    }
    if (source != nullptr)
    {
        *source = from;
    }
    return received;
}

bool NetSession::IsPeer(const sockaddr_in& address) const
{
    return hasPeer_
        && peer_.sin_family == address.sin_family
        && peer_.sin_port == address.sin_port
        && peer_.sin_addr.S_un.S_addr == address.sin_addr.S_un.S_addr;
}

bool NetSession::IsThirdPartyConnect(const int bytes) const
{
    if (bytes != sizeof(proto::ConnectionRequest))
    {
        return false;
    }
    const auto& req = *reinterpret_cast<const proto::ConnectionRequest*>(rx_.data());
    return req.controlWord == proto::kActionConnect && req.identifier == proto::kTagEfz;
}

bool NetSession::IsSpectatorQuery(const int bytes) const
{
    if (bytes != sizeof(proto::ConnectionRequest))
    {
        return false;
    }
    const auto& request = *reinterpret_cast<const proto::ConnectionRequest*>(rx_.data());
    return request.controlWord == proto::kActionSpectatorQuery
        && request.identifier == proto::kTagEfz;
}

bool NetSession::HandleSpectatorQuery(
    const int bytes,
    const sockaddr_in& source,
    const bool occupied)
{
    if (!IsSpectatorQuery(bytes))
    {
        return false;
    }

    const auto& request = *reinterpret_cast<const proto::ConnectionRequest*>(rx_.data());
    if (request.majorVersion != proto::kVerMajor || request.minorVersion != proto::kVerMinor)
    {
        SessionLog::Warn(
            L"net",
            L"spectator query version mismatch peer=%u.%u local=%u.%u",
            request.majorVersion,
            request.minorVersion,
            proto::kVerMajor,
            proto::kVerMinor);
        proto::VersionMismatchPacket mismatch{};
        SendTo(source, &mismatch, sizeof(mismatch), packetResendCount_);
        return true;
    }

    SessionLog::Step(
        L"net",
        L"spectator query from %s host_state=%s",
        IpToString(source.sin_addr).c_str(),
        occupied ? L"occupied" : L"waiting");
    if (occupied)
    {
        SendHostStarting(source);
    }
    else
    {
        SendHostWaiting(source);
    }
    return true;
}

void NetSession::SendAlreadyInPlay(const sockaddr_in& destination)
{
    proto::AlreadyInPlayPacket packet{};
    SendTo(destination, &packet, sizeof(packet), packetResendCount_);
}

void NetSession::SendHostWaiting(const sockaddr_in& destination)
{
    proto::HostWaitingPacket packet{};
    SendTo(destination, &packet, sizeof(packet), packetResendCount_);
}

void NetSession::SendHostStarting(const sockaddr_in& destination)
{
    proto::HostStartingPacket packet{};
    SendTo(destination, &packet, sizeof(packet), packetResendCount_);
}

std::wstring NetSession::IpToString(const in_addr& addr) const
{
    wchar_t buffer[64] = {};
    InetNtopW(AF_INET, const_cast<IN_ADDR*>(&addr), buffer, static_cast<DWORD>(std::size(buffer)));
    return buffer;
}

std::wstring NetSession::LocalIpString() const
{
    char host[256] = {};
    if (gethostname(host, sizeof(host)) != 0)
    {
        return L"127.0.0.1";
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &result) != 0 || result == nullptr)
    {
        return L"127.0.0.1";
    }
    auto* addr = reinterpret_cast<sockaddr_in*>(result->ai_addr);
    const std::wstring ip = IpToString(addr->sin_addr);
    freeaddrinfo(result);
    return ip;
}

void NetSession::FillNames(proto::ConnectionRequest& packet) const
{
    proto::CopyWideToField(config_.playerName, packet.playerName);
    proto::CopyWideToField(config_.location, packet.location);
    proto::CopyWideToField(proto::NetworkModeWireString(config_.useRollbackNetplay), packet.networkType);
}

void NetSession::FillNames(proto::AcceptPacket& packet) const
{
    proto::CopyWideToField(config_.playerName, packet.playerName);
    proto::CopyWideToField(config_.location, packet.location);
    proto::CopyWideToField(proto::NetworkModeWireString(config_.useRollbackNetplay), packet.networkType);
}

NetResult NetSession::HostHandshake(NetSessionInfo* outInfo)
{
    SessionLog::Step(L"net", L"HostHandshake begin");
    Close();
    if (config_.inputDelay > 20u || !proto::IsSupportedTargetFps(config_.targetFps))
    {
        SessionLog::Error(
            L"net",
            L"HostHandshake rejected local delay=%u target_fps=%u",
            static_cast<unsigned>(config_.inputDelay),
            static_cast<unsigned>(config_.targetFps));
        return NetResult::InvalidConfiguration;
    }
    cancelled_.store(false);
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET)
    {
        SessionLog::Error(L"net", L"HostHandshake socket() failed");
        return NetResult::SocketError;
    }

    u_long nonBlocking = 1;
    ioctlsocket(socket_, FIONBIO, &nonBlocking);

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(config_.port);
    local.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
    if (bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
    {
        SessionLog::Error(L"net", L"HostHandshake bind(%u) failed", static_cast<unsigned>(config_.port));
        Close();
        return NetResult::SocketError;
    }

    host_ = true;
    hasPeer_ = false;
    localIsP1_ = true;

    autopunch_.Attach(
        socket_,
        config_.port,
        nullptr,
        false,
        &NetSession::AutopunchLog,
        this);

    Log(
        L"Waiting on UDP %u (%s); direct first, AutoPunch %S:%u fallback after %u ms...",
        config_.port,
        LocalIpString().c_str(),
        trialnet::autopunch::kCoordinatorHost,
        trialnet::autopunch::kCoordinatorPort,
        trialnet::autopunch::kDirectAttemptMs);

    proto::ConnectionRequest request{};
    const DWORD listenStarted = timeGetTime();
    for (;;)
    {
        const DWORD now = timeGetTime();
        if (!autopunch_.IsActive()
            && now - listenStarted >= trialnet::autopunch::kDirectAttemptMs)
        {
            autopunch_.Start(now);
        }
        autopunch_.Service(now);

        sockaddr_in source{};
        const int bytes = ReceiveOnce(50, &source);
        if (bytes < 0)
        {
            Close();
            return NetResult::SocketError;
        }
        const auto punchResult = autopunch_.HandleInbound(
            rx_.data(),
            bytes > 0 ? static_cast<std::size_t>(bytes) : 0u,
            source,
            nullptr);
        if (punchResult != trialnet::autopunch::InboundResult::NotHandled)
        {
            continue;
        }
        if (bytes != sizeof(proto::ConnectionRequest))
        {
            continue;
        }

        if (HandleSpectatorQuery(bytes, source, false))
        {
            continue;
        }

        std::memcpy(&request, rx_.data(), sizeof(request));
        if (request.controlWord != proto::kActionConnect || request.identifier != proto::kTagEfz)
        {
            continue;
        }
        if (request.majorVersion != proto::kVerMajor || request.minorVersion != proto::kVerMinor)
        {
            SessionLog::Warn(
                L"net",
                L"HostHandshake rejected peer protocol %u.%u (need exact %u.%u)",
                request.majorVersion,
                request.minorVersion,
                proto::kVerMajor,
                proto::kVerMinor);
            proto::VersionMismatchPacket mismatch{};
            SendTo(source, &mismatch, sizeof(mismatch), packetResendCount_);
            continue;
        }

        const proto::NetSyncMode localMode = proto::LocalNetworkMode(config_.useRollbackNetplay);
        const proto::NetSyncMode peerMode = proto::ParseNetworkMode(request.networkType);
        if (!proto::NetworkModesMatch(localMode, peerMode))
        {
            SessionLog::Warn(
                L"net",
                L"HostHandshake rejected peer mode %s (local %s)",
                proto::NetworkModeLabel(peerMode),
                proto::NetworkModeLabel(localMode));
            proto::DeniedPacket denied{};
            SendTo(source, &denied, sizeof(denied), packetResendCount_);
            continue;
        }

        peer_ = source;
        hasPeer_ = true;
        Log(
            L"Accepted %s (%s) — you are %s",
            IpToString(source.sin_addr).c_str(),
            proto::FieldToWide(request.playerName).c_str(),
            localIsP1_ ? L"P1" : L"P2");
        MessageBeep(MB_OK);
        autopunch_.Stop("launcher peer connected");
        break;
    }

    proto::AcceptPacket accept{};
    accept.is1PSide = localIsP1_ ? 1 : 0;
    FillNames(accept);

    DWORD totalLatency = 0;
    DWORD highestLatency = 0;
    DWORD lastTime = timeGetTime();
    std::uint8_t order = 0;
    while (order < 9)
    {
        SendToPeer(&accept, sizeof(accept));

        sockaddr_in source{};
        const int bytes = ReceiveOnce(50, &source);
        if (bytes < 0)
        {
            Close();
            return NetResult::SocketError;
        }
        if (bytes == 0)
        {
            if (timeGetTime() - lastTime >= config_.maxTimeoutMs)
            {
                SessionLog::Warn(L"net", L"HostHandshake ping timeout");
                Close();
                return NetResult::Timeout;
            }
            continue;
        }
        if (!IsPeer(source))
        {
            if (!HandleSpectatorQuery(bytes, source, true) && IsThirdPartyConnect(bytes))
            {
                SendHostStarting(source);
            }
            continue;
        }
        if (bytes != sizeof(proto::PingPacket))
        {
            continue;
        }

        const auto& ping = *reinterpret_cast<const proto::PingPacket*>(rx_.data());
        if (ping.controlWord != proto::kActionPing || ping.packetOrder != order)
        {
            continue;
        }

        const DWORD now = timeGetTime();
        const DWORD elapsed = now - lastTime;
        totalLatency += elapsed;
        highestLatency = std::max(highestLatency, elapsed);
        lastTime = now;

        proto::ReplyPacket reply{};
        reply.packetOrder = order;
        SendToPeer(&reply, sizeof(reply));
        ++order;
    }

    const DWORD avg = (totalLatency - highestLatency) >> 3;
    Log(L"Average latency: %u ms", avg);

    proto::InitializationConfigurationPacket init{};
    init.frameSkipFreq = config_.inputDelay;
    init.maxRetryCount = config_.maxRetryCount;
    init.packetResendCount = config_.packetResendCount;
    init.maxTimeout = config_.maxTimeoutMs;
    init.capabilities = proto::LocalCapabilities(
        config_.useRollbackNetplay,
        config_.allowPracticeKeys);
    init.targetFps = config_.targetFps;

    const DWORD initStarted = timeGetTime();
    DWORD lastInitSend = 0u;
    const DWORD initTimeout = std::max<DWORD>(
        config_.maxTimeoutMs,
        trialnet::autopunch::kConnectTimeoutMs);
    bool initAcknowledged = false;
    std::uint8_t clientCapabilities = 0u;
    while (timeGetTime() - initStarted < initTimeout)
    {
        const DWORD now = timeGetTime();
        if (lastInitSend == 0u || now - lastInitSend >= 250u)
        {
            lastInitSend = now;
            SendToPeer(&init, sizeof(init));
        }

        sockaddr_in source{};
        const int bytes = ReceiveOnce(50, &source);
        if (bytes < 0)
        {
            Close();
            return NetResult::SocketError;
        }
        if (bytes == 0)
        {
            continue;
        }
        if (!IsPeer(source))
        {
            if (!HandleSpectatorQuery(bytes, source, true) && IsThirdPartyConnect(bytes))
            {
                SendHostStarting(source);
            }
            continue;
        }
        if (bytes == sizeof(proto::InitializationAckPacket))
        {
            const auto& ack = *reinterpret_cast<const proto::InitializationAckPacket*>(rx_.data());
            if (ack.controlWord == proto::kActionInitConfig && ack.accepted == 0u)
            {
                SessionLog::Warn(
                    L"net",
                    L"HostHandshake client rejected target_fps=%u",
                    static_cast<unsigned>(init.targetFps));
                Close();
                return NetResult::InvalidConfiguration;
            }
            if (ack.controlWord == proto::kActionInitConfig && ack.accepted == 1u)
            {
                clientCapabilities = ack.capabilities;
                initAcknowledged = true;
                break;
            }
        }
    }
    if (!initAcknowledged)
    {
        SessionLog::Warn(L"net", L"HostHandshake configuration ACK timeout");
        Close();
        return NetResult::Timeout;
    }

    const bool allowPracticeKeys = proto::PracticeKeysMutuallyEnabled(
        init.capabilities,
        clientCapabilities);
    Log(
        L"Practice shortcuts online: %s (local opt-in=%s, peer opt-in=%s)",
        allowPracticeKeys ? L"enabled" : L"disabled",
        config_.allowPracticeKeys ? L"yes" : L"no",
        (clientCapabilities & proto::kCapPracticeKeys) != 0u ? L"yes" : L"no");

    if (outInfo != nullptr)
    {
        outInfo->isHost = true;
        outInfo->localIsP1 = localIsP1_;
        outInfo->allowPracticeKeys = allowPracticeKeys;
        outInfo->inputDelay = config_.inputDelay;
        outInfo->targetFps = config_.targetFps;
        outInfo->peerPort = 0u; // the in-game host must learn the new client socket endpoint
        outInfo->peerIp = IpToString(peer_.sin_addr);
        outInfo->peerName = proto::FieldToWide(request.playerName);
        outInfo->averageLatencyMs = avg;
        outInfo->connectionMethod = autopunch_.WasAssisted()
            ? ConnectionMethod::AutoPunch
            : ConnectionMethod::Direct;
    }
    SessionLog::Step(
        L"net",
        L"HostHandshake ok peer=%s side=%s delay=%u target_fps=%u practice_keys=%s avgMs=%u via=%s",
        IpToString(peer_.sin_addr).c_str(),
        localIsP1_ ? L"P1" : L"P2",
        static_cast<unsigned>(config_.inputDelay),
        static_cast<unsigned>(config_.targetFps),
        allowPracticeKeys ? L"enabled" : L"disabled",
        avg,
        ConnectionMethodLabel(
            autopunch_.WasAssisted() ? ConnectionMethod::AutoPunch : ConnectionMethod::Direct));
    return NetResult::Ok;
}

NetResult NetSession::ConnectHandshake(
    const std::wstring& targetIp,
    NetSessionInfo* outInfo,
    const std::uint16_t targetPort)
{
    const std::uint16_t effectivePort = targetPort != 0u ? targetPort : config_.port;
    SessionLog::Step(
        L"net",
        L"ConnectHandshake begin target=%s:%u",
        targetIp.c_str(),
        static_cast<unsigned>(effectivePort));
    Close();
    cancelled_.store(false);
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET)
    {
        SessionLog::Error(L"net", L"ConnectHandshake socket() failed");
        return NetResult::SocketError;
    }

    u_long nonBlocking = 1;
    ioctlsocket(socket_, FIONBIO, &nonBlocking);

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    local.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
    if (bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
    {
        SessionLog::Error(L"net", L"ConnectHandshake ephemeral bind failed");
        Close();
        return NetResult::SocketError;
    }

    IN_ADDR addr{};
    if (!ResolveIpv4Target(targetIp, &addr, &cancelled_))
    {
        SessionLog::Error(L"net", L"ConnectHandshake could not resolve host %s", targetIp.c_str());
        const bool cancelled = cancelled_.load();
        Close();
        return cancelled ? NetResult::Cancelled : NetResult::InvalidDestination;
    }

    peer_ = {};
    peer_.sin_family = AF_INET;
    peer_.sin_port = htons(effectivePort);
    peer_.sin_addr = addr;
    hasPeer_ = true;
    host_ = false;

    sockaddr_in bound{};
    int boundLength = sizeof(bound);
    if (getsockname(socket_, reinterpret_cast<sockaddr*>(&bound), &boundLength) != 0
        || bound.sin_port == 0u)
    {
        SessionLog::Error(L"net", L"ConnectHandshake getsockname failed");
        Close();
        return NetResult::SocketError;
    }
    autopunch_.Attach(
        socket_,
        ntohs(bound.sin_port),
        &peer_,
        true,
        &NetSession::AutopunchLog,
        this);

    Log(
        L"Connecting directly to %s:%u; AutoPunch %S:%u fallback after %u ms...",
        targetIp.c_str(),
        effectivePort,
        trialnet::autopunch::kCoordinatorHost,
        trialnet::autopunch::kCoordinatorPort,
        trialnet::autopunch::kDirectAttemptMs);

    proto::ConnectionRequest request{};
    FillNames(request);
    const DWORD start = timeGetTime();
    DWORD lastRequestSend = 0u;
    const DWORD connectTimeout = std::max<DWORD>(
        config_.maxTimeoutMs,
        trialnet::autopunch::kConnectTimeoutMs);

    std::wstring hostName;
    for (;;)
    {
        const DWORD now = timeGetTime();
        if (now - start >= trialnet::autopunch::kDirectAttemptMs && !autopunch_.IsActive())
        {
            autopunch_.Start(now);
        }
        autopunch_.Service(now);
        if (lastRequestSend == 0u || now - lastRequestSend >= 250u)
        {
            lastRequestSend = now;
            SendToPeer(&request, sizeof(request));
        }

        sockaddr_in source{};
        const int bytes = ReceiveOnce(10, &source);
        if (bytes < 0)
        {
            Close();
            return NetResult::SocketError;
        }
        if (bytes == 0)
        {
            if (timeGetTime() - start >= connectTimeout)
            {
                Close();
                return NetResult::Timeout;
            }
            continue;
        }

        sockaddr_in mappedPeer{};
        const auto punchResult = autopunch_.HandleInbound(
            rx_.data(),
            static_cast<std::size_t>(bytes),
            source,
            &mappedPeer);
        if (punchResult == trialnet::autopunch::InboundResult::PeerMapped
            || punchResult == trialnet::autopunch::InboundResult::PeerObserved)
        {
            peer_ = mappedPeer;
            lastRequestSend = 0u;
            continue;
        }
        if (punchResult == trialnet::autopunch::InboundResult::Consumed || !IsPeer(source))
        {
            continue;
        }

        const auto* control = reinterpret_cast<const proto::PublicControlWord*>(rx_.data());
        if (control->controlWord == proto::kReplyInPlay)
        {
            SessionLog::Warn(L"net", L"ConnectHandshake host already in play");
            Close();
            return NetResult::AlreadyInPlay;
        }
        if (control->controlWord == proto::kReplyHostStarting)
        {
            SessionLog::Step(L"net", L"ConnectHandshake host has two players and is starting");
            Close();
            return NetResult::HostStarting;
        }
        if (control->controlWord == proto::kReplyVerNotMatch)
        {
            SessionLog::Warn(L"net", L"ConnectHandshake version mismatch");
            Close();
            return NetResult::VersionMismatch;
        }
        if (control->controlWord == proto::kReplyDenied)
        {
            SessionLog::Warn(L"net", L"ConnectHandshake denied (mode mismatch or host rejected)");
            Close();
            return NetResult::NetModeMismatch;
        }
        if (bytes == sizeof(proto::AcceptPacket) && control->controlWord == proto::kReplyAck)
        {
            const auto& accept = *reinterpret_cast<const proto::AcceptPacket*>(rx_.data());
            hostName = proto::FieldToWide(accept.playerName);
            if (accept.is1PSide != 1)
            {
                SessionLog::Warn(
                    L"net",
                    L"ConnectHandshake host is1PSide=%u (expected 1 — host should be P1)",
                    static_cast<unsigned>(accept.is1PSide));
            }
            localIsP1_ = false;
            if (accept.minorVersion != proto::kVerMinor)
            {
                SessionLog::Warn(
                    L"net",
                    L"ConnectHandshake host protocol minor %u (need exact %u)",
                    accept.minorVersion,
                    proto::kVerMinor);
                Close();
                return NetResult::VersionMismatch;
            }
            const proto::NetSyncMode localMode = proto::LocalNetworkMode(config_.useRollbackNetplay);
            const proto::NetSyncMode hostMode = proto::ParseNetworkMode(accept.networkType);
            if (!proto::NetworkModesMatch(localMode, hostMode))
            {
                SessionLog::Warn(
                    L"net",
                    L"ConnectHandshake host mode %s (local %s)",
                    proto::NetworkModeLabel(hostMode),
                    proto::NetworkModeLabel(localMode));
                Close();
                return NetResult::NetModeMismatch;
            }
            Log(
                L"Accepted by %s (%s) — you are %s",
                targetIp.c_str(),
                proto::FieldToWide(accept.playerName).c_str(),
                localIsP1_ ? L"P1" : L"P2");
            autopunch_.Stop("launcher peer connected");
            break;
        }
    }

    DWORD totalLatency = 0;
    DWORD highestLatency = 0;
    for (std::uint8_t order = 0; order < 9; ++order)
    {
        proto::PingPacket ping{};
        ping.packetOrder = order;
        const DWORD pingStart = timeGetTime();
        SendToPeer(&ping, sizeof(ping));
        for (;;)
        {
            sockaddr_in source{};
            const int bytes = ReceiveOnce(10, &source);
            if (bytes < 0)
            {
                Close();
                return NetResult::SocketError;
            }
            if (bytes == 0)
            {
                if (timeGetTime() - pingStart >= config_.maxTimeoutMs)
                {
                    Close();
                    return NetResult::Timeout;
                }
                continue;
            }
            if (!IsPeer(source))
            {
                continue;
            }
            if (bytes == sizeof(proto::ReplyPacket))
            {
                const auto& reply = *reinterpret_cast<const proto::ReplyPacket*>(rx_.data());
                if (reply.controlWord == proto::kReplyPing && reply.packetOrder == order)
                {
                    const DWORD elapsed = timeGetTime() - pingStart;
                    totalLatency += elapsed;
                    highestLatency = std::max(highestLatency, elapsed);
                    break;
                }
            }
        }
    }

    const DWORD avg = (totalLatency - highestLatency) >> 3;
    Log(L"Average latency: %u ms — waiting for host config...", avg);

    const DWORD configWaitStarted = timeGetTime();
    const DWORD configWaitTimeout = std::max<DWORD>(
        config_.maxTimeoutMs,
        trialnet::autopunch::kConnectTimeoutMs);
    for (;;)
    {
        sockaddr_in source{};
        const int bytes = ReceiveOnce(50, &source);
        if (bytes < 0)
        {
            Close();
            return NetResult::SocketError;
        }
        if (bytes == 0 || !IsPeer(source))
        {
            if (timeGetTime() - configWaitStarted >= configWaitTimeout)
            {
                SessionLog::Warn(L"net", L"ConnectHandshake configuration timeout");
                Close();
                return NetResult::Timeout;
            }
            continue;
        }
        const auto* control = reinterpret_cast<const proto::PublicControlWord*>(rx_.data());
        if (control->controlWord == proto::kActionCancel)
        {
            Close();
            return NetResult::Cancelled;
        }
        if (bytes == sizeof(proto::InitializationConfigurationPacket)
            && control->controlWord == proto::kActionInitConfig)
        {
            const auto& init = *reinterpret_cast<const proto::InitializationConfigurationPacket*>(rx_.data());
            if (init.frameSkipFreq > 20u || !proto::IsSupportedTargetFps(init.targetFps))
            {
                SessionLog::Warn(
                    L"net",
                    L"ConnectHandshake rejected host delay=%u target_fps=%u",
                    static_cast<unsigned>(init.frameSkipFreq),
                    static_cast<unsigned>(init.targetFps));
                proto::InitializationAckPacket rejected{};
                rejected.accepted = 0u;
                SendTo(peer_, &rejected, sizeof(rejected), 10u);
                Close();
                return NetResult::InvalidConfiguration;
            }
            const bool hostRollback = (init.capabilities & proto::kCapRollback) != 0u;
            if (hostRollback != config_.useRollbackNetplay)
            {
                SessionLog::Warn(
                    L"net",
                    L"ConnectHandshake host capabilities mismatch rollback=%d local=%d",
                    hostRollback ? 1 : 0,
                    config_.useRollbackNetplay ? 1 : 0);
                Close();
                return NetResult::NetModeMismatch;
            }
            proto::InitializationAckPacket ack{};
            ack.capabilities = proto::LocalCapabilities(
                config_.useRollbackNetplay,
                config_.allowPracticeKeys);
            // Not tied to the packet-repeat setting, which is valid at 1.
            SendTo(peer_, &ack, sizeof(ack), 10u);
            const bool allowPracticeKeys = proto::PracticeKeysMutuallyEnabled(
                init.capabilities,
                ack.capabilities);
            Log(
                L"Practice shortcuts online: %s (host opt-in=%s, local opt-in=%s)",
                allowPracticeKeys ? L"enabled" : L"disabled",
                (init.capabilities & proto::kCapPracticeKeys) != 0u ? L"yes" : L"no",
                config_.allowPracticeKeys ? L"yes" : L"no");
            if (outInfo != nullptr)
            {
                outInfo->isHost = false;
                outInfo->localIsP1 = localIsP1_;
                outInfo->allowPracticeKeys = allowPracticeKeys;
                outInfo->inputDelay = init.frameSkipFreq;
                outInfo->targetFps = init.targetFps;
                // The DLL does its own lookup on a new socket, so keep the host's port as the key.
                outInfo->peerPort = effectivePort;
                // Session.ini feeds an IPv4 transport: never pass a DNS hostname through.
                outInfo->peerIp = IpToString(peer_.sin_addr);
                outInfo->peerName = hostName;
                outInfo->averageLatencyMs = avg;
                outInfo->connectionMethod = autopunch_.WasAssisted()
                    ? ConnectionMethod::AutoPunch
                    : ConnectionMethod::Direct;
            }
            SessionLog::Step(
                L"net",
                L"ConnectHandshake ok host=%s side=%s delay=%u target_fps=%u practice_keys=%s avgMs=%u via=%s peerPort=%u",
                targetIp.c_str(),
                localIsP1_ ? L"P1" : L"P2",
                static_cast<unsigned>(init.frameSkipFreq),
                static_cast<unsigned>(init.targetFps),
                allowPracticeKeys ? L"enabled" : L"disabled",
                avg,
                ConnectionMethodLabel(
                    autopunch_.WasAssisted() ? ConnectionMethod::AutoPunch : ConnectionMethod::Direct),
                static_cast<unsigned>(ntohs(peer_.sin_port)));
            return NetResult::Ok;
        }
    }
}

NetResult NetSession::ProbeSpectatorHost(
    const std::wstring& targetIp,
    const std::uint16_t targetPort)
{
    const std::uint16_t effectivePort = targetPort != 0u ? targetPort : config_.port;
    SessionLog::Step(
        L"spectator",
        L"ProbeSpectatorHost begin target=%s:%u",
        targetIp.c_str(),
        static_cast<unsigned>(effectivePort));
    Close();
    cancelled_.store(false);
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET)
    {
        return NetResult::SocketError;
    }

    u_long nonBlocking = 1u;
    ioctlsocket(socket_, FIONBIO, &nonBlocking);

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = 0u;
    local.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
    if (bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
    {
        Close();
        return NetResult::SocketError;
    }

    IN_ADDR address{};
    if (!ResolveIpv4Target(targetIp, &address, &cancelled_))
    {
        SessionLog::Error(L"spectator", L"Presence query could not resolve host %s", targetIp.c_str());
        const bool cancelled = cancelled_.load();
        Close();
        return cancelled ? NetResult::Cancelled : NetResult::InvalidDestination;
    }

    peer_ = {};
    peer_.sin_family = AF_INET;
    peer_.sin_port = htons(effectivePort);
    peer_.sin_addr = address;
    hasPeer_ = true;
    host_ = false;

    sockaddr_in bound{};
    int boundLength = sizeof(bound);
    if (getsockname(socket_, reinterpret_cast<sockaddr*>(&bound), &boundLength) != 0
        || bound.sin_port == 0u)
    {
        Close();
        return NetResult::SocketError;
    }
    autopunch_.Attach(
        socket_,
        ntohs(bound.sin_port),
        &peer_,
        true,
        &NetSession::AutopunchLog,
        this);

    SessionLog::Step(
        L"spectator",
        L"presence query direct-first target=%s:%u local=%u assistance_after=%u",
        targetIp.c_str(),
        static_cast<unsigned>(effectivePort),
        static_cast<unsigned>(ntohs(bound.sin_port)),
        trialnet::autopunch::kDirectAttemptMs);
    Log(L"Looking for a match at %s. Connection assistance will start automatically if needed.",
        targetIp.c_str());

    proto::ConnectionRequest query{};
    query.controlWord = proto::kActionSpectatorQuery;
    FillNames(query);
    const DWORD started = timeGetTime();
    DWORD lastSendMs = 0u;
    const DWORD timeoutMs = std::max<DWORD>(
        config_.maxTimeoutMs,
        trialnet::autopunch::kConnectTimeoutMs);

    for (;;)
    {
        const DWORD now = timeGetTime();
        if (now - started >= trialnet::autopunch::kDirectAttemptMs && !autopunch_.IsActive())
        {
            autopunch_.Start(now);
        }
        autopunch_.Service(now);
        if (lastSendMs == 0u || now - lastSendMs >= 250u)
        {
            lastSendMs = now;
            SendToPeer(&query, sizeof(query));
        }

        sockaddr_in source{};
        const int bytes = ReceiveOnce(10u, &source);
        if (bytes < 0)
        {
            Close();
            return NetResult::SocketError;
        }
        if (bytes == 0)
        {
            if (timeGetTime() - started >= timeoutMs)
            {
                Close();
                return NetResult::Timeout;
            }
            continue;
        }

        sockaddr_in mappedPeer{};
        const auto punchResult = autopunch_.HandleInbound(
            rx_.data(),
            static_cast<std::size_t>(bytes),
            source,
            &mappedPeer);
        if (punchResult == trialnet::autopunch::InboundResult::PeerMapped
            || punchResult == trialnet::autopunch::InboundResult::PeerObserved)
        {
            peer_ = mappedPeer;
            lastSendMs = 0u;
            continue;
        }
        if (punchResult == trialnet::autopunch::InboundResult::Consumed || !IsPeer(source))
        {
            continue;
        }

        const auto* control = reinterpret_cast<const proto::PublicControlWord*>(rx_.data());
        if (control->controlWord == proto::kReplyInPlay)
        {
            SessionLog::Step(L"spectator", L"presence query result=active target=%s", targetIp.c_str());
            Close();
            return NetResult::AlreadyInPlay;
        }
        if (control->controlWord == proto::kReplyHostWaiting)
        {
            SessionLog::Step(L"spectator", L"presence query result=waiting target=%s", targetIp.c_str());
            Close();
            return NetResult::HostWaiting;
        }
        if (control->controlWord == proto::kReplyHostStarting)
        {
            SessionLog::Step(L"spectator", L"presence query result=starting target=%s", targetIp.c_str());
            Close();
            return NetResult::HostStarting;
        }
        if (control->controlWord == proto::kReplyVerNotMatch)
        {
            Close();
            return NetResult::VersionMismatch;
        }
        if (control->controlWord == proto::kReplyDenied)
        {
            Close();
            return NetResult::Denied;
        }
    }
}

bool NetSession::SendLaunchGo(
    const std::uint8_t inputDelay,
    const std::uint8_t targetFps,
    const std::uint32_t launchAtMs)
{
    if (!hasPeer_)
    {
        SessionLog::Warn(L"net", L"SendLaunchGo denied (no peer)");
        return false;
    }
    if (inputDelay > 20u || !proto::IsSupportedTargetFps(targetFps)
        || launchAtMs != proto::kLaunchSyncLeadMs)
    {
        SessionLog::Error(
            L"net",
            L"SendLaunchGo rejected delay=%u target_fps=%u",
            static_cast<unsigned>(inputDelay),
            static_cast<unsigned>(targetFps));
        return false;
    }

    proto::LaunchGoPacket packet{};
    packet.inputDelay = inputDelay;
    packet.launchAtMs = launchAtMs;
    packet.targetFps = targetFps;
    for (int i = 0; i < 10; ++i)
    {
        if (!SendToPeer(&packet, sizeof(packet)))
        {
            SessionLog::Warn(L"net", L"SendLaunchGo socket send failed");
            return false;
        }
        Sleep(20);
    }
    Log(
        L"Sent launch-go to peer (delay=%u target_fps=%u launchAt=%u)",
        static_cast<unsigned>(inputDelay),
        static_cast<unsigned>(targetFps),
        launchAtMs);
    return true;
}

NetResult NetSession::WaitForLaunchGo(
    std::uint8_t* outDelay,
    std::uint8_t* outTargetFps,
    std::uint32_t* outLaunchAtMs,
    const DWORD timeoutMs)
{
    SessionLog::Step(L"net", L"WaitForLaunchGo timeoutMs=%lu", static_cast<unsigned long>(timeoutMs));
    if (!hasPeer_)
    {
        SessionLog::Warn(L"net", L"WaitForLaunchGo denied (no peer)");
        return NetResult::Denied;
    }

    const DWORD start = timeGetTime();
    while (timeGetTime() - start < timeoutMs)
    {
        sockaddr_in source{};
        const int bytes = ReceiveOnce(50, &source);
        if (bytes < 0)
        {
            Sleep(10);
            continue;
        }
        if (bytes == 0)
        {
            continue;
        }
        if (!IsPeer(source))
        {
            if (IsThirdPartyConnect(bytes))
            {
                SendAlreadyInPlay(source);
            }
            continue;
        }
        if (bytes >= static_cast<int>(sizeof(proto::PublicControlWord)))
        {
            const auto& control = *reinterpret_cast<const proto::PublicControlWord*>(rx_.data());
            if (control.controlWord == proto::kActionLaunchGo)
            {
                if (bytes != static_cast<int>(sizeof(proto::LaunchGoPacket)))
                {
                    SessionLog::Warn(
                        L"net",
                        L"WaitForLaunchGo rejected invalid packet size=%d expected=%zu",
                        bytes,
                        sizeof(proto::LaunchGoPacket));
                    return NetResult::InvalidConfiguration;
                }
                const auto& packet = *reinterpret_cast<const proto::LaunchGoPacket*>(rx_.data());
                if (!proto::IsValidLaunchGoPacket(packet))
                {
                    SessionLog::Warn(
                        L"net",
                        L"WaitForLaunchGo rejected delay=%u target_fps=%u reserved=%u,%u",
                        static_cast<unsigned>(packet.inputDelay),
                        static_cast<unsigned>(packet.targetFps),
                        static_cast<unsigned>(packet.reserved[0]),
                        static_cast<unsigned>(packet.reserved[1]));
                    return NetResult::InvalidConfiguration;
                }
                if (outDelay != nullptr)
                {
                    *outDelay = packet.inputDelay;
                }
                if (outTargetFps != nullptr)
                {
                    *outTargetFps = packet.targetFps;
                }
                if (outLaunchAtMs != nullptr)
                {
                    *outLaunchAtMs = packet.launchAtMs;
                }
                Log(L"Received launch-go from host");
                SessionLog::Step(
                    L"net",
                    L"WaitForLaunchGo ok delay=%u target_fps=%u launchAt=%u",
                    static_cast<unsigned>(packet.inputDelay),
                    static_cast<unsigned>(packet.targetFps),
                    packet.launchAtMs);
                return NetResult::Ok;
            }
        }
    }
    SessionLog::Warn(L"net", L"WaitForLaunchGo timeout");
    return NetResult::Timeout;
}

NetResult NetSession::ServiceOccupiedHost(const DWORD timeoutMs)
{
    if (socket_ == INVALID_SOCKET || !host_ || !hasPeer_)
    {
        return NetResult::Denied;
    }

    const DWORD started = timeGetTime();
    do
    {
        const DWORD now = timeGetTime();
        if (!autopunch_.IsActive())
        {
            autopunch_.Start(now);
        }
        autopunch_.Service(now);

        sockaddr_in source{};
        const DWORD elapsed = timeGetTime() - started;
        const DWORD remaining = elapsed < timeoutMs ? timeoutMs - elapsed : 0u;
        const int bytes = ReceiveOnce(std::min<DWORD>(remaining, 50u), &source);
        if (bytes < 0)
        {
            return NetResult::SocketError;
        }
        if (bytes == 0 || IsPeer(source))
        {
            continue;
        }
        const auto punchResult = autopunch_.HandleInbound(
            rx_.data(),
            static_cast<std::size_t>(bytes),
            source,
            nullptr);
        if (punchResult != trialnet::autopunch::InboundResult::NotHandled)
        {
            continue;
        }
        if (HandleSpectatorQuery(bytes, source, true))
        {
            return NetResult::Ok;
        }
        if (IsThirdPartyConnect(bytes))
        {
            SessionLog::Step(
                L"net",
                L"occupied host replied host-starting to %s",
                IpToString(source.sin_addr).c_str());
            SendHostStarting(source);
            return NetResult::Ok;
        }
    } while (timeGetTime() - started < timeoutMs);

    return NetResult::Ok;
}

}
