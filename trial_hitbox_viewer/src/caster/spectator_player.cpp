#include "spectator_player.h"

#include "caster_input.h"
#include "control_swap.h"
#include "debug_log.h"
#include "determinism.h"
#include "game_pacing.h"
#include "lifecycle_log.h"
#include "menu_nav.h"
#include "netplay_bootstrap.h"
#include "render_gate.h"
#include "rng.h"
#include "rollback_memory.h"
#include "spectator_checkpoint.h"
#include "spectator_client.h"
#include "trial_addresses.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <xmmintrin.h>

namespace thv::spectator::player
{
namespace
{

constexpr std::size_t kStartupBufferFrames = 30u;
constexpr std::uint32_t kNoGeneration = std::numeric_limits<std::uint32_t>::max();

std::atomic<bool> g_active{false};
// Held for the whole owned tick; a same-thread shutdown only marks cleanup pending.
std::recursive_mutex g_driveMutex;
std::uint32_t g_driveDepth = 0u;
DWORD g_driveThreadId = 0u;
bool g_shutdownPending = false;
std::uint32_t g_gameId = 0u;
std::uint32_t g_baseSeed = 0u;
std::uint32_t g_battleGeneration = kNoGeneration;
bool g_waitingForStartupBuffer = true;
std::atomic<bool> g_closePosted{false};
std::atomic<DWORD> g_closePostedMs{0u};
std::atomic<bool> g_playbackFatal{false};
std::atomic<const char*> g_playbackError{nullptr};
bool g_sessionInitialized = false;
std::uint32_t g_verifiedBattleFrames = 0u;
DWORD g_lastTailWaitLogMs = 0u;
DWORD g_lastCatchUpLogMs = 0u;
DWORD g_screenMismatchSinceMs = 0u;
DWORD g_terminalSeenMs = 0u;

void FinalizeShutdownLocked()
{
    // Caller owns g_driveMutex and the outermost game-thread Drive scope has ended.
    g_active.store(false, std::memory_order_release);
    SetPresentSuppressed(false);
    SetRollbackAudioFrame(0u, false);
    client::Shutdown();
    pacing::ResetSessionTargetFps();
    if (caster::IsSpectatorActive())
    {
        caster::SetTransportMode(caster::TransportMode::Offline);
    }
    g_gameId = 0u;
    g_baseSeed = 0u;
    g_battleGeneration = kNoGeneration;
    g_waitingForStartupBuffer = true;
    g_closePosted.store(false, std::memory_order_release);
    g_closePostedMs.store(0u, std::memory_order_release);
    g_playbackFatal.store(false, std::memory_order_release);
    g_playbackError.store(nullptr, std::memory_order_release);
    g_sessionInitialized = false;
    g_verifiedBattleFrames = 0u;
    g_lastTailWaitLogMs = 0u;
    g_lastCatchUpLogMs = 0u;
    g_screenMismatchSinceMs = 0u;
    g_terminalSeenMs = 0u;
    g_shutdownPending = false;
}

class DriveScope final
{
public:
    DriveScope()
    {
        if (g_driveDepth == 0u)
        {
            g_driveThreadId = GetCurrentThreadId();
        }
        ++g_driveDepth;
    }

    ~DriveScope()
    {
        if (g_driveDepth == 0u)
        {
            return;
        }
        --g_driveDepth;
        if (g_driveDepth == 0u)
        {
            g_driveThreadId = 0u;
            if (g_shutdownPending)
            {
                FinalizeShutdownLocked();
            }
        }
    }

    DriveScope(const DriveScope&) = delete;
    DriveScope& operator=(const DriveScope&) = delete;
};

std::uint8_t ReadScreenMode()
{
    const auto* const screen = static_cast<const std::uint8_t*>(
        trial::ResolveVa(trial::kVaBattleScreenMode));
    return screen != nullptr ? *screen : 0xFFu;
}

void PaceOwnedStall(const std::uint8_t screen)
{
    if (screen == trial::kScreenBattle)
    {
        pacing::ConsumeBattleFrameSlot();
    }
    else
    {
        pacing::ConsumeActiveScreenFrameSlot();
    }
}

void PrepareCharSelectStage()
{
    const auto* const anim = static_cast<const std::int32_t*>(
        trial::ResolveVa(trial::kVaScreenAnimCounter));
    auto* const stage = static_cast<std::uint8_t*>(
        trial::ResolveVa(trial::kVaCharSelectStage));
    if (anim == nullptr || stage == nullptr || *anim < trial::kNativeTransitionNearComplete)
    {
        return;
    }
    std::uint8_t syncedStage = static_cast<std::uint8_t>(g_baseSeed % 4u);
    syncedStage = syncedStage == 2u ? 0u : (syncedStage == 1u ? 3u : syncedStage);
    *stage = syncedStage;
}

void PrepareBattleGeneration(const std::uint32_t generation)
{
    if (g_battleGeneration == generation)
    {
        return;
    }
    const std::uint32_t matchSeed = g_baseSeed ^ (0x9E3779B9u * (generation + 1u));
    det::ResetFpuForSimulation();
    det::rng::SetSeed(matchSeed);
    det::rng::VisualRestore(matchSeed ^ 0xA511E9B3u);

    // Mirror the peers' match-entry shake canonicalization or the aux hash differs.
    std::array<std::uint8_t, trial::kAuxStateSize> entryAux{};
    rollback::memory::CaptureShakeAux(entryAux.data());
    std::fill(
        entryAux.begin(),
        entryAux.begin() + static_cast<std::ptrdiff_t>(trial::kShakeStateSize),
        std::uint8_t{0});
    rollback::memory::RestoreShakeAux(entryAux.data());
    g_battleGeneration = generation;
    log::Info(
        "spectator",
        "battle entry reseed game=%u generation=%u seed=%08X",
        g_gameId,
        generation,
        matchSeed);
}

bool ApplyGameStartVisualMetadata(const protocol::GameStart& start)
{
    if (start.p1PaletteId > trial::kMaxStockPaletteId
        || start.p2PaletteId > trial::kMaxStockPaletteId)
    {
        log::Error(
            "spectator",
            "invalid stock palette metadata game=%u P1=%u P2=%u",
            start.gameId,
            static_cast<unsigned>(start.p1PaletteId),
            static_cast<unsigned>(start.p2PaletteId));
        return false;
    }

    auto* const p1Selection = static_cast<std::uint16_t*>(
        trial::ResolveVa(trial::kVaP1Player));
    auto* const p2Selection = static_cast<std::uint16_t*>(
        trial::ResolveVa(trial::kVaP2Player));
    auto* const stage = static_cast<std::uint8_t*>(
        trial::ResolveVa(trial::kVaCharSelectStage));
    if (p1Selection == nullptr || p2Selection == nullptr || stage == nullptr)
    {
        log::Error("spectator", "Trial visual selector globals are unavailable");
        return false;
    }

    const std::uint16_t p1Word = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(start.p1CharacterId)
        | (static_cast<std::uint16_t>(start.p1PaletteId) << 8u));
    const std::uint16_t p2Word = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(start.p2CharacterId)
        | (static_cast<std::uint16_t>(start.p2PaletteId) << 8u));
    *p1Selection = p1Word;
    *p2Selection = p2Word;
    *stage = start.stageId;
    log::Info(
        "spectator",
        "visual metadata applied game=%u P1(c%u,p%u word=%04X) P2(c%u,p%u word=%04X) stage=%u",
        start.gameId,
        static_cast<unsigned>(start.p1CharacterId),
        static_cast<unsigned>(start.p1PaletteId),
        static_cast<unsigned>(p1Word),
        static_cast<unsigned>(start.p2CharacterId),
        static_cast<unsigned>(start.p2PaletteId),
        static_cast<unsigned>(p2Word),
        static_cast<unsigned>(start.stageId));
    return true;
}

bool VerifyConfirmedMatchState(const protocol::ConfirmedInput& frame)
{
    if (frame.hasRegionHashes == 0u)
    {
        return true;
    }

    std::vector<std::uint8_t> battle;
    std::array<std::uint8_t, det::kFpuStateSize> fpu{};
    std::array<std::uint8_t, trial::kAuxStateSize> shake{};
    std::uint32_t rng = 0u;
    if (!rollback::memory::CaptureBattle(&battle)
        || battle.size() < trial::kDynamicBattleSize)
    {
        return false;
    }
    rollback::memory::CaptureDeterminism(fpu.data(), &rng);
    rollback::memory::CaptureShakeAux(shake.data());

    std::array<std::uint32_t, proto::kFrameHashRegions> local{};
    std::size_t offset = 0u;
    for (int region = 0; region < trial::kDynamicRegionCount; ++region)
    {
        const std::size_t size = trial::kDynamicBattleRegions[region].size;
        local[region] = CheckpointCrc32(battle.data() + offset, size);
        offset += size;
    }
    std::array<std::uint8_t, sizeof(std::uint16_t) + sizeof(std::uint32_t)> determinism{};
    std::memcpy(determinism.data(), fpu.data(), sizeof(std::uint16_t));
    std::memcpy(determinism.data() + sizeof(std::uint16_t), &rng, sizeof(rng));
    local[trial::kDynamicRegionCount] =
        CheckpointCrc32(determinism.data(), determinism.size());
    local[trial::kDynamicRegionCount + 1] = CheckpointCrc32(shake.data(), shake.size());

    for (std::size_t region = 0u; region < local.size(); ++region)
    {
        if (local[region] != frame.regionHashes[region])
        {
            (void)log::EnableFileLogging();
            log::Error(
                "spectator",
                "STATE DESYNC game=%u record=%u source=%u region=%zu local=%08X host=%08X",
                g_gameId,
                frame.recordIndex,
                frame.sourceFrame,
                region,
                local[region],
                frame.regionHashes[region]);
            return false;
        }
    }
    ++g_verifiedBattleFrames;
    if (g_verifiedBattleFrames == 1u || (g_verifiedBattleFrames % 256u) == 0u)
    {
        log::Info(
            "spectator",
            "confirmed state verified game=%u record=%u source=%u count=%u",
            g_gameId,
            frame.recordIndex,
            frame.sourceFrame,
            g_verifiedBattleFrames);
    }
    return true;
}

bool TryStartFrontGame(const std::uint8_t currentScreen)
{
    protocol::GameStart start{};
    if (!client::PeekGameStart(&start))
    {
        return false;
    }
    if (currentScreen != trial::kScreenCharSelect
        || start.screen != trial::kScreenCharSelect)
    {
        return false;
    }

    const client::Status status = client::GetStatus();
    if (g_waitingForStartupBuffer && !status.terminal
        && status.queuedFrames < kStartupBufferFrames
        && status.queuedGames < 2u)
    {
        return false;
    }

    if (!g_sessionInitialized || start.rngSeed != g_baseSeed)
    {
        (void)log::EnableFileLogging();
        g_playbackError.store("spectator game boundary disagrees with session initialization");
        g_playbackFatal.store(true);
        return false;
    }
    caster::SetSpectatorInputWords(0u, 0u);
    if (!ApplyGameStartVisualMetadata(start))
    {
        g_playbackError.store("spectator game visual metadata is invalid");
        g_playbackFatal.store(true);
        return false;
    }
    det::rng::Restore(start.frontendRngState);
    det::rng::VisualRestore(start.frontendVisualRngState);
    if (!client::MarkGameStarted(start.gameId))
    {
        return false;
    }
    g_gameId = start.gameId;
    g_battleGeneration = kNoGeneration;
    g_waitingForStartupBuffer = false;
    log::Info(
        "spectator",
        "playback begin game=%u source=%u:%u fps=%u buffered=%zu",
        start.gameId,
        start.sourceGeneration,
        start.sourceFrame,
        static_cast<unsigned>(start.targetFps),
        status.queuedFrames);
    return true;
}

bool TryInitializeSessionBeforeTransition()
{
    if (g_sessionInitialized)
    {
        return true;
    }
    protocol::GameStart start{};
    if (!client::PeekGameStart(&start))
    {
        return false;
    }
    if (start.fpuControlWord == 0u || start.mxcsr == 0u
        || start.compatibilityId != trialnet::compat::kCompatibilityId
        || start.simulationContractId != protocol::kSimulationContractId
        || !protocol::IsSupportedTargetFps(start.targetFps))
    {
        g_playbackError.store("spectator deterministic init metadata is invalid");
        g_playbackFatal.store(true);
        return false;
    }
    if (!pacing::ConfigureSessionTargetFps(start.targetFps))
    {
        log::Error(
            "spectator",
            "presentation pacing contract unavailable fps=%u",
            static_cast<unsigned>(start.targetFps));
        g_playbackError.store("spectator target frame rate is unavailable");
        g_playbackFatal.store(true);
        return false;
    }

    det::RepinFpu();
    if (det::GetCapturedFpuCw() != start.fpuControlWord)
    {
        (void)log::EnableFileLogging();
        log::Error(
            "spectator",
            "FPU control mismatch local=%04X source=%04X",
            static_cast<unsigned>(det::GetCapturedFpuCw()),
            static_cast<unsigned>(start.fpuControlWord));
        g_playbackError.store("spectator FPU control word mismatch");
        g_playbackFatal.store(true);
        return false;
    }
    _mm_setcsr(start.mxcsr);
    det::rng::SetSeed(start.rngSeed);
    det::rng::VisualRestore(start.rngSeed ^ 0xA511E9B3u);
    g_baseSeed = start.rngSeed;
    g_sessionInitialized = true;

    // Never start before GameStart: earlier RNG consumption poisons loading objects.
    netplay::BeginDebugVsHuman(0);
    caster::SetTransportMode(caster::TransportMode::Spectator);
    log::Info(
        "spectator",
        "deterministic init applied seed=%08X fpu=%04X mxcsr=%08X delay=%u rollback=%u fps=%u P1=%s P2=%s",
        start.rngSeed,
        static_cast<unsigned>(start.fpuControlWord),
        start.mxcsr,
        static_cast<unsigned>(start.inputDelay),
        static_cast<unsigned>(start.rollbackFrames),
        static_cast<unsigned>(start.targetFps),
        start.p1Name.data(),
        start.p2Name.data());
    return true;
}

int CatchUpBatchSize(const client::Status& status)
{
    if (status.queuedFrames > 480u)
    {
        return 8;
    }
    if (status.queuedFrames > 240u)
    {
        return 4;
    }
    if (status.queuedFrames > 120u)
    {
        return 2;
    }
    return 1;
}

}

bool Initialize(const session::LauncherSession& launcherSession)
{
    Shutdown();
    {
        std::lock_guard<std::recursive_mutex> lock(g_driveMutex);
        if (g_driveDepth != 0u)
        {
            // Cannot reinitialize from inside a native spectator tick.
            log::Error(
                "spectator",
                "refused re-entrant playback initialization driveThread=%lu depth=%u",
                static_cast<unsigned long>(g_driveThreadId),
                g_driveDepth);
            return false;
        }
    }
    if (!client::Initialize(launcherSession))
    {
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(g_driveMutex);
    g_gameId = 0u;
    g_baseSeed = 0u;
    g_battleGeneration = kNoGeneration;
    g_waitingForStartupBuffer = true;
    g_closePosted.store(false, std::memory_order_release);
    g_closePostedMs.store(0u, std::memory_order_release);
    g_playbackFatal.store(false, std::memory_order_release);
    g_playbackError.store(nullptr, std::memory_order_release);
    g_sessionInitialized = false;
    g_verifiedBattleFrames = 0u;
    g_lastTailWaitLogMs = 0u;
    g_lastCatchUpLogMs = 0u;
    g_screenMismatchSinceMs = 0u;
    g_terminalSeenMs = 0u;
    g_shutdownPending = false;
    caster::SetTransportMode(caster::TransportMode::Spectator);
    g_active.store(true, std::memory_order_release);
    log::Info("spectator", "playback driver armed");
    return true;
}

void Shutdown()
{
    // Publish the stop before waiting: an in-flight Drive checks it after nativeTick.
    g_active.store(false, std::memory_order_release);
    std::lock_guard<std::recursive_mutex> lock(g_driveMutex);
    if (g_driveDepth != 0u)
    {
        // Same-thread reentry: do not reset globals; DriveScope cleans up at depth 0.
        g_shutdownPending = true;
        return;
    }
    FinalizeShutdownLocked();
}

void OnModTick(const HWND gameWindow)
{
    if (!g_active.load(std::memory_order_acquire))
    {
        return;
    }
    client::Poll();
    // FPU/MXCSR are thread-local; Trial/RNG globals must not change on this worker.
    const client::Status status = client::GetStatus();
    if ((status.fatal || g_playbackFatal.load(std::memory_order_acquire))
        && !g_closePosted.exchange(true, std::memory_order_acq_rel))
    {
        const DWORD postedMs = GetTickCount();
        g_closePostedMs.store(postedMs, std::memory_order_release);
        const char* const playbackError = g_playbackError.load(std::memory_order_acquire);
        const char* const error = g_playbackFatal.load(std::memory_order_acquire)
            ? (playbackError != nullptr ? playbackError : "spectator playback failed")
            : status.error.c_str();
        log::Error("spectator", "closing spectator process: %s", error);
        if (gameWindow != nullptr)
        {
            PostMessageA(gameWindow, WM_CLOSE, 0, 0);
        }
    }
    else
    {
        const DWORD closePostedMs = g_closePostedMs.load(std::memory_order_acquire);
        if (g_closePosted.load(std::memory_order_acquire) && closePostedMs != 0u
            && GetTickCount() - closePostedMs >= 1000u)
        {
            log::Error("spectator", "WM_CLOSE timeout; forcing spectator process exit");
            ExitProcess(0);
        }
    }
}

bool IsActive()
{
    return g_active.load(std::memory_order_acquire);
}

bool DriveScreenTick(
    const HWND gameWindow,
    const std::uint8_t hookedScreen,
    ScreenTickFn const nativeTick,
    int* const outResult)
{
    (void)gameWindow;
    if (outResult == nullptr)
    {
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(g_driveMutex);
    if (!IsActive())
    {
        // Shutdown published but not finalized: keep stalling so no native frame leaks.
        if (caster::IsSpectatorActive())
        {
            *outResult = 1;
            return true;
        }
        return false;
    }
    DriveScope driveScope;
    *outResult = 1;
    client::Poll();

    // Only legal caller: FPU/MXCSR are thread-local; RNG/nav/game globals are sim state.
    (void)TryInitializeSessionBeforeTransition();
    if (!IsActive())
    {
        PaceOwnedStall(hookedScreen);
        return true;
    }

    const client::Status transport = client::GetStatus();
    if (transport.fatal || g_playbackFatal.load() || nativeTick == nullptr)
    {
        PaceOwnedStall(hookedScreen);
        return true;
    }

    if (g_gameId == 0u)
    {
        if (!TryStartFrontGame(ReadScreenMode()))
        {
            if (transport.terminal && transport.terminalGameId == 0u
                && transport.terminalRecordIndex == 0u
                && transport.terminalInputDigest == protocol::kInputDigestSeed)
            {
                if (transport.terminalReason == protocol::TerminalReason::ArchiveUnavailable)
                {
                    log::Error(
                        "spectator",
                        "SPECTATOR PLAYBACK INCOMPLETE archive unavailable before initialization");
                    g_playbackError.store("spectator archive was unavailable");
                }
                else
                {
                    log::Info(
                        "spectator",
                        "SPECTATOR PLAYBACK COMPLETE game=0 records=0 digest=%016llX total=%llu",
                        static_cast<unsigned long long>(transport.terminalInputDigest),
                        static_cast<unsigned long long>(transport.terminalTotalConfirmedFrames));
                    g_playbackError.store("spectator source session ended");
                }
                g_playbackFatal.store(true);
                PaceOwnedStall(hookedScreen);
                return true;
            }
            protocol::GameStart pendingStart{};
            if (transport.terminal && !client::PeekGameStart(&pendingStart))
            {
                if (g_terminalSeenMs == 0u)
                {
                    g_terminalSeenMs = GetTickCount();
                }
                if (GetTickCount() - g_terminalSeenMs >= 10000u)
                {
                    g_playbackError.store(
                        "spectator source ended before deterministic initialization arrived");
                    g_playbackFatal.store(true);
                }
                PaceOwnedStall(hookedScreen);
                return true;
            }
            // Native bootstrap must still reach character select; ownership begins at GameStart.
            if (ReadScreenMode() != trial::kScreenCharSelect)
            {
                return false;
            }
            PaceOwnedStall(hookedScreen);
            return true;
        }
    }

    protocol::ConfirmedInput first{};
    if (!client::PeekFrame(&first))
    {
        if (client::AdvanceCompletedGame())
        {
            g_gameId = 0u;
            g_battleGeneration = kNoGeneration;
            (void)TryStartFrontGame(ReadScreenMode());
        }
        const client::Status tail = client::GetStatus();
        if (tail.terminal)
        {
            if (g_terminalSeenMs == 0u)
            {
                g_terminalSeenMs = GetTickCount();
            }
            if (tail.playingGameId == tail.terminalGameId
                && tail.playingRecordIndex == tail.terminalRecordIndex
                && tail.playedInputDigest == tail.terminalInputDigest)
            {
                if (tail.terminalReason == protocol::TerminalReason::ArchiveUnavailable)
                {
                    log::Error(
                        "spectator",
                        "SPECTATOR PLAYBACK INCOMPLETE game=%u records=%u digest=%016llX total=%llu reason=archive-unavailable",
                        tail.terminalGameId,
                        tail.terminalRecordIndex,
                        static_cast<unsigned long long>(tail.terminalInputDigest),
                        static_cast<unsigned long long>(tail.terminalTotalConfirmedFrames));
                    g_playbackError.store("spectator archive ended before the full confirmed tail arrived");
                }
                else
                {
                    log::Info(
                        "spectator",
                        "SPECTATOR PLAYBACK COMPLETE game=%u records=%u digest=%016llX total=%llu",
                        tail.terminalGameId,
                        tail.terminalRecordIndex,
                        static_cast<unsigned long long>(tail.terminalInputDigest),
                        static_cast<unsigned long long>(tail.terminalTotalConfirmedFrames));
                    g_playbackError.store("spectator source session ended");
                }
                g_playbackFatal.store(true);
            }
            else if (tail.queuedFrames == 0u
                && GetTickCount() - g_terminalSeenMs >= 10000u)
            {
                log::Error(
                    "spectator",
                    "terminal tail incomplete play=%u:%u digest=%016llX final=%u:%u digest=%016llX",
                    tail.playingGameId,
                    tail.playingRecordIndex,
                    static_cast<unsigned long long>(tail.playedInputDigest),
                    tail.terminalGameId,
                    tail.terminalRecordIndex,
                    static_cast<unsigned long long>(tail.terminalInputDigest));
                g_playbackError.store("spectator source ended before the confirmed tail arrived");
                g_playbackFatal.store(true);
            }
        }
        const DWORD now = GetTickCount();
        if (now - g_lastTailWaitLogMs >= 1000u)
        {
            g_lastTailWaitLogMs = now;
            const client::Status wait = client::GetStatus();
            log::Info(
                "spectator",
                "live-tail wait play=%u:%u recv=%u:%u fps=%u queuedGames=%zu queuedFrames=%zu connected=%d fatal=%d screen=%u",
                wait.playingGameId,
                wait.playingRecordIndex,
                wait.receivingGameId,
                wait.receivingRecordIndex,
                static_cast<unsigned>(wait.targetFps),
                wait.queuedGames,
                wait.queuedFrames,
                wait.connected ? 1 : 0,
                wait.fatal ? 1 : 0,
                static_cast<unsigned>(ReadScreenMode()));
        }
        PaceOwnedStall(hookedScreen);
        return true;
    }

    const std::uint8_t actualScreen = ReadScreenMode();
    if (first.screen != actualScreen || hookedScreen != actualScreen)
    {
        log::Error(
            "spectator",
            "screen mismatch game=%u record=%u expected=%u actual=%u hook=%u",
            g_gameId,
            first.recordIndex,
            static_cast<unsigned>(first.screen),
            static_cast<unsigned>(actualScreen),
            static_cast<unsigned>(hookedScreen));
        const DWORD now = GetTickCount();
        if (g_screenMismatchSinceMs == 0u)
        {
            g_screenMismatchSinceMs = now;
        }
        if (now - g_screenMismatchSinceMs >= 250u)
        {
            (void)log::EnableFileLogging();
            g_playbackError.store("spectator screen timeline mismatch");
            g_playbackFatal.store(true);
        }
        PaceOwnedStall(hookedScreen);
        return true;
    }
    g_screenMismatchSinceMs = 0u;

    const int requestedBatch = CatchUpBatchSize(transport);
    const std::size_t actualBatch = client::ContiguousFramesForScreen(
        actualScreen,
        static_cast<std::size_t>(requestedBatch));
    const DWORD catchUpNow = GetTickCount();
    if (actualBatch > 1u && catchUpNow - g_lastCatchUpLogMs >= 1000u)
    {
        g_lastCatchUpLogMs = catchUpNow;
        log::Info(
            "spectator",
            "catch-up batch=%zu fps=%u queuedFrames=%zu visibleTicks=1 hiddenTicks=%zu",
            actualBatch,
            static_cast<unsigned>(transport.targetFps),
            transport.queuedFrames,
            actualBatch - 1u);
    }
    const std::uint32_t pacingAnchor = pacing::CaptureNativeFrameWaitAnchor();
    int result = 1;
    int advanced = 0;
    for (std::size_t index = 0u; index < actualBatch; ++index)
    {
        protocol::ConfirmedInput frame{};
        if (!client::PeekFrame(&frame) || frame.screen != ReadScreenMode())
        {
            break;
        }

        const bool hidden = IsHiddenCatchUpTick(index, actualBatch);
        SetPresentSuppressed(hidden);
        SetRollbackAudioFrame(frame.sourceFrame, true);
        if (hidden)
        {
            pacing::BypassNextNativeFrameWait();
        }
        else
        {
            pacing::RestoreNextNativeFrameWaitAnchor(pacingAnchor);
        }

        caster::SetSpectatorInputWords(frame.p1Word, frame.p2Word);
        // Force the human path before the snapshot to match the peers' hashes.
        MaintainNetplayCpuFlags();
        if (frame.screen == trial::kScreenCharSelect)
        {
            PrepareCharSelectStage();
        }
        else if (frame.hasRegionHashes != 0u)
        {
            // Reseed at the first hashed generation, not at battle start.
            PrepareBattleGeneration(frame.sourceGeneration);
            det::ResetFpuForSimulation();
            if (!VerifyConfirmedMatchState(frame))
            {
                g_playbackError.store("confirmed spectator state hash mismatch");
                g_playbackFatal.store(true);
                break;
            }
        }

        // Verify before applying F4/F5: practice fields are hashed.
        if (frame.screen == trial::kScreenBattle)
        {
            const std::uint8_t practiceState = menu::FilterRecordedSystemState(
                static_cast<std::uint8_t>((frame.p1Word >> 8u) | (frame.p2Word >> 8u)));
            if (!menu::ApplyTrialPracticeSystemState(practiceState))
            {
                g_playbackError.store("spectator practice system-state target was unavailable");
                g_playbackFatal.store(true);
                break;
            }
            if ((practiceState & menu::kOnlinePracticeSystemMask) != 0u)
            {
                lifecycle::Input(
                    "spectator_system",
                    "apply confirmed practice state=0x%02X source=%u generation=%u",
                    static_cast<unsigned>(practiceState),
                    frame.sourceFrame,
                    frame.sourceGeneration);
            }
        }

        {
            // Hidden catch-up tick: keep EFZ's FPS latch on the visible timeline.
            pacing::ScopedNativeFpsAccountingIsolation fpsAccounting(hidden);
            result = nativeTick(gameWindow);
        }
        // Shutdown during nativeTick: no commit; leave cursor and globals for DriveScope.
        if (!IsActive())
        {
            break;
        }
        if (!client::ConsumeFrame(g_gameId, frame.recordIndex))
        {
            (void)log::EnableFileLogging();
            log::Error(
                "spectator",
                "consume failed game=%u record=%u",
                g_gameId,
                frame.recordIndex);
            g_playbackError.store("spectator playback cursor mismatch");
            g_playbackFatal.store(true);
            break;
        }
        ++advanced;
        g_terminalSeenMs = 0u;
        if (ReadScreenMode() != frame.screen)
        {
            break;
        }
    }

    SetPresentSuppressed(false);
    SetRollbackAudioFrame(0u, false);
    *outResult = result;
    if (advanced == 0)
    {
        PaceOwnedStall(hookedScreen);
    }
    return true;
}

}
