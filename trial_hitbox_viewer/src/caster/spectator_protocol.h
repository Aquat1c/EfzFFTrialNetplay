#pragma once

#include "trial_protocol.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace thv::spectator::protocol
{

constexpr std::uint32_t kMagic = 0x535A4645u; // "EFZS"
constexpr std::uint16_t kVersion = trialnet::compat::kCompatibilityVersion;
constexpr std::uint32_t kSimulationContractId = 0x5453500Bu; // Bump when the replay contract changes.
constexpr std::uint64_t kInputDigestSeed = 0xCBF29CE484222325ull;
// Keeps the packed datagram below the 1200-byte UDP payload ceiling.
constexpr std::size_t kBatchFrames = 20u;
// Fits below the common 1200-byte UDP payload budget once framing is included.
constexpr std::size_t kBootstrapChunkBytes = 1024u;

// Zero is not a negotiated rate.
inline constexpr bool IsSupportedTargetFps(const std::uint8_t targetFps)
{
    return proto::IsSupportedTargetFps(targetFps);
}

inline constexpr bool MatchesTargetFpsContract(
    const std::uint8_t acceptedTargetFps,
    const std::uint8_t candidateTargetFps)
{
    return IsSupportedTargetFps(candidateTargetFps)
        && (acceptedTargetFps == 0u || acceptedTargetFps == candidateTargetFps);
}

enum class MessageKind : std::uint16_t
{
    Hello = 1,
    Bootstrap = 2,
    ConfirmedInputBatch = 3,
    Detach = 4,
    KeepAlive = 5,
    BootstrapChunk = 6,
    Ack = 7,
    GameStart = 8,
    Reject = 9,
    InitAck = 10,
    Terminal = 11,
    Waiting = 12,
    TerminalAck = 13,
    // Minted by the source, consumed by the target; viewers never reinterpret it.
    Redirect = 14,
};

enum class RejectReason : std::uint16_t
{
    InvalidRequest = 1,
    VersionMismatch = 2,
    GameNotRetained = 3,
    ServerFull = 4,
    SimulationMismatch = 5,
};

enum class TerminalReason : std::uint16_t
{
    SessionEnded = 1,
    PlayerDisconnected = 2,
    HostShutdown = 3,
    ArchiveUnavailable = 4,
    ProtocolError = 5,
};

enum class WaitingReason : std::uint16_t
{
    MatchNotStarted = 1,
    BetweenGames = 2,
};

#pragma pack(push, 1)
struct Header
{
    std::uint32_t magic = kMagic;
    std::uint16_t version = kVersion;
    MessageKind kind = MessageKind::Hello;
    std::uint32_t sessionTag = 0u;
    std::uint32_t sequence = 0u;
};

// requestedGameId=0 selects the first retained game; non-zero record index resumes.
struct Hello
{
    Header header{kMagic, kVersion, MessageKind::Hello, 0u, 0u};
    std::uint32_t requestedGameId = 0u;
    std::uint32_t nextRecordIndex = 0u;
    std::uint32_t clientNonce = 0u;
    std::uint32_t simulationContractId = kSimulationContractId;
    // All zero on normal attach; after a failed redirect, failedGrantId names the grant.
    std::uint32_t relayEpoch = 0u;
    std::uint32_t grantId = 0u;
    std::uint32_t failedGrantId = 0u;
    std::array<std::uint8_t, 16> token{};
};

// targetIpv4 is in sin_addr.s_addr byte order; targetInternalPort is host order.
struct Redirect
{
    Header header{kMagic, kVersion, MessageKind::Redirect, 0u, 0u};
    std::uint32_t clientNonce = 0u;
    std::uint32_t relayEpoch = 0u;
    std::uint32_t grantId = 0u;
    std::uint32_t requestedGameId = 0u;
    std::uint32_t nextRecordIndex = 0u;
    std::uint64_t expectedDigest = kInputDigestSeed;
    std::uint32_t targetIpv4 = 0u;
    std::uint16_t targetInternalPort = 0u;
    std::uint8_t hop = 0u;
    std::uint8_t reserved = 0u;
    std::array<std::uint8_t, 16> token{};
};

// GameStart is not accepted until this fingerprint and nonce return.
struct InitAck
{
    Header header{kMagic, kVersion, MessageKind::InitAck, 0u, 0u};
    std::uint32_t gameId = 0u;
    std::uint32_t nextRecordIndex = 0u;
    std::uint32_t clientNonce = 0u;
    std::uint64_t initFingerprint = 0u;
};

// nextRecordIndex is the first record still needed.
struct Ack
{
    Header header{kMagic, kVersion, MessageKind::Ack, 0u, 0u};
    std::uint32_t gameId = 0u;
    std::uint32_t nextRecordIndex = 0u;
};

struct KeepAlive
{
    Header header{kMagic, kVersion, MessageKind::KeepAlive, 0u, 0u};
    std::uint32_t gameId = 0u;
    std::uint32_t nextRecordIndex = 0u;
};

struct Detach
{
    Header header{kMagic, kVersion, MessageKind::Detach, 0u, 0u};
};

struct Reject
{
    Header header{kMagic, kVersion, MessageKind::Reject, 0u, 0u};
    RejectReason reason = RejectReason::InvalidRequest;
    std::uint16_t serverVersion = kVersion;
    std::uint32_t requestedGameId = 0u;
};

struct Bootstrap
{
    Header header{kMagic, kVersion, MessageKind::Bootstrap, 0u, 0u};
    std::uint32_t rngShadow = 0u;
    std::uint32_t generation = 0u;
    std::uint32_t startFrame = 0u;
    std::uint32_t simFrame = 0u;
    std::uint32_t checkpointBytes = 0u;
    std::uint32_t checkpointCrc32 = 0u;
    std::uint16_t chunkCount = 0u;
    std::uint8_t inputDelay = 0u;
    std::uint8_t rollbackFrames = 0u;
    std::uint8_t p1CharacterId = 0u;
    std::uint8_t p2CharacterId = 0u;
    std::uint8_t stageId = 0u;
    std::uint8_t targetFps = 0u;
};

struct BootstrapChunk
{
    Header header{kMagic, kVersion, MessageKind::BootstrapChunk, 0u, 0u};
    std::uint32_t offset = 0u;
    std::uint16_t payloadBytes = 0u;
    std::uint16_t reserved = 0u;
    std::array<std::uint8_t, kBootstrapChunkBytes> payload{};
};

// gameId and recordIndex form the spectator cursor.
struct GameStart
{
    Header header{kMagic, kVersion, MessageKind::GameStart, 0u, 0u};
    std::uint32_t gameId = 0u;
    std::uint32_t sourceGeneration = 0u;
    std::uint32_t sourceFrame = 0u;
    std::uint32_t rngSeed = 0u;
    // Source RNG shadows at the character-select boundary.
    std::uint32_t frontendRngState = 0u;
    std::uint32_t frontendVisualRngState = 0u;
    std::uint32_t clientNonce = 0u;
    std::uint32_t compatibilityId = trialnet::compat::kCompatibilityId;
    std::uint32_t simulationContractId = kSimulationContractId;
    std::uint32_t mxcsr = 0u;
    std::uint64_t inputDigestSeed = kInputDigestSeed;
    std::uint16_t fpuControlWord = 0u;
    std::uint8_t screen = 0u;
    std::uint8_t inputDelay = 0u;
    std::uint8_t rollbackFrames = 0u;
    // Fingerprinted: must stay fixed for a set.
    std::uint8_t targetFps = 0u;
    std::uint8_t p1CharacterId = 0u;
    std::uint8_t p2CharacterId = 0u;
    // High bytes of the two character-select WORDs (stock palette selectors).
    std::uint8_t p1PaletteId = 0u;
    std::uint8_t p2PaletteId = 0u;
    std::uint8_t stageId = 0u;
    std::uint8_t localSide = 0u;
    // UTF-8, nul-terminated when shorter.
    std::array<char, 32> p1Name{};
    std::array<char, 32> p2Name{};
};

struct ConfirmedInput
{
    std::uint32_t recordIndex = 0u;
    std::uint32_t sourceGeneration = 0u;
    std::uint32_t sourceFrame = 0u;
    // Action byte in bits 0..7, system/context byte in bits 8..15.
    std::uint16_t p1Word = 0u;
    std::uint16_t p2Word = 0u;
    std::uint8_t screen = 0u;
    std::uint8_t hasRegionHashes = 0u;
    std::array<std::uint32_t, proto::kFrameHashRegions> regionHashes{};
    // Digest after this complete P1/P2 record.
    std::uint64_t inputDigest = 0u;
};

struct ConfirmedInputBatch
{
    Header header{kMagic, kVersion, MessageKind::ConfirmedInputBatch, 0u, 0u};
    std::uint32_t gameId = 0u;
    std::uint32_t startRecordIndex = 0u;
    std::uint8_t count = 0u;
    std::array<std::uint8_t, 3> reserved{};
    std::array<ConfirmedInput, kBatchFrames> frames{};
};

struct Terminal
{
    Header header{kMagic, kVersion, MessageKind::Terminal, 0u, 0u};
    TerminalReason reason = TerminalReason::SessionEnded;
    std::uint16_t reserved = 0u;
    std::uint32_t finalGameId = 0u;
    std::uint32_t finalRecordIndex = 0u;
    std::uint64_t finalInputDigest = kInputDigestSeed;
    std::uint64_t totalConfirmedFrames = 0u;
};

struct TerminalAck
{
    Header header{kMagic, kVersion, MessageKind::TerminalAck, 0u, 0u};
    std::uint32_t finalGameId = 0u;
    std::uint32_t finalRecordIndex = 0u;
    std::uint64_t finalInputDigest = kInputDigestSeed;
};

struct Waiting
{
    Header header{kMagic, kVersion, MessageKind::Waiting, 0u, 0u};
    WaitingReason reason = WaitingReason::MatchNotStarted;
    std::uint16_t retryAfterMs = 500u;
    std::uint32_t clientNonce = 0u;
};
#pragma pack(pop)

inline std::uint64_t DigestBytes(
    std::uint64_t digest,
    const void* const bytes,
    const std::size_t size)
{
    const auto* const data = static_cast<const std::uint8_t*>(bytes);
    for (std::size_t index = 0u; index < size; ++index)
    {
        digest ^= data[index];
        digest *= 0x100000001B3ull;
    }
    return digest;
}

inline std::uint64_t AdvanceInputDigest(
    std::uint64_t digest,
    const std::uint32_t gameId,
    const std::uint32_t recordIndex,
    const std::uint32_t sourceGeneration,
    const std::uint32_t sourceFrame,
    const std::uint8_t screen,
    const std::uint16_t p1Word,
    const std::uint16_t p2Word,
    const std::uint8_t hasRegionHashes,
    const std::array<std::uint32_t, proto::kFrameHashRegions>& regionHashes)
{
    digest = DigestBytes(digest, &gameId, sizeof(gameId));
    digest = DigestBytes(digest, &recordIndex, sizeof(recordIndex));
    digest = DigestBytes(digest, &sourceGeneration, sizeof(sourceGeneration));
    digest = DigestBytes(digest, &sourceFrame, sizeof(sourceFrame));
    digest = DigestBytes(digest, &screen, sizeof(screen));
    digest = DigestBytes(digest, &p1Word, sizeof(p1Word));
    digest = DigestBytes(digest, &p2Word, sizeof(p2Word));
    digest = DigestBytes(digest, &hasRegionHashes, sizeof(hasRegionHashes));
    return DigestBytes(digest, regionHashes.data(), sizeof(regionHashes));
}

inline bool HasCanonicalRegionHashes(
    const std::uint8_t hasRegionHashes,
    const std::array<std::uint32_t, proto::kFrameHashRegions>& regionHashes)
{
    if (hasRegionHashes > 1u)
    {
        return false;
    }
    return hasRegionHashes != 0u
        || std::all_of(regionHashes.begin(), regionHashes.end(), [](const std::uint32_t hash) {
               return hash == 0u;
           });
}

inline std::uint64_t GameStartFingerprint(const GameStart& start)
{
    // Excludes the header sequence and session tag, which are not immutable.
    return DigestBytes(
        kInputDigestSeed,
        reinterpret_cast<const std::uint8_t*>(&start) + sizeof(Header),
        sizeof(GameStart) - sizeof(Header));
}

inline std::size_t ConfirmedInputBatchWireSize(const std::size_t count)
{
    return offsetof(ConfirmedInputBatch, frames)
        + (count <= kBatchFrames ? count : kBatchFrames) * sizeof(ConfirmedInput);
}

static_assert(sizeof(Header) == 16u, "spectator header layout drift");
static_assert(sizeof(Hello) == 60u, "spectator hello layout drift");
static_assert(sizeof(Redirect) == 68u, "spectator redirect layout drift");
static_assert(sizeof(InitAck) == 36u, "spectator init ack layout drift");
static_assert(sizeof(Ack) == 24u, "spectator ack layout drift");
static_assert(sizeof(KeepAlive) == 24u, "spectator keepalive layout drift");
static_assert(sizeof(Detach) == 16u, "spectator detach layout drift");
static_assert(sizeof(Reject) == 24u, "spectator reject layout drift");
static_assert(sizeof(Bootstrap) == 48u, "spectator bootstrap layout drift");
static_assert(sizeof(GameStart) == 140u, "spectator game start layout drift");
static_assert(sizeof(Terminal) <= 1200u, "spectator terminal exceeds UDP budget");
static_assert(sizeof(TerminalAck) <= 1200u, "spectator terminal ack exceeds UDP budget");
static_assert(sizeof(Waiting) <= 1200u, "spectator waiting exceeds UDP budget");
static_assert(sizeof(GameStart) <= 1200u, "spectator game start exceeds UDP budget");
static_assert(sizeof(BootstrapChunk) <= 1200u, "spectator bootstrap chunk exceeds UDP budget");
static_assert(sizeof(ConfirmedInputBatch) <= 1200u, "spectator input batch exceeds UDP budget");

}
