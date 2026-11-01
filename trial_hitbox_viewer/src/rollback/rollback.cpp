#include "rollback.h"

#include "caster_input.h"
#include "caster_session.h"
#include "control_swap.h"
#include "debug_log.h"
#include "desync_check.h"
#include "lifecycle_log.h"
#include "framestep.h"
#include "netplay_delay.h"
#include "netplay_lockstep.h"
#include "netplay_sync.h"
#include "rollback_engine.h"
#include "trial_addresses.h"
#include "trial_protocol.h"
#include "watchdog.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

namespace thv::rollback
{
namespace
{

// Recursive: the battle tick's input poll re-enters rollback::* on the same thread.
std::recursive_mutex g_mutex;
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_enabled{kDefaultEnabled};
std::atomic<bool> g_netplayArmed{false};
std::atomic<bool> g_netplaySessionStarted{false};
std::uint32_t g_handoffWaitStartMs = 0u;
std::uint32_t g_handoffWaitLogMs = 0u;
Engine g_engine;

}

bool IsEnabled()
{
    return g_enabled.load();
}

bool IsArmedForNetplay()
{
    return g_netplayArmed.load();
}

bool IsBattleIntroBoundaryPending()
{
    const auto* const mode = static_cast<const std::uint8_t*>(
        trial::ResolveVa(trial::kVaBattleScreenMode));
    const auto* const subState = static_cast<const std::uint32_t*>(
        trial::ResolveVa(trial::kVaScreenSubState));
    const auto* const counter = static_cast<const std::int32_t*>(
        trial::ResolveVa(trial::kVaScreenAnimCounter));
    return mode != nullptr && subState != nullptr && counter != nullptr
        && *mode == trial::kScreenLoading
        && *subState == 2u
        && *counter == trial::kBattleIntroFirstFrameCounter;
}

void ArmForNetplay()
{
    g_netplayArmed.store(true);
    g_netplaySessionStarted.store(false);
    g_handoffWaitStartMs = 0u;
    g_handoffWaitLogMs = 0u;
    SetEnabled(false);
    lifecycle::Phase("rollback", "idle", "armed", "lockstep until battle");
    log::Info("rollback", "armed for netplay — lockstep until battle, rollback activates at handoff");
}

void DisarmNetplay()
{
    g_netplayArmed.store(false);
    g_netplaySessionStarted.store(false);
}

void StartNetplaySession()
{
    if (!g_netplayArmed.load())
    {
        return;
    }

    netplay::lockstep::AlignForBattleHandoff();
    netplay::lockstep::HandoffStatus handoff = netplay::lockstep::EvaluateHandoff();

    netplay::SendBattleEntry(handoff.simFrame, handoff.ready, proto::kBattleEntryEpochHandoff);
    std::uint32_t peerEntryFrame = 0;
    bool peerEntryReady = false;
    const bool peerEntryKnown =
        netplay::GetPeerBattleEntry(&peerEntryFrame, &peerEntryReady, proto::kBattleEntryEpochHandoff);
    if (handoff.ready && peerEntryKnown && peerEntryReady && peerEntryFrame < handoff.simFrame)
    {
        netplay::lockstep::TrimForBattleEntry(peerEntryFrame);
        handoff = netplay::lockstep::EvaluateHandoff();
        netplay::SendBattleEntry(
            handoff.simFrame, handoff.ready, proto::kBattleEntryEpochHandoff,true);
    }
    const bool entryAligned =
        peerEntryKnown && peerEntryReady && peerEntryFrame == handoff.simFrame;

    // Never fall through with unequal frontiers.
    const std::uint32_t kHandoffWaitTimeoutMs = static_cast<std::uint32_t>(
        std::clamp<std::uint64_t>(
            5000ull + static_cast<std::uint64_t>(netplay::lockstep::GetPingMs()) * 8ull,
            10000ull,
            60000ull));
    const std::uint32_t nowMs = static_cast<std::uint32_t>(GetTickCount());
    if (g_handoffWaitStartMs == 0u)
    {
        g_handoffWaitStartMs = nowMs;
    }
    const bool timedOut = (nowMs - g_handoffWaitStartMs) >= kHandoffWaitTimeoutMs;

    if (!(handoff.ready && entryAligned))
    {
        if (nowMs - g_handoffWaitLogMs >= 250u)
        {
            g_handoffWaitLogMs = nowMs;
            lifecycle::Sync(
                "handoff",
                "barrier waiting (%s) sim=%u local=%zu remote=%zu peer[known=%d ready=%d frame=%u]",
                netplay::lockstep::HandoffBlockReasonName(handoff.blockReason),
                handoff.simFrame,
                handoff.localSize,
                handoff.remoteSize,
                peerEntryKnown ? 1 : 0,
                peerEntryReady ? 1 : 0,
                peerEntryFrame);
            log::Info(
                "rollback",
                "handoff barrier: waiting (%s) sim=%u local=%zu remote=%zu peer[known=%d ready=%d frame=%u]",
                netplay::lockstep::HandoffBlockReasonName(handoff.blockReason),
                handoff.simFrame,
                handoff.localSize,
                handoff.remoteSize,
                peerEntryKnown ? 1 : 0,
                peerEntryReady ? 1 : 0,
                peerEntryFrame);
        }
        if (timedOut)
        {
            lifecycle::Fault(
                "handoff",
                "barrier timeout (%s entryAligned=%d peerFrame=%u) after %ums — aborting without import",
                netplay::lockstep::HandoffBlockReasonName(handoff.blockReason),
                entryAligned ? 1 : 0,
                peerEntryFrame,
                kHandoffWaitTimeoutMs);
            netplay::AbortNetplaySession("battle-entry alignment timed out");
        }
        return;
    }

    g_handoffWaitStartMs = 0u;
    g_handoffWaitLogMs = 0u;
    g_netplaySessionStarted.store(true);
    netplay::lockstep::LogHandoffTransition(handoff);
    log::Step(
        "rollback",
        "handoff barrier ready — both peers entering battle at frame %u (local=%zu remote=%zu)",
        handoff.simFrame,
        handoff.localSize,
        handoff.remoteSize);

    std::vector<std::uint8_t> localWire;
    std::vector<std::uint8_t> remoteWire;
    netplay::lockstep::ExportWireHistories(&localWire, &remoteWire);
    const std::uint32_t simFrame = netplay::lockstep::GetSimFrame();
    const std::uint32_t pingMs = netplay::lockstep::GetPingMs();

    // Quarantine delayed frontend datagrams before importing the trimmed histories.
    netplay::CommitBattleEntry(handoff.simFrame, proto::kBattleEntryEpochHandoff);

    int configuredDelay = netplay::GetNegotiatedInputDelay();
    if (configuredDelay < 0)
    {
        configuredDelay = 0;
        if (const session::LauncherSession* launcherSession = session::GetCachedSession())
        {
            configuredDelay = launcherSession->inputDelay;
        }
    }

    int predictionWindow = netplay::GetNegotiatedRollbackFrames();
    if (predictionWindow < 0)
    {
        predictionWindow = 6;
        if (const session::LauncherSession* launcherSession = session::GetCachedSession())
        {
            predictionWindow = launcherSession->rollbackFrames;
        }
    }

    {
        std::lock_guard lock(g_mutex);
        g_engine.Reset();
        g_engine.SetLocalSide(caster::IsNetplayActive() ? caster::GetLocalSide() : GetHumanSide());
        g_engine.SetInputDelay(configuredDelay);
        g_engine.SetPingMs(pingMs);
        g_engine.SetPredictionWindow(predictionWindow);
        // Reset clears InitSnapshot; restore the negotiated seed before import.
        g_engine.SetRngSeed(netplay::GetSyncedRngSeed());
        g_engine.ImportWireHistories(localWire, remoteWire, simFrame);
    }

    g_netplayArmed.store(false);
    SetEnabled(true);
    lifecycle::Phase("rollback", "lockstep", "battle_session", "handoff");
    log::Step(
        "rollback",
        "netplay session started at battle handoff sim=%u local=%zu remote=%zu",
        simFrame,
        localWire.size(),
        remoteWire.size());
}

namespace
{

void FlushSyncAfterMainTick()
{
    if (caster::IsNetplayActive())
    {
        netplay::SendLocalSyncFrame();
    }
}

}

bool ShouldDriveFrontendTick()
{
    if (!g_enabled.load() || !caster::IsNetplayActive())
    {
        return false;
    }

    const int memorial = Session::MapMemorialScreenIndexFromGame();
    return memorial == 0 || memorial == 1 || memorial == 2
        || Session::IsPostMatchScreenFromGame();
}

void PrepareStageForNativeLoad()
{
    if (!caster::IsNetplayActive() || !netplay::IsDeterminismSynced()
        || Session::MapMemorialScreenIndexFromGame() != 1)
    {
        return;
    }

    const auto* const anim = static_cast<const std::int32_t*>(
        trial::ResolveVa(trial::kVaScreenAnimCounter));
    auto* const stage = static_cast<std::uint8_t*>(
        trial::ResolveVa(trial::kVaCharSelectStage));
    if (anim == nullptr || stage == nullptr || *anim < trial::kNativeTransitionNearComplete)
    {
        return;
    }

    std::uint8_t syncedStage = static_cast<std::uint8_t>(netplay::GetSyncedRngSeed() % 4u);
    if (syncedStage == 2u)
    {
        syncedStage = 0u;
    }
    if (syncedStage == 1u)
    {
        syncedStage = 3u;
    }
    if (*stage != syncedStage)
    {
        log::Step(
            "rollback",
            "pre-load stage sync %u -> %u",
            static_cast<unsigned>(*stage),
            static_cast<unsigned>(syncedStage));
        *stage = syncedStage;
    }
}

bool ShouldDriveBattleTick()
{
    if (!g_enabled.load() || !caster::IsNetplayActive())
    {
        return false;
    }

    return MapMemorialScreenIndexFromGame() == 3;
}

void SetEnabled(bool enabled)
{
    const bool was = g_enabled.exchange(enabled);
    if (was != enabled)
    {
        lifecycle::Phase("rollback", was ? "enabled" : "disabled", enabled ? "enabled" : "disabled", "SetEnabled");
        log::Info("rollback", "engine %s", enabled ? "ENABLED" : "DISABLED");
        if (enabled)
        {
            std::lock_guard lock(g_mutex);
            g_engine.SetLocalSide(GetHumanSide());
        }
        else if (!g_netplaySessionStarted.load())
        {
            std::lock_guard lock(g_mutex);
            g_engine.Reset();
        }
    }
}

bool Initialize()
{
    if (g_initialized.exchange(true))
    {
        return true;
    }

    g_engine.Reset();
    g_engine.SetLocalSide(GetHumanSide());
    g_enabled.store(kDefaultEnabled);

    log::Info(
        "rollback",
        "module loaded (rollback engine, %s)",
        kDefaultEnabled ? "enabled" : "disabled — call SetEnabled(true) to activate");
    return true;
}

void Shutdown()
{
    if (!g_initialized.exchange(false))
    {
        return;
    }

    DisarmNetplay();
    g_enabled.store(false);
    std::lock_guard lock(g_mutex);
    g_engine.Reset();
    log::Info("rollback", "module shutdown");
}

void OnModTick()
{
    // Intentionally inert: a second MainTick/Poll owner races histories and teardown.
}

void RunBattleTick(HWND gameWindow)
{
    if (!g_initialized.load() || !g_enabled.load())
    {
        return;
    }

    if (IsFramestepPaused())
    {
        return;
    }

    if (caster::IsNetplayActive())
    {
        netplay::Poll();
        if (netplay::IsNetplayAborted())
        {
            return;
        }
    }
    {
        std::lock_guard lock(g_mutex);
        g_engine.SetGameWindow(gameWindow);
        g_engine.SetLocalSide(caster::IsNetplayActive() ? caster::GetLocalSide() : GetHumanSide());
        watchdog::ScopedPhase phase("rollback_main_tick");
        g_engine.MainTick();
    }
    if (caster::IsNetplayActive() && desync::ResimDivergences() != 0u)
    {
        netplay::AbortNetplaySession("local rollback re-simulation diverged");
        return;
    }
    FlushSyncAfterMainTick();
}

void RunFrontendRollbackTick(HWND gameWindow)
{
    if (!g_initialized.load() || !g_enabled.load())
    {
        return;
    }

    if (IsFramestepPaused())
    {
        return;
    }

    if (caster::IsNetplayActive())
    {
        netplay::Poll();
        if (netplay::IsNetplayAborted())
        {
            return;
        }
    }
    {
        std::lock_guard lock(g_mutex);
        g_engine.SetGameWindow(gameWindow);
        g_engine.SetLocalSide(caster::IsNetplayActive() ? caster::GetLocalSide() : GetHumanSide());
        g_engine.MainTick();
    }
    if (caster::IsNetplayActive() && desync::ResimDivergences() != 0u)
    {
        netplay::AbortNetplaySession("local rollback re-simulation diverged");
        return;
    }
    FlushSyncAfterMainTick();
}

void OnBattleFrameBoundary(HWND gameWindow)
{
    if (!g_initialized.load() || !g_enabled.load())
    {
        return;
    }

    std::lock_guard lock(g_mutex);
    g_engine.OnBattleFrameBoundary(gameWindow);
}

void SetInputDelay(int frames)
{
    std::lock_guard lock(g_mutex);
    g_engine.SetInputDelay(frames);
}

void SetPingMs(const std::uint32_t pingMs)
{
    std::lock_guard lock(g_mutex);
    g_engine.SetPingMs(pingMs);
}

void SetPredictionWindow(const int frames)
{
    std::lock_guard lock(g_mutex);
    g_engine.SetPredictionWindow(frames);
}

void SetLocalSide(int side)
{
    std::lock_guard lock(g_mutex);
    g_engine.SetLocalSide(side);
}

void SetRngSeed(const std::uint32_t seed)
{
    std::lock_guard lock(g_mutex);
    g_engine.SetRngSeed(seed);
}

void PushLocalInput(std::uint8_t wireMask)
{
    if (!g_initialized.load() || !g_enabled.load())
    {
        return;
    }

    std::lock_guard lock(g_mutex);
    g_engine.PushLocalInput(wireMask);
}

void PushRemoteInput(std::uint8_t wireMask)
{
    if (!g_initialized.load() || !g_enabled.load())
    {
        return;
    }

    std::lock_guard lock(g_mutex);
    g_engine.PushRemoteInput(wireMask);
}

void SetHostCallbacks(
    int(__cdecl* advanceBattleTick)(HWND hWnd),
    int(__cdecl* advanceLoadingTick)(HWND hWnd),
    int(__cdecl* advanceCharSelectTick)(HWND hWnd),
    int(__cdecl* advanceMenuTick)(HWND hWnd),
    int(__cdecl* advanceTitleTick)(HWND hWnd),
    HWND gameWindow)
{
    HostCallbacks callbacks{};
    callbacks.advanceBattleTick = advanceBattleTick;
    callbacks.advanceLoadingTick = advanceLoadingTick;
    callbacks.advanceCharSelectTick = advanceCharSelectTick;
    callbacks.advanceMenuTick = advanceMenuTick;
    callbacks.advanceTitleTick = advanceTitleTick;
    callbacks.gameWindow = gameWindow;

    std::lock_guard lock(g_mutex);
    g_engine.SetHostCallbacks(callbacks);
}

Status GetStatus()
{
    Status status{};
    status.compiledIn = kCompiledIn;
    status.enabled = g_enabled.load();

    if (!g_initialized.load())
    {
        return status;
    }

    std::lock_guard lock(g_mutex);
    status = g_engine.BuildStatus();
    status.enabled = g_enabled.load();
    return status;
}

bool IsInputAuthorityActive()
{
    if (!g_initialized.load() || !g_enabled.load())
    {
        return false;
    }

    std::lock_guard lock(g_mutex);
    return g_engine.IsInputAuthorityActive();
}

bool ShouldEngineDriveBattleTick()
{
    return ShouldDriveBattleTick();
}

bool TryGetWireMaskForPlayerSide(int playerSide, std::uint8_t* outMask)
{
    if (!g_initialized.load() || !g_enabled.load())
    {
        return false;
    }

    std::lock_guard lock(g_mutex);
    return g_engine.TryGetWireMaskForPlayerSide(playerSide, outMask);
}

std::uint8_t GetMergedSystemState()
{
    if (!g_initialized.load() || !g_enabled.load())
    {
        return 0u;
    }

    std::lock_guard lock(g_mutex);
    return g_engine.GetMergedSystemState();
}

void AcknowledgeSystemStateApplied(const std::uint8_t state)
{
    if (!g_initialized.load() || !g_enabled.load())
    {
        return;
    }

    std::lock_guard lock(g_mutex);
    g_engine.AcknowledgeSystemStateApplied(state);
}

int GetMemorialScreenIndex()
{
    if (!g_initialized.load())
    {
        return -1;
    }

    std::lock_guard lock(g_mutex);
    return g_engine.GetSession().MapMemorialScreenIndex();
}

int MapMemorialScreenIndexFromGame()
{
    return Session::MapMemorialScreenIndexFromGame();
}

bool IsPostMatchScreen()
{
    return Session::IsPostMatchScreenFromGame();
}

int GetLastMainTickIterations()
{
    if (!g_initialized.load() || !g_enabled.load())
    {
        return 0;
    }

    std::lock_guard lock(g_mutex);
    return g_engine.GetLastMainTickIterations();
}

void FillSyncDataPacket(proto::SyncDataPacket* out)
{
    if (out == nullptr || !g_initialized.load() || !g_enabled.load())
    {
        return;
    }

    std::lock_guard lock(g_mutex);
    g_engine.FillSyncDataPacket(out);
}

bool ApplySyncDataPacket(const proto::SyncDataPacket& packet)
{
    if (!g_initialized.load() || !g_enabled.load())
    {
        return false;
    }

    std::lock_guard lock(g_mutex);
    return g_engine.ApplySyncDataPacket(packet);
}

}
