#include "spectator_relay.h"

#include "debug_log.h"
#include "determinism.h"
#include "netplay_sync.h"
#include "trial_addresses.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <windows.h>
#include <xmmintrin.h>

namespace thv::spectator
{
namespace
{

// Every per-game tail is retained: about 11 MiB/hour at 64 Hz.
struct GameArchive
{
    std::uint32_t gameId = 0u;
    GameStartInfo start{};
    std::uint64_t startInputDigest = protocol::kInputDigestSeed;
    std::deque<ConfirmedFrame> frames{};
};

std::mutex g_mutex;
std::deque<GameArchive> g_games;
std::optional<Checkpoint> g_checkpoint;
std::uint32_t g_nextGameId = 1u;
std::uint32_t g_lastRejectedExpected = std::numeric_limits<std::uint32_t>::max();
std::uint32_t g_lastRejectedSource = std::numeric_limits<std::uint32_t>::max();
Status g_status{};
std::array<char, 32> g_p1Name{};
std::array<char, 32> g_p2Name{};
std::uint8_t g_sourceLocalSide = 0u;
std::uint16_t g_initialFpuControlWord = 0u;
std::uint32_t g_initialMxcsr = 0u;

std::array<char, 32> NarrowName(const std::wstring& value)
{
    std::array<char, 32> out{};
    if (value.empty())
    {
        return out;
    }
    const int written = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        out.data(),
        static_cast<int>(out.size() - 1u),
        nullptr,
        nullptr);
    if (written > 0)
    {
        out[static_cast<std::size_t>(written)] = '\0';
    }
    return out;
}

GameArchive* FindGame(const std::uint32_t gameId)
{
    const auto it = std::find_if(
        g_games.begin(),
        g_games.end(),
        [gameId](const GameArchive& game) { return game.gameId == gameId; });
    return it != g_games.end() ? &*it : nullptr;
}

bool SameGameStart(const GameStartInfo& left, const GameStartInfo& right)
{
    return left.sourceGeneration == right.sourceGeneration
        && left.sourceFrame == right.sourceFrame
        && left.rngSeed == right.rngSeed
        && left.frontendRngState == right.frontendRngState
        && left.frontendVisualRngState == right.frontendVisualRngState
        && left.screen == right.screen
        && left.p1CharacterId == right.p1CharacterId
        && left.p2CharacterId == right.p2CharacterId
        && left.p1PaletteId == right.p1PaletteId
        && left.p2PaletteId == right.p2PaletteId
        && left.stageId == right.stageId;
}

}

void BeginHostArchive(const session::LauncherSession& launcherSession)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_games.clear();
    g_checkpoint.reset();
    g_nextGameId = 1u;
    g_lastRejectedExpected = std::numeric_limits<std::uint32_t>::max();
    g_lastRejectedSource = std::numeric_limits<std::uint32_t>::max();
    g_status = {};
    // `hostArchiveActive` means this process is a replay source, on either peer.
    g_status.hostArchiveActive =
        launcherSession.mode == session::LauncherMode::Host
        || launcherSession.mode == session::LauncherMode::Client;
    g_status.rngSeed = netplay::GetSyncedRngSeed();
    g_status.inputDelay = launcherSession.inputDelay;
    g_status.rollbackFrames = launcherSession.rollbackFrames;
    g_status.targetFps = launcherSession.targetFps;
    if (g_status.hostArchiveActive && !protocol::IsSupportedTargetFps(g_status.targetFps))
    {
        log::Error(
            "spectator",
            "refusing recorder with invalid session fps=%u",
            static_cast<unsigned>(g_status.targetFps));
        g_status.hostArchiveActive = false;
    }
    g_status.latestInputDigest = protocol::kInputDigestSeed;
    g_sourceLocalSide = launcherSession.localIsP1 ? 0u : 1u;
    const auto localName = NarrowName(launcherSession.playerName);
    const auto remoteName = NarrowName(launcherSession.peerName);
    // Archive records use canonical side labels, never local/remote.
    g_p1Name = g_sourceLocalSide == 0u ? localName : remoteName;
    g_p2Name = g_sourceLocalSide == 0u ? remoteName : localName;
    g_initialFpuControlWord = det::GetCapturedFpuCw();
    g_initialMxcsr = _mm_getcsr();
    if (g_status.hostArchiveActive)
    {
        log::Info(
            "spectator",
            "confirmed P1/P2 set recorder armed source=P%u fps=%u (waiting for character-select boundary)",
            static_cast<unsigned>(g_sourceLocalSide + 1u),
            static_cast<unsigned>(g_status.targetFps));
    }
}

bool CommitNegotiatedRollbackFrames(const std::uint8_t rollbackFrames)
{
    if (rollbackFrames > proto::kMaxRollbackFrames)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_status.hostArchiveActive)
    {
        return false;
    }
    if (g_status.rollbackFrames == rollbackFrames)
    {
        return true;
    }
    if (g_status.gameStartAvailable && g_status.rollbackFrames != rollbackFrames)
    {
        log::Error(
            "spectator",
            "refusing late rollback contract change old=%u new=%u",
            static_cast<unsigned>(g_status.rollbackFrames),
            static_cast<unsigned>(rollbackFrames));
        return false;
    }
    const std::uint8_t previous = g_status.rollbackFrames;
    g_status.rollbackFrames = rollbackFrames;
    log::Info(
        "spectator",
        "recorder rollback contract committed old=%u negotiated=%u",
        static_cast<unsigned>(previous),
        static_cast<unsigned>(rollbackFrames));
    return true;
}

bool CaptureGameStartVisualMetadata(GameStartInfo* const start)
{
    if (start == nullptr)
    {
        return false;
    }

    const auto* const p1Selection = static_cast<const std::uint16_t*>(
        trial::ResolveVa(trial::kVaP1Player));
    const auto* const p2Selection = static_cast<const std::uint16_t*>(
        trial::ResolveVa(trial::kVaP2Player));
    const auto* const stage = static_cast<const std::uint8_t*>(
        trial::ResolveVa(trial::kVaCharSelectStage));
    if (p1Selection == nullptr || p2Selection == nullptr || stage == nullptr)
    {
        return false;
    }

    const std::uint16_t p1Word = *p1Selection;
    const std::uint16_t p2Word = *p2Selection;
    start->p1CharacterId = static_cast<std::uint8_t>(p1Word & 0xFFu);
    start->p2CharacterId = static_cast<std::uint8_t>(p2Word & 0xFFu);
    start->p1PaletteId = static_cast<std::uint8_t>(p1Word >> 8u);
    start->p2PaletteId = static_cast<std::uint8_t>(p2Word >> 8u);
    start->stageId = *stage;
    return true;
}

void EndSession()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_status.hostArchiveActive)
    {
        log::Info(
            "spectator",
            "recorder ended games=%zu frames=%zu currentGame=%u currentFrames=%zu gaps=%zu duplicates=%zu",
            g_status.retainedGames,
            g_status.retainedFrames,
            g_status.gameId,
            g_status.confirmedFrames,
            g_status.rejectedGaps,
            g_status.duplicateFrames);
    }
    g_games.clear();
    g_checkpoint.reset();
    g_nextGameId = 1u;
    g_lastRejectedExpected = std::numeric_limits<std::uint32_t>::max();
    g_lastRejectedSource = std::numeric_limits<std::uint32_t>::max();
    g_status = {};
    g_p1Name = {};
    g_p2Name = {};
    g_sourceLocalSide = 0u;
    g_initialFpuControlWord = 0u;
    g_initialMxcsr = 0u;
}

bool BeginGame(const GameStartInfo& start)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_status.hostArchiveActive)
    {
        return false;
    }

    if (!g_games.empty()
        && g_games.back().start.sourceGeneration == start.sourceGeneration
        && g_games.back().start.sourceFrame == start.sourceFrame)
    {
        if (SameGameStart(g_games.back().start, start))
        {
            return true; // the raw-screen poll and engine transition may observe one boundary
        }
        ++g_status.rejectedGaps;
        log::Error(
            "spectator",
            "conflicting game metadata at source=%u:%u game=%u",
            start.sourceGeneration,
            start.sourceFrame,
            g_games.back().gameId);
        return false;
    }

    g_checkpoint.reset();
    GameArchive game{};
    game.gameId = g_nextGameId++;
    game.start = start;
    game.startInputDigest = g_status.latestInputDigest;
    g_games.push_back(std::move(game));

    g_status.gameStartAvailable = true;
    g_status.gameActive = true;
    g_status.firstGameId = g_games.front().gameId;
    g_status.gameId = g_games.back().gameId;
    g_status.gameStartGeneration = start.sourceGeneration;
    g_status.gameStartFrame = start.sourceFrame;
    g_status.lastSourceGeneration = start.sourceGeneration;
    g_status.lastSourceFrame = start.sourceFrame;
    g_status.confirmedFrames = 0u;
    g_status.retainedGames = g_games.size();
    // Do not cross into the player transport while holding the spectator mutex.
    if (start.rngSeed != 0u)
    {
        g_status.rngSeed = start.rngSeed;
    }
    g_status.checkpointGeneration = 0u;
    g_status.checkpointFrame = 0u;
    g_status.checkpointBytes = 0u;
    g_lastRejectedExpected = std::numeric_limits<std::uint32_t>::max();
    g_lastRejectedSource = std::numeric_limits<std::uint32_t>::max();

    log::Info(
        "spectator",
        "set recorder begin game=%u retainedGames=%zu source=%u:%u screen=%u rng=%08X visuals=P1(c%u,p%u) P2(c%u,p%u) stage=%u",
        g_status.gameId,
        g_status.retainedGames,
        start.sourceGeneration,
        start.sourceFrame,
        static_cast<unsigned>(start.screen),
        g_status.rngSeed,
        static_cast<unsigned>(start.p1CharacterId),
        static_cast<unsigned>(start.p1PaletteId),
        static_cast<unsigned>(start.p2CharacterId),
        static_cast<unsigned>(start.p2PaletteId),
        static_cast<unsigned>(start.stageId));
    return true;
}

bool IsRecordingActive()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status.hostArchiveActive && g_status.gameActive && !g_games.empty();
}

bool NeedsCheckpoint(const std::uint32_t generation, const std::uint32_t frame)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_status.hostArchiveActive || !g_status.gameActive)
    {
        return false;
    }
    return !g_checkpoint.has_value()
        || g_checkpoint->generation != generation
        || g_checkpoint->frame > frame;
}

bool PublishCheckpoint(Checkpoint checkpoint)
{
    std::string error;
    if (!ValidateCheckpoint(checkpoint, &error))
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_status.checkpointFailures;
        log::Error(
            "spectator",
            "checkpoint rejected generation=%u frame=%u error=%s failures=%zu",
            checkpoint.generation,
            checkpoint.frame,
            error.c_str(),
            g_status.checkpointFailures);
        return false;
    }

    std::vector<std::uint8_t> encoded;
    if (!SerializeCheckpoint(checkpoint, &encoded, &error))
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_status.checkpointFailures;
        log::Error(
            "spectator",
            "checkpoint serialization failed generation=%u frame=%u error=%s failures=%zu",
            checkpoint.generation,
            checkpoint.frame,
            error.c_str(),
            g_status.checkpointFailures);
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_status.hostArchiveActive || !g_status.gameActive)
    {
        return false;
    }
    g_status.checkpointGeneration = checkpoint.generation;
    g_status.checkpointFrame = checkpoint.frame;
    g_status.checkpointBytes = encoded.size();
    g_checkpoint = std::move(checkpoint);
    log::Info(
        "spectator",
        "diagnostic checkpoint game=%u generation=%u frame=%u bytes=%zu tail=%zu",
        g_status.gameId,
        g_status.checkpointGeneration,
        g_status.checkpointFrame,
        g_status.checkpointBytes,
        g_games.empty() ? 0u : g_games.back().frames.size());
    return true;
}

std::optional<Checkpoint> CopyCheckpoint()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_checkpoint;
}

bool BuildLatestBootstrapTransfer(
    const std::uint32_t sessionTag,
    const std::uint32_t firstSequence,
    protocol::Bootstrap* const outBootstrap,
    std::vector<protocol::BootstrapChunk>* const outChunks)
{
    Checkpoint checkpoint{};
    std::uint8_t inputDelay = 0u;
    std::uint8_t rollbackFrames = 0u;
    std::uint8_t targetFps = 0u;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_status.hostArchiveActive || !g_checkpoint.has_value())
        {
            return false;
        }
        checkpoint = *g_checkpoint;
        inputDelay = g_status.inputDelay;
        rollbackFrames = g_status.rollbackFrames;
        targetFps = g_status.targetFps;
    }
    std::string error;
    if (!BuildBootstrapTransfer(
            checkpoint,
            sessionTag,
            firstSequence,
            inputDelay,
            rollbackFrames,
            targetFps,
            outBootstrap,
            outChunks,
            &error))
    {
        log::Error("spectator", "bootstrap transfer build failed error=%s", error.c_str());
        return false;
    }
    return true;
}

bool BuildGameStartMessage(
    const std::uint32_t gameId,
    const std::uint32_t sessionTag,
    const std::uint32_t sequence,
    protocol::GameStart* const outStart)
{
    if (outStart == nullptr)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_status.hostArchiveActive || !g_status.gameStartAvailable || g_games.empty()
        || !protocol::IsSupportedTargetFps(g_status.targetFps))
    {
        return false;
    }
    const std::uint32_t requestedGameId = gameId != 0u ? gameId : g_games.front().gameId;
    const GameArchive* const game = FindGame(requestedGameId);
    if (game == nullptr)
    {
        return false;
    }

    protocol::GameStart message{};
    message.header.sessionTag = sessionTag;
    message.header.sequence = sequence;
    message.gameId = game->gameId;
    message.sourceGeneration = game->start.sourceGeneration;
    message.sourceFrame = game->start.sourceFrame;
    message.rngSeed = game->start.rngSeed != 0u ? game->start.rngSeed : g_status.rngSeed;
    message.frontendRngState = game->start.frontendRngState;
    message.frontendVisualRngState = game->start.frontendVisualRngState;
    message.compatibilityId = trialnet::compat::kCompatibilityId;
    message.simulationContractId = protocol::kSimulationContractId;
    message.mxcsr = g_initialMxcsr;
    message.inputDigestSeed = game->startInputDigest;
    message.fpuControlWord = g_initialFpuControlWord;
    message.screen = game->start.screen;
    message.inputDelay = g_status.inputDelay;
    message.rollbackFrames = g_status.rollbackFrames;
    message.targetFps = g_status.targetFps;
    message.p1CharacterId = game->start.p1CharacterId;
    message.p2CharacterId = game->start.p2CharacterId;
    message.p1PaletteId = game->start.p1PaletteId;
    message.p2PaletteId = game->start.p2PaletteId;
    message.stageId = game->start.stageId;
    // Must stay canonical: a local side would make host and client fingerprints differ.
    message.localSide = 0u;
    message.p1Name = g_p1Name;
    message.p2Name = g_p2Name;
    *outStart = message;
    return true;
}

bool BuildConfirmedInputBatch(
    const std::uint32_t gameId,
    const std::uint32_t recordIndex,
    const std::uint32_t sessionTag,
    const std::uint32_t sequence,
    protocol::ConfirmedInputBatch* const outBatch)
{
    if (outBatch == nullptr)
    {
        return false;
    }

    protocol::ConfirmedInputBatch batch{};
    batch.header.sessionTag = sessionTag;
    batch.header.sequence = sequence;
    batch.gameId = gameId;
    batch.startRecordIndex = recordIndex;

    std::lock_guard<std::mutex> lock(g_mutex);
    const GameArchive* const game = FindGame(gameId);
    if (!g_status.hostArchiveActive || !g_status.gameActive || game == nullptr
        || recordIndex >= game->frames.size())
    {
        return false;
    }

    auto it = game->frames.begin() + static_cast<std::ptrdiff_t>(recordIndex);
    std::uint32_t expected = recordIndex;
    for (; it != game->frames.end() && batch.count < batch.frames.size(); ++it)
    {
        if (it->gameId != gameId || it->recordIndex != expected)
        {
            return false; // recorder invariant failed; never transmit a discontinuous tail
        }
        protocol::ConfirmedInput& wire = batch.frames[batch.count++];
        wire.recordIndex = it->recordIndex;
        wire.sourceGeneration = it->sourceGeneration;
        wire.sourceFrame = it->sourceFrame;
        wire.p1Word = it->p1Word;
        wire.p2Word = it->p2Word;
        wire.screen = it->screen;
        wire.hasRegionHashes = it->hasRegionHashes ? 1u : 0u;
        wire.regionHashes = it->regionHashes;
        wire.inputDigest = it->inputDigest;
        ++expected;
    }
    if (batch.count == 0u)
    {
        return false;
    }
    *outBatch = batch;
    return true;
}

bool GetGameExtent(const std::uint32_t gameId, GameExtent* const outExtent)
{
    if (outExtent == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const GameArchive* const game = FindGame(gameId);
    if (!g_status.hostArchiveActive || game == nullptr)
    {
        return false;
    }
    GameExtent extent{};
    extent.gameId = gameId;
    extent.frameCount = game->frames.size();
    extent.hasNextGame = !g_games.empty() && gameId != g_games.back().gameId;
    extent.startInputDigest = game->startInputDigest;
    extent.finalInputDigest = game->frames.empty()
        ? game->startInputDigest : game->frames.back().inputDigest;
    *outExtent = extent;
    return true;
}

bool GetInputDigestAt(
    const std::uint32_t gameId,
    const std::uint32_t nextRecordIndex,
    std::uint64_t* const outDigest)
{
    if (outDigest == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const GameArchive* const game = FindGame(gameId);
    if (game == nullptr || nextRecordIndex > game->frames.size())
    {
        return false;
    }
    *outDigest = nextRecordIndex == 0u
        ? game->startInputDigest
        : game->frames[nextRecordIndex - 1u].inputDigest;
    return true;
}

bool PublishConfirmedFrame(
    const std::uint32_t sourceGeneration,
    const std::uint32_t sourceFrame,
    const std::uint8_t screen,
    const std::uint16_t p1Word,
    const std::uint16_t p2Word,
    const std::uint32_t* const regionHashes,
    const std::size_t regionCount)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_status.hostArchiveActive || !g_status.gameActive || g_games.empty())
    {
        return false;
    }

    GameArchive& game = g_games.back();

    const bool hasRegionHashes = regionHashes != nullptr || regionCount != 0u;
    if ((regionHashes == nullptr) != (regionCount == 0u)
        || (regionCount != 0u && regionCount != proto::kFrameHashRegions))
    {
        ++g_status.rejectedGaps;
        log::Error(
            "spectator",
            "invalid confirmed-frame hash payload game=%u source=%u:%u pointer=%d count=%zu",
            game.gameId,
            sourceGeneration,
            sourceFrame,
            regionHashes != nullptr ? 1 : 0,
            regionCount);
        return false;
    }

    std::array<std::uint32_t, proto::kFrameHashRegions> incomingRegionHashes{};
    if (hasRegionHashes)
    {
        std::copy_n(regionHashes, incomingRegionHashes.size(), incomingRegionHashes.begin());
    }

    const std::uint32_t expectedFrame = game.frames.empty()
        ? game.start.sourceFrame
        : game.frames.back().sourceFrame + 1u;
    if (sourceFrame != expectedFrame)
    {
        if (!game.frames.empty() && sourceFrame == game.frames.back().sourceFrame
            && sourceGeneration == game.frames.back().sourceGeneration
            && screen == game.frames.back().screen
            && p1Word == game.frames.back().p1Word && p2Word == game.frames.back().p2Word
            && hasRegionHashes == game.frames.back().hasRegionHashes
            && (!hasRegionHashes || incomingRegionHashes == game.frames.back().regionHashes))
        {
            ++g_status.duplicateFrames;
            return true;
        }

        if (sourceFrame < expectedFrame)
        {
            ++g_status.duplicateFrames;
        }
        else
        {
            if (expectedFrame != g_lastRejectedExpected || sourceFrame != g_lastRejectedSource)
            {
                g_lastRejectedExpected = expectedFrame;
                g_lastRejectedSource = sourceFrame;
                ++g_status.rejectedGaps;
                log::Error(
                    "spectator",
                    "recorder gap game=%u expectedFrame=%u got=%u generation=%u gaps=%zu",
                    g_status.gameId,
                    expectedFrame,
                    sourceFrame,
                    sourceGeneration,
                    g_status.rejectedGaps);
            }
        }
        return false;
    }

    ConfirmedFrame item{};
    item.gameId = g_status.gameId;
    item.recordIndex = static_cast<std::uint32_t>(game.frames.size());
    item.sourceGeneration = sourceGeneration;
    item.sourceFrame = sourceFrame;
    item.p1Word = p1Word;
    item.p2Word = p2Word;
    item.screen = screen;
    item.hasRegionHashes = hasRegionHashes;
    if (item.hasRegionHashes)
    {
        item.regionHashes = incomingRegionHashes;
    }
    item.inputDigest = protocol::AdvanceInputDigest(
        game.frames.empty() ? game.startInputDigest : game.frames.back().inputDigest,
        item.gameId,
        item.recordIndex,
        item.sourceGeneration,
        item.sourceFrame,
        item.screen,
        item.p1Word,
        item.p2Word,
        item.hasRegionHashes ? 1u : 0u,
        item.regionHashes);
    game.frames.push_back(item);
    g_lastRejectedExpected = std::numeric_limits<std::uint32_t>::max();
    g_lastRejectedSource = std::numeric_limits<std::uint32_t>::max();
    g_status.lastSourceGeneration = sourceGeneration;
    g_status.lastSourceFrame = sourceFrame;
    ++g_status.confirmedFrames;
    ++g_status.totalConfirmedFrames;
    ++g_status.retainedFrames;
    g_status.latestInputDigest = item.inputDigest;
    if (item.recordIndex == 0u || ((item.recordIndex + 1u) % 256u) == 0u
        || ((p1Word | p2Word) & 0xFF00u) != 0u)
    {
        if (((p1Word | p2Word) & 0xFF00u) != 0u)
        {
            log::Info(
                "spectator",
                "SPECTATOR SYSTEM INPUT source game=%u record=%u source=%u p1=%04X p2=%04X",
                item.gameId,
                item.recordIndex,
                item.sourceFrame,
                static_cast<unsigned>(item.p1Word),
                static_cast<unsigned>(item.p2Word));
        }
        if (item.recordIndex == 0u || ((item.recordIndex + 1u) % 256u) == 0u)
        {
            log::Info(
                "spectator",
                "SPECTATOR INPUT DIGEST source game=%u records=%u digest=%016llX source=%u",
                item.gameId,
                item.recordIndex + 1u,
                static_cast<unsigned long long>(item.inputDigest),
                item.sourceFrame);
        }
    }
    return true;
}

std::vector<ConfirmedFrame> CopyFrom(
    const std::uint32_t gameId,
    const std::uint32_t recordIndex,
    const std::size_t maxFrames)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<ConfirmedFrame> out;
    const GameArchive* const game = FindGame(gameId);
    if (game == nullptr || recordIndex >= game->frames.size() || maxFrames == 0u)
    {
        return out;
    }
    const std::size_t count = std::min(maxFrames, game->frames.size() - recordIndex);
    out.reserve(count);
    auto it = game->frames.begin() + static_cast<std::ptrdiff_t>(recordIndex);
    for (std::size_t i = 0u; i < count; ++i, ++it)
    {
        out.push_back(*it);
    }
    return out;
}

Status GetStatus()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

}
