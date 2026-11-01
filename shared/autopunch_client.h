#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstddef>
#include <cstdint>

namespace trialnet::autopunch
{

enum class InboundResult
{
    NotHandled,
    Consumed,
    PeerObserved,
    PeerMapped,
};

struct Stats
{
    std::uint32_t registersSent = 0;
    std::uint32_t lookupsSent = 0;
    std::uint32_t directPunchesSent = 0;
    std::uint32_t directPunchesReceived = 0;
    std::uint32_t mappingsReceived = 0;
};

using LogCallback = void (*)(void* context, const char* message);

class Client
{
public:
    // Caller owns the socket. targetPortKnown=false is the host/listen case.
    bool Attach(
        SOCKET socket,
        std::uint16_t localPort,
        const sockaddr_in* target,
        bool targetPortKnown,
        LogCallback logger = nullptr,
        void* loggerContext = nullptr);
    void Detach();

    void Start(std::uint32_t nowMs);
    void Stop(const char* reason);
    void Service(std::uint32_t nowMs);

    InboundResult HandleInbound(
        const void* bytes,
        std::size_t length,
        const sockaddr_in& source,
        sockaddr_in* outPeer);

    bool IsActive() const { return active_; }
    bool WasAssisted() const { return assisted_; }
    const Stats& GetStats() const { return stats_; }

private:
    bool ResolveCoordinator(std::uint32_t nowMs);
    bool IsCoordinator(const sockaddr_in& source) const;
    bool SendRaw(const sockaddr_in& destination, const void* bytes, std::size_t length);
    bool SendRegister();
    bool SendLookup();
    int SendDirectBurst(const sockaddr_in& destination, int count);
    void Log(const char* format, ...) const;

    SOCKET socket_ = INVALID_SOCKET;
    sockaddr_in coordinator_{};
    sockaddr_in configuredTarget_{};
    sockaddr_in activeTarget_{};
    std::uint16_t localPort_ = 0;
    std::uint16_t targetInternalPort_ = 0;
    bool targetSet_ = false;
    bool targetPortKnown_ = false;
    bool passiveListener_ = false;
    bool coordinatorResolved_ = false;
    bool active_ = false;
    bool assisted_ = false;
    std::uint32_t startedMs_ = 0;
    std::uint32_t lastResolveMs_ = 0;
    std::uint32_t lastRegisterMs_ = 0;
    std::uint32_t lastLookupMs_ = 0;
    std::uint32_t lastDirectMs_ = 0;
    LogCallback logger_ = nullptr;
    void* loggerContext_ = nullptr;
    Stats stats_{};
};

}
