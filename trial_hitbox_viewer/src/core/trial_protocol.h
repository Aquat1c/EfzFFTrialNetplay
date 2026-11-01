#pragma once

#include "trial_compatibility.h"

#include <array>
#include <cstdint>

// Player wire format; launcher counterpart in trial_caster/src/protocol.h.
namespace thv::proto
{

constexpr std::uint8_t kActionExchangeRandomSeed = 4;
constexpr std::uint8_t kActionSyncData = 5;
constexpr std::uint8_t kActionQuit = 6;
constexpr std::uint8_t kActionLaunchGo = 7;
constexpr std::uint8_t kActionRollbackReady = 8;
constexpr std::uint8_t kActionFrameHash = 9;
constexpr std::uint8_t kActionBattleEntry = 10;
// Never part of input ACKs, prediction or liveness.
constexpr std::uint8_t kActionSpectatorNodeAdvert = 11;
constexpr std::uint8_t kActionSpectatorGrant = 12;
constexpr std::uint8_t kActionSpectatorGrantAck = 13;
constexpr std::uint8_t kActionSpectatorClaim = 14;

constexpr std::uint8_t kRollbackWireVersion =
    static_cast<std::uint8_t>(trialnet::compat::kCompatibilityVersion);
constexpr std::uint8_t kRollbackFlagEnabled = 0x01u;
constexpr std::uint8_t kRollbackFlagAck = 0x02u;

// Covers delay 0..20 + rollback 0..20 under loss; packet stays below 256 B.
constexpr int kMaxQueueSlot = 64;
constexpr std::uint8_t kMaxInputDelay = 20u;
constexpr std::uint8_t kMaxRollbackFrames = 20u;
constexpr std::uint8_t kDefaultTargetFps = 60u;
constexpr std::uint8_t kMaximumTargetFps = 64u;

constexpr bool IsSupportedTargetFps(const std::uint8_t targetFps)
{
    return targetFps == kDefaultTargetFps || targetFps == kMaximumTargetFps;
}

constexpr std::uint64_t MakeRuntimeSessionId(
    const std::uint32_t clientNonce,
    const std::uint32_t hostNonce,
    const std::uint32_t seed)
{
    if (clientNonce == 0u || hostNonce == 0u || seed == 0u)
    {
        return 0u;
    }
    std::uint64_t value = (static_cast<std::uint64_t>(clientNonce) << 32u)
        | static_cast<std::uint64_t>(hostNonce);
    value ^= static_cast<std::uint64_t>(seed) + 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27u)) * 0x94D049BB133111EBull;
    value ^= value >> 31u;
    return value != 0u ? value : 0x45465A4E4554504Cull; // "EFZNETPL"
}

constexpr std::uint8_t kSpectatorNodeCanServe = 0x01u;
constexpr std::uint8_t kSpectatorNodeCoordinator = 0x02u;

enum class SpectatorGrantStatus : std::uint8_t
{
    Ready = 1u,
    ArchiveMismatch = 2u,
    CapacityUnavailable = 3u,
    InvalidGrant = 4u,
};

enum class QuitReason : std::uint8_t
{
    GracefulSessionEnd = 1u,
    PlayerDisconnected = 2u,
    ProtocolAbort = 3u,
};

constexpr std::uint8_t kQuitFlagAck = 0x01u;

#pragma pack(push, 1)
struct PlayerInputRecord
{
    std::uint8_t playerAction = 0;
    std::uint8_t systemState = 0;
};

struct SyncDataPacket
{
    std::uint8_t controlWord = kActionSyncData;
    // Incremented at each committed battle entry.
    std::uint16_t timeline = 0;
    std::uint32_t currentFrame = 0;
    // Receiver's contiguous input count: the real ACK.
    std::uint32_t ackFrame = 0;
    // Cumulative confirmed-state hash; UINT32_MAX = none confirmed yet this session.
    std::uint32_t confirmedHashFrame = UINT32_MAX;
    std::uint64_t confirmedHashChain = 0;
    std::array<PlayerInputRecord, kMaxQueueSlot> actionData{};
    std::uint64_t sessionId = 0u;
};

struct QuitPacket
{
    std::uint8_t controlWord = kActionQuit;
    QuitReason reason = QuitReason::ProtocolAbort;
    std::uint8_t flags = 0u;
    std::uint8_t wireVersion = kRollbackWireVersion;
    std::uint8_t reserved[3] = {};
    std::uint64_t sessionId = 0u;
};

enum class SeedHandshakeKind : std::uint8_t
{
    ClientHello = 1u,
    HostOffer = 2u,
    ClientAck = 3u,
    HostConfirm = 4u,
    ClientConfirmAck = 5u,
};

struct ExchangeRandomSeedPacket
{
    std::uint8_t controlWord = kActionExchangeRandomSeed;
    std::uint8_t wireVersion = kRollbackWireVersion;
    SeedHandshakeKind kind = SeedHandshakeKind::ClientHello;
    std::uint8_t reserved = 0u;
    std::uint32_t clientNonce = 0u;
    std::uint32_t hostNonce = 0u;
    std::uint32_t seed = 0;
};

struct LaunchGoPacket
{
    std::uint8_t controlWord = kActionLaunchGo;
    std::uint8_t inputDelay = 1;
    // Must be zero here; launcher datagrams use a non-zero lead.
    std::uint32_t launchAtMs = 0;
    std::uint8_t targetFps = kDefaultTargetFps;
    std::uint8_t reserved[2] = {};
    std::uint64_t sessionId = 0u;
};

struct RollbackReadyPacket
{
    std::uint8_t controlWord = kActionRollbackReady;
    std::uint8_t rollbackVersion = kRollbackWireVersion;
    std::uint8_t flags = kRollbackFlagEnabled;
    std::uint8_t maxRollback = 0;
    std::uint8_t queueSlots = static_cast<std::uint8_t>(kMaxQueueSlot);
    // Both peers must advertise the exact same supported value.
    std::uint8_t reserved[3] = {};
    std::uint64_t sessionId = 0u;
};

struct BattleEntryPacket
{
    std::uint8_t controlWord = kActionBattleEntry;
    std::uint8_t ready = 0;
    std::uint32_t epoch = 0;   // UINT32_MAX = armed handoff, else transport generation
    std::uint32_t frame = 0;   // aligned local frontier (sim == local == remote size)
    std::uint64_t sessionId = 0u;
};

constexpr std::uint32_t kBattleEntryEpochHandoff = UINT32_MAX;

constexpr int kFrameHashRegions = 7;
struct FrameHashPacket
{
    std::uint8_t controlWord = kActionFrameHash;
    std::uint8_t regionCount = kFrameHashRegions;
    std::uint16_t timeline = 0;
    std::uint32_t frame = 0;
    std::uint32_t hash[kFrameHashRegions] = {};
    std::uint64_t sessionId = 0u;
};

// internalPort is host byte order.
struct SpectatorNodeAdvertPacket
{
    std::uint8_t controlWord = kActionSpectatorNodeAdvert;
    std::uint8_t wireVersion = kRollbackWireVersion;
    std::uint8_t flags = 0u;
    std::uint8_t freeSlots = 0u;
    std::uint32_t relayEpoch = 0u;
    std::uint32_t gameId = 0u;
    std::uint32_t nextRecordIndex = 0u;
    std::uint64_t inputDigest = 0u;
    std::uint64_t gameStartFingerprint = 0u;
    std::uint16_t internalPort = 0u;
    std::uint8_t reserved[6] = {};
};

// Viewer IPv4 is network byte order, matching sockaddr_in::sin_addr.
struct SpectatorGrantPacket
{
    std::uint8_t controlWord = kActionSpectatorGrant;
    std::uint8_t wireVersion = kRollbackWireVersion;
    std::uint8_t hop = 1u;
    std::uint8_t reserved = 0u;
    std::uint32_t relayEpoch = 0u;
    std::uint32_t grantId = 0u;
    std::uint32_t clientNonce = 0u;
    std::uint32_t requestedGameId = 0u;
    std::uint32_t nextRecordIndex = 0u;
    std::uint32_t viewerIpv4 = 0u;
    std::uint32_t ttlMs = 0u;
    std::uint64_t expectedDigest = 0u;
    std::uint64_t gameStartFingerprint = 0u;
    std::array<std::uint8_t, 16> token{};
};

struct SpectatorGrantAckPacket
{
    std::uint8_t controlWord = kActionSpectatorGrantAck;
    std::uint8_t wireVersion = kRollbackWireVersion;
    SpectatorGrantStatus status = SpectatorGrantStatus::InvalidGrant;
    std::uint8_t freeSlots = 0u;
    std::uint32_t relayEpoch = 0u;
    std::uint32_t grantId = 0u;
    std::uint32_t clientNonce = 0u;
    std::uint32_t requestedGameId = 0u;
    std::uint32_t nextRecordIndex = 0u;
    std::uint64_t expectedDigest = 0u;
    std::uint64_t gameStartFingerprint = 0u;
    std::uint8_t reserved[4] = {};
};

struct SpectatorClaimPacket
{
    std::uint8_t controlWord = kActionSpectatorClaim;
    std::uint8_t wireVersion = kRollbackWireVersion;
    std::uint8_t hop = 1u;
    std::uint8_t reserved = 0u;
    std::uint32_t relayEpoch = 0u;
    std::uint32_t grantId = 0u;
    std::uint32_t clientNonce = 0u;
    std::uint32_t requestedGameId = 0u;
    std::uint32_t nextRecordIndex = 0u;
    std::uint64_t expectedDigest = 0u;
    std::array<std::uint8_t, 16> token{};
};
#pragma pack(pop)

constexpr bool IsClientRendezvousHello(const ExchangeRandomSeedPacket& packet)
{
    return packet.controlWord == kActionExchangeRandomSeed
        && packet.wireVersion == kRollbackWireVersion
        && packet.kind == SeedHandshakeKind::ClientHello
        && packet.reserved == 0u
        && packet.clientNonce != 0u
        && packet.hostNonce == 0u
        && packet.seed == 0u;
}

constexpr bool IsHostSeedOffer(
    const ExchangeRandomSeedPacket& packet,
    const std::uint32_t expectedClientNonce)
{
    return expectedClientNonce != 0u
        && packet.controlWord == kActionExchangeRandomSeed
        && packet.wireVersion == kRollbackWireVersion
        && packet.kind == SeedHandshakeKind::HostOffer
        && packet.reserved == 0u
        && packet.clientNonce == expectedClientNonce
        && packet.hostNonce != 0u
        && packet.seed != 0u;
}

constexpr bool IsClientSeedAck(
    const ExchangeRandomSeedPacket& packet,
    const std::uint32_t expectedClientNonce,
    const std::uint32_t expectedHostNonce,
    const std::uint32_t expectedSeed)
{
    return expectedClientNonce != 0u && expectedHostNonce != 0u && expectedSeed != 0u
        && packet.controlWord == kActionExchangeRandomSeed
        && packet.wireVersion == kRollbackWireVersion
        && packet.kind == SeedHandshakeKind::ClientAck
        && packet.reserved == 0u
        && packet.clientNonce == expectedClientNonce
        && packet.hostNonce == expectedHostNonce
        && packet.seed == expectedSeed;
}

constexpr bool IsHostSeedConfirm(
    const ExchangeRandomSeedPacket& packet,
    const std::uint32_t expectedClientNonce,
    const std::uint32_t expectedHostNonce,
    const std::uint32_t expectedSeed)
{
    return expectedClientNonce != 0u && expectedHostNonce != 0u && expectedSeed != 0u
        && packet.controlWord == kActionExchangeRandomSeed
        && packet.wireVersion == kRollbackWireVersion
        && packet.kind == SeedHandshakeKind::HostConfirm
        && packet.reserved == 0u
        && packet.clientNonce == expectedClientNonce
        && packet.hostNonce == expectedHostNonce
        && packet.seed == expectedSeed;
}

constexpr bool IsClientSeedConfirmAck(
    const ExchangeRandomSeedPacket& packet,
    const std::uint32_t expectedClientNonce,
    const std::uint32_t expectedHostNonce,
    const std::uint32_t expectedSeed)
{
    return expectedClientNonce != 0u && expectedHostNonce != 0u && expectedSeed != 0u
        && packet.controlWord == kActionExchangeRandomSeed
        && packet.wireVersion == kRollbackWireVersion
        && packet.kind == SeedHandshakeKind::ClientConfirmAck
        && packet.reserved == 0u
        && packet.clientNonce == expectedClientNonce
        && packet.hostNonce == expectedHostNonce
        && packet.seed == expectedSeed;
}

constexpr bool IsValidRollbackReadyContract(
    const RollbackReadyPacket& packet,
    const std::uint8_t expectedTargetFps,
    const bool requireAck,
    const std::uint64_t expectedSessionId)
{
    return expectedSessionId != 0u
        && packet.controlWord == kActionRollbackReady
        && packet.rollbackVersion == kRollbackWireVersion
        && (packet.flags & kRollbackFlagEnabled) != 0u
        && (packet.flags & ~(kRollbackFlagEnabled | kRollbackFlagAck)) == 0u
        && ((packet.flags & kRollbackFlagAck) != 0u) == requireAck
        && packet.maxRollback <= kMaxRollbackFrames
        && packet.queueSlots == static_cast<std::uint8_t>(kMaxQueueSlot)
        && IsSupportedTargetFps(packet.reserved[0])
        && packet.reserved[0] == expectedTargetFps
        && packet.reserved[1] == 0u
        && packet.reserved[2] == 0u
        && packet.sessionId == expectedSessionId;
}

constexpr bool IsValidLaunchGoContract(
    const LaunchGoPacket& packet,
    const std::uint8_t expectedTargetFps,
    const std::uint64_t expectedSessionId)
{
    return expectedSessionId != 0u
        && packet.controlWord == kActionLaunchGo
        && packet.inputDelay <= kMaxInputDelay
        && packet.launchAtMs == 0u
        && IsSupportedTargetFps(packet.targetFps)
        && packet.targetFps == expectedTargetFps
        && packet.reserved[0] == 0u
        && packet.reserved[1] == 0u
        && packet.sessionId == expectedSessionId;
}

static_assert(sizeof(PlayerInputRecord) == 2, "trial protocol packing mismatch");
static_assert(sizeof(SyncDataPacket) == 159, "trial protocol packing mismatch");
static_assert(sizeof(QuitPacket) == 15, "trial protocol packing mismatch");
static_assert(sizeof(LaunchGoPacket) == 17, "trial protocol packing mismatch");
static_assert(sizeof(RollbackReadyPacket) == 16, "trial protocol packing mismatch");
static_assert(sizeof(ExchangeRandomSeedPacket) == 16, "trial protocol packing mismatch");
static_assert(sizeof(FrameHashPacket) == 44, "trial protocol packing mismatch");
static_assert(sizeof(BattleEntryPacket) == 18, "trial protocol packing mismatch");
static_assert(sizeof(SpectatorNodeAdvertPacket) == 40u, "spectator node advert layout drift");
static_assert(sizeof(SpectatorGrantPacket) == 64u, "spectator grant layout drift");
static_assert(sizeof(SpectatorGrantAckPacket) == 44u, "spectator grant ack layout drift");
static_assert(sizeof(SpectatorClaimPacket) == 48u, "spectator claim layout drift");
static_assert(sizeof(SpectatorNodeAdvertPacket) != sizeof(SpectatorGrantPacket), "size-dispatch collision");
static_assert(sizeof(SpectatorNodeAdvertPacket) != sizeof(SpectatorGrantAckPacket), "size-dispatch collision");
static_assert(sizeof(SpectatorNodeAdvertPacket) != sizeof(SpectatorClaimPacket), "size-dispatch collision");
static_assert(sizeof(SpectatorGrantPacket) != sizeof(SpectatorGrantAckPacket), "size-dispatch collision");
static_assert(sizeof(SpectatorGrantPacket) != sizeof(SpectatorClaimPacket), "size-dispatch collision");
static_assert(sizeof(SpectatorGrantAckPacket) != sizeof(SpectatorClaimPacket), "size-dispatch collision");

struct InputQueue
{
    std::uint32_t writeIndex = 0;
    std::array<PlayerInputRecord, kMaxQueueSlot> slots{};
};

}
