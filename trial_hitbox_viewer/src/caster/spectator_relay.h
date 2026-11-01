#pragma once

#include "caster_session.h"
#include "spectator_checkpoint.h"
#include "trial_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace thv::spectator
{

struct GameStartInfo
{
    std::uint32_t sourceGeneration = 0u;
    std::uint32_t sourceFrame = 0u;
    std::uint32_t rngSeed = 0u;
    std::uint32_t frontendRngState = 0u;
    std::uint32_t frontendVisualRngState = 0u;
    std::uint8_t screen = 0u;
    std::uint8_t p1CharacterId = 0u;
    std::uint8_t p2CharacterId = 0u;
    std::uint8_t p1PaletteId = 0u;
    std::uint8_t p2PaletteId = 0u;
    std::uint8_t stageId = 0u;
};

struct ConfirmedFrame
{
    std::uint32_t gameId = 0u;
    std::uint32_t recordIndex = 0u;
    std::uint32_t sourceGeneration = 0u;
    std::uint32_t sourceFrame = 0u;
    std::uint16_t p1Word = 0u;
    std::uint16_t p2Word = 0u;
    std::uint8_t screen = 0u;
    bool hasRegionHashes = false;
    std::array<std::uint32_t, proto::kFrameHashRegions> regionHashes{};
    std::uint64_t inputDigest = protocol::kInputDigestSeed;
};

struct Status
{
    bool hostArchiveActive = false;
    bool gameStartAvailable = false;
    bool gameActive = false;
    std::uint32_t rngSeed = 0u;
    std::uint8_t inputDelay = 0u;
    std::uint8_t rollbackFrames = 0u;
    std::uint8_t targetFps = 0u;
    std::uint32_t firstGameId = 0u;
    std::uint32_t gameId = 0u;
    std::uint32_t gameStartGeneration = 0u;
    std::uint32_t gameStartFrame = 0u;
    std::uint32_t lastSourceGeneration = 0u;
    std::uint32_t lastSourceFrame = 0u;
    std::uint32_t checkpointGeneration = 0u;
    std::uint32_t checkpointFrame = 0u;
    std::size_t checkpointBytes = 0u;
    std::size_t checkpointFailures = 0u;
    std::size_t retainedGames = 0u;
    std::size_t retainedFrames = 0u;
    std::size_t confirmedFrames = 0u;
    std::size_t totalConfirmedFrames = 0u;
    std::size_t rejectedGaps = 0u;
    std::size_t duplicateFrames = 0u;
    std::uint64_t latestInputDigest = protocol::kInputDigestSeed;
};

struct GameExtent
{
    std::uint32_t gameId = 0u;
    std::size_t frameCount = 0u;
    bool hasNextGame = false;
    std::uint64_t startInputDigest = protocol::kInputDigestSeed;
    std::uint64_t finalInputDigest = protocol::kInputDigestSeed;
};

void BeginHostArchive(const session::LauncherSession& launcherSession);
// Commit before any GameStart: both archives must fingerprint the negotiated value.
bool CommitNegotiatedRollbackFrames(std::uint8_t rollbackFrames);
void EndSession();

bool BeginGame(const GameStartInfo& start);
bool IsRecordingActive();

bool CaptureGameStartVisualMetadata(GameStartInfo* start);

// Called once per confirmed frame. P1/P2 are canonical labels, never local/remote.
bool PublishConfirmedFrame(
    std::uint32_t sourceGeneration,
    std::uint32_t sourceFrame,
    std::uint8_t screen,
    std::uint16_t p1Word,
    std::uint16_t p2Word,
    const std::uint32_t* regionHashes,
    std::size_t regionCount);

constexpr std::uint16_t PackInputWord(
    const std::uint8_t action,
    const std::uint8_t systemState)
{
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(action)
        | (static_cast<std::uint16_t>(systemState) << 8u));
}

// Input delay must never re-index sourceFrame, especially after a live delay change.
inline std::uint8_t ConfirmedSystemStateForCanonicalSideAt(
    const std::vector<std::uint8_t>& localHistory,
    const std::vector<std::uint8_t>& remoteHistory,
    const int localSide,
    const int canonicalSide,
    const std::uint32_t sourceFrame)
{
    if ((localSide != 0 && localSide != 1)
        || (canonicalSide != 0 && canonicalSide != 1))
    {
        return 0u;
    }
    const std::vector<std::uint8_t>& history = canonicalSide == localSide
        ? localHistory
        : remoteHistory;
    return sourceFrame < history.size() ? history[sourceFrame] : 0u;
}

bool NeedsCheckpoint(std::uint32_t generation, std::uint32_t frame);
bool PublishCheckpoint(Checkpoint checkpoint);
std::optional<Checkpoint> CopyCheckpoint();

bool BuildLatestBootstrapTransfer(
    std::uint32_t sessionTag,
    std::uint32_t firstSequence,
    protocol::Bootstrap* outBootstrap,
    std::vector<protocol::BootstrapChunk>* outChunks);

bool BuildGameStartMessage(
    // Zero selects the earliest retained game; otherwise the ID must exist in this set.
    std::uint32_t gameId,
    std::uint32_t sessionTag,
    std::uint32_t sequence,
    protocol::GameStart* outStart);
bool BuildConfirmedInputBatch(
    std::uint32_t gameId,
    std::uint32_t recordIndex,
    std::uint32_t sessionTag,
    std::uint32_t sequence,
    protocol::ConfirmedInputBatch* outBatch);
bool GetGameExtent(std::uint32_t gameId, GameExtent* outExtent);
// Digest at a cumulative receive cursor (0 = the game's seed; N = record N-1 digest).
bool GetInputDigestAt(
    std::uint32_t gameId,
    std::uint32_t nextRecordIndex,
    std::uint64_t* outDigest);

std::vector<ConfirmedFrame> CopyFrom(
    std::uint32_t gameId,
    std::uint32_t recordIndex,
    std::size_t maxFrames);
Status GetStatus();

}
