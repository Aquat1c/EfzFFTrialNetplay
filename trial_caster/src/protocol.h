#pragma once

#include "trial_compatibility.h"

#include <array>
#include <cstdint>
#include <string>

namespace trial_caster::proto
{

constexpr std::uint8_t kActionConnect = 0;
constexpr std::uint8_t kActionCancel = 1;
constexpr std::uint8_t kActionPing = 2;
constexpr std::uint8_t kActionInitConfig = 3;
constexpr std::uint8_t kActionExchangeRandomSeed = 4;
constexpr std::uint8_t kActionSyncData = 5;
constexpr std::uint8_t kActionQuit = 6;
constexpr std::uint8_t kActionLaunchGo = 7;
// Same 201-byte envelope as ConnectionRequest, but reserves no P2 slot.
constexpr std::uint8_t kActionSpectatorQuery = 8;

constexpr std::uint8_t kReplyAck = 0;
constexpr std::uint8_t kReplyInPlay = 1;
constexpr std::uint8_t kReplyPing = 2;
constexpr std::uint8_t kReplyVerNotMatch = 3;
constexpr std::uint8_t kReplyDenied = 4;
constexpr std::uint8_t kReplyHostWaiting = 5;
// Only the injected host reports kReplyInPlay.
constexpr std::uint8_t kReplyHostStarting = 6;

constexpr std::uint16_t kVerMajor = trialnet::compat::kLauncherProtocolMajor;
constexpr std::uint16_t kVerMinor = trialnet::compat::kCompatibilityVersion;

constexpr std::uint8_t kCapRollback = 0x01u;
constexpr std::uint8_t kCapLockstep = 0x02u;
constexpr std::uint8_t kCapPracticeKeys = 0x04u;

constexpr std::uint8_t kDefaultTargetFps = 60u;
constexpr std::uint8_t kMaximumTargetFps = 64u;

constexpr bool IsSupportedTargetFps(const std::uint8_t targetFps)
{
    return targetFps == kDefaultTargetFps || targetFps == kMaximumTargetFps;
}

constexpr std::uint8_t LocalCapabilities(
    const bool useRollbackNetplay,
    const bool allowPracticeKeys)
{
    return static_cast<std::uint8_t>(
        (useRollbackNetplay ? kCapRollback : kCapLockstep)
        | (allowPracticeKeys ? kCapPracticeKeys : 0u));
}

constexpr bool PracticeKeysMutuallyEnabled(
    const std::uint8_t hostCapabilities,
    const std::uint8_t clientCapabilities)
{
    return (hostCapabilities & kCapPracticeKeys) != 0u
        && (clientCapabilities & kCapPracticeKeys) != 0u;
}

enum class NetSyncMode : std::uint8_t
{
    Lockstep = 0,
    Rollback = 1,
};

constexpr int kMaxMessageSize = 256;
constexpr int kMaxRetryCount = 99;
constexpr int kMaxOverlappedPacket = 9;
constexpr int kMaxQueueSlot = 10;
constexpr int kMaxTimeoutMs = 2000;
constexpr std::uint32_t kTagEfz = 0x45465A4Eu; // 'EFZN'

#pragma pack(push, 1)

struct PlayerInputRecord
{
    std::uint8_t playerAction = 0;
    std::uint8_t systemState = 0;
};

struct ConnectionRequest
{
    std::uint8_t controlWord = kActionConnect;
    std::uint32_t identifier = kTagEfz;
    std::uint16_t majorVersion = kVerMajor;
    std::uint16_t minorVersion = kVerMinor;
    std::array<std::uint16_t, 32> playerName{};
    std::array<std::uint16_t, 32> location{};
    std::array<std::uint16_t, 32> networkType{};
};

struct AcceptPacket
{
    std::uint8_t controlWord = kReplyAck;
    std::uint8_t is1PSide = 0;
    std::uint16_t minorVersion = kVerMinor;
    std::array<std::uint16_t, 32> playerName{};
    std::array<std::uint16_t, 32> location{};
    std::array<std::uint16_t, 32> networkType{};
};

struct PingPacket
{
    std::uint8_t controlWord = kActionPing;
    std::uint8_t packetOrder = 0;
};

struct ReplyPacket
{
    std::uint8_t controlWord = kReplyPing;
    std::uint8_t packetOrder = 0;
};

struct VersionMismatchPacket
{
    std::uint8_t controlWord = kReplyVerNotMatch;
};

struct DeniedPacket
{
    std::uint8_t controlWord = kReplyDenied;
};

struct AlreadyInPlayPacket
{
    std::uint8_t controlWord = kReplyInPlay;
};

struct HostWaitingPacket
{
    std::uint8_t controlWord = kReplyHostWaiting;
};

struct HostStartingPacket
{
    std::uint8_t controlWord = kReplyHostStarting;
};

struct InitializationConfigurationPacket
{
    std::uint8_t controlWord = kActionInitConfig;
    std::uint8_t frameSkipFreq = 1;
    std::uint8_t maxRetryCount = kMaxRetryCount;
    std::uint8_t packetResendCount = kMaxOverlappedPacket;
    std::uint16_t maxTimeout = kMaxTimeoutMs;
    std::uint8_t capabilities = kCapRollback;
    std::uint8_t targetFps = kDefaultTargetFps;
};

struct InitializationAckPacket
{
    std::uint8_t controlWord = kActionInitConfig;
    std::uint8_t accepted = 1;
    std::uint8_t capabilities = 0u;
};

struct PublicControlWord
{
    std::uint8_t controlWord = 0;
};

// Must be non-zero: the injected runtime reserves zero to keep phases apart.
constexpr std::uint32_t kLaunchSyncLeadMs = 2000u;

struct LaunchGoPacket
{
    std::uint8_t controlWord = kActionLaunchGo;
    std::uint8_t inputDelay = 1;
    std::uint32_t launchAtMs = kLaunchSyncLeadMs; // countdown from first send/receive
    std::uint8_t targetFps = kDefaultTargetFps;
    // Seven and eight bytes collide with datagram-size dispatch; keep LaunchGo at nine.
    std::array<std::uint8_t, 2> reserved{};
};

struct ExchangeRandomSeedPacket
{
    std::uint8_t controlWord = kActionExchangeRandomSeed;
    std::uint32_t seed = 0;
};

#pragma pack(pop)

static_assert(sizeof(ConnectionRequest) == 201);
static_assert(sizeof(HostWaitingPacket) == 1);
static_assert(sizeof(HostStartingPacket) == 1);
static_assert(sizeof(AcceptPacket) == 196);
static_assert(sizeof(InitializationConfigurationPacket) == 8);
static_assert(sizeof(InitializationAckPacket) == 3);
static_assert(sizeof(LaunchGoPacket) == 9);

constexpr bool IsValidLaunchGoPacket(const LaunchGoPacket& packet)
{
    return packet.controlWord == kActionLaunchGo
        && packet.inputDelay <= 20u
        && packet.launchAtMs == kLaunchSyncLeadMs
        && IsSupportedTargetFps(packet.targetFps)
        && packet.reserved[0] == 0u
        && packet.reserved[1] == 0u;
}

bool PeerRequiresRollback(const std::array<std::uint16_t, 32>& networkTypeField);
NetSyncMode ParseNetworkMode(const std::array<std::uint16_t, 32>& networkTypeField);
NetSyncMode LocalNetworkMode(bool useRollbackNetplay);
std::wstring NetworkModeWireString(bool useRollbackNetplay);
const wchar_t* NetworkModeLabel(NetSyncMode mode);
bool NetworkModesMatch(NetSyncMode local, NetSyncMode peer);
static_assert(sizeof(ExchangeRandomSeedPacket) == 5);

void CopyWideToField(const std::wstring& src, std::array<std::uint16_t, 32>& dest);
std::wstring FieldToWide(const std::array<std::uint16_t, 32>& src);

}
