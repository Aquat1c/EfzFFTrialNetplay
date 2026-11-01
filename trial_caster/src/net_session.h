#pragma once

#include "autopunch_client.h"
#include "caster_config.h"
#include "protocol.h"

#include <winsock2.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace trial_caster
{

enum class NetResult
{
    Ok,
    Cancelled,
    Timeout,
    SocketError,
    InvalidDestination,
    VersionMismatch,
    AlreadyInPlay,
    Denied,
    RollbackRequired,
    NetModeMismatch,
    HostWaiting,
    HostStarting,
    InvalidConfiguration,
};

enum class ConnectionMethod
{
    Direct,
    AutoPunch,
};

const wchar_t* ConnectionMethodLabel(ConnectionMethod method);

struct NetSessionInfo
{
    bool isHost = false;
    bool localIsP1 = true;
    bool allowPracticeKeys = false;
    std::uint8_t inputDelay = 1;
    std::uint8_t targetFps = proto::kDefaultTargetFps;
    std::uint16_t peerPort = 0;
    std::wstring peerIp;
    std::wstring peerName;
    std::uint32_t averageLatencyMs = 0;
    ConnectionMethod connectionMethod = ConnectionMethod::Direct;
};

using NetLogFn = std::function<void(const std::wstring&)>;

class NetSession
{
public:
    explicit NetSession(const CasterConfig& config);

    void SetLogger(NetLogFn logger);

    NetResult HostHandshake(NetSessionInfo* outInfo);
    NetResult ConnectHandshake(
        const std::wstring& targetIp,
        NetSessionInfo* outInfo,
        std::uint16_t targetPort = 0u);
    NetResult ProbeSpectatorHost(
        const std::wstring& targetIp,
        std::uint16_t targetPort = 0u);
    bool SendLaunchGo(
        std::uint8_t inputDelay,
        std::uint8_t targetFps,
        std::uint32_t launchAtMs);
    NetResult WaitForLaunchGo(
        std::uint8_t* outDelay,
        std::uint8_t* outTargetFps,
        std::uint32_t* outLaunchAtMs,
        DWORD timeoutMs);
    NetResult ServiceOccupiedHost(DWORD timeoutMs);
    void Close();

private:
    bool SendToPeer(const void* data, std::size_t size);
    bool SendTo(const sockaddr_in& address, const void* data, std::size_t size, std::uint8_t repeat);
    int ReceiveOnce(DWORD timeoutMs, sockaddr_in* source);
    void Log(const wchar_t* fmt, ...) const;
    void FillNames(proto::ConnectionRequest& packet) const;
    void FillNames(proto::AcceptPacket& packet) const;
    bool IsPeer(const sockaddr_in& address) const;
    bool IsThirdPartyConnect(int bytes) const;
    bool IsSpectatorQuery(int bytes) const;
    bool HandleSpectatorQuery(int bytes, const sockaddr_in& source, bool occupied);
    void SendAlreadyInPlay(const sockaddr_in& destination);
    void SendHostWaiting(const sockaddr_in& destination);
    void SendHostStarting(const sockaddr_in& destination);
    std::wstring IpToString(const in_addr& addr) const;
    std::wstring LocalIpString() const;
    static void AutopunchLog(void* context, const char* message);

    const CasterConfig& config_;
    NetLogFn logger_;
    SOCKET socket_ = INVALID_SOCKET;
    sockaddr_in peer_{};
    bool hasPeer_ = false;
    bool host_ = false;
    bool localIsP1_ = true;
    std::uint8_t packetResendCount_ = proto::kMaxOverlappedPacket;
    std::array<std::uint8_t, 65536> rx_{};
    trialnet::autopunch::Client autopunch_{};
    std::atomic<bool> cancelled_{false};
};

}
