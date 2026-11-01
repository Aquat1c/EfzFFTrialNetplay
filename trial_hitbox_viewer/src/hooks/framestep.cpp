#include "framestep.h"
#include "caster_input.h"
#include "control_config.h"
#include "caster_telemetry.h"
#include "control_swap.h"
#include "debug_log.h"
#include "determinism.h"
#include "game_pacing.h"
#include "lifecycle_log.h"
#include "netplay_frame_gate.h"
#include "netplay_sync.h"
#include "rollback.h"
#include "savestate.h"
#include "sdl_input.h"
#include "spectator_player.h"
#include "trial_addresses.h"
#include "watchdog.h"
#include "window_telemetry.h"

#include <MinHook.h>

#include <windows.h>

#include <atomic>

namespace thv
{
namespace
{

using BattleTickFn = int(__cdecl*)(HWND hWnd);
using ScreenTickFn = int(__cdecl*)(HWND hWnd);
BattleTickFn g_originalBattleTick = nullptr;
ScreenTickFn g_originalCharSelectTick = nullptr;
ScreenTickFn g_originalLoadingTick = nullptr;
ScreenTickFn g_originalMenuTick = nullptr;
ScreenTickFn g_originalTitleTick = nullptr;

std::atomic<bool> g_initialized{false};
std::atomic<bool> g_paused{false};
std::atomic<bool> g_stepRequested{false};
std::atomic<HWND> g_lastGameWindow{nullptr};

thread_local int g_tickDepth = 0;
thread_local bool g_pumping = false;

struct TickDepthGuard
{
    TickDepthGuard() { ++g_tickDepth; }
    ~TickDepthGuard() { --g_tickDepth; }
    bool reentered() const { return g_tickDepth > 1; }
};

enum class TickPath : std::uint8_t
{
    Vanilla,
    LockstepGate,
    RollbackFrontend,
    RollbackBattle,
    LockstepBattle,
};

void MaybeLogTickPath(const TickPath path)
{
    static TickPath s_last = TickPath::Vanilla;
    if (path == s_last)
    {
        return;
    }

    const char* name = "?";
    switch (path)
    {
    case TickPath::Vanilla:
        name = "vanilla";
        break;
    case TickPath::LockstepGate:
        name = "lockstep_gate";
        break;
    case TickPath::RollbackFrontend:
        name = "rollback_frontend";
        break;
    case TickPath::RollbackBattle:
        name = "rollback_battle";
        break;
    case TickPath::LockstepBattle:
        name = "lockstep_battle";
        break;
    }

    lifecycle::Netplay("tick path -> %s", name);
    s_last = path;
}

void PumpWindowMessages()
{
    if (g_pumping)
    {
        // Re-entered from a dispatched message: never nest a second DispatchMessageA loop.
        static thread_local DWORD s_lastLog = 0;
        const DWORD now = GetTickCount();
        if (now - s_lastLog > 1000)
        {
            lifecycle::Netplay("reentrant PumpWindowMessages skipped (nested dispatch)");
            s_lastLog = now;
        }
        return;
    }

    g_pumping = true;
    MSG msg = {};
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            // PeekMessage(PM_REMOVE) would steal the quit from EFZ's WinMain loop; preserve it.
            PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    g_pumping = false;
}

bool HoldGameThreadForOnlineExit(const bool battle)
{
    if (!window_telemetry::QuiesceGameThreadForOnlineExit())
    {
        return false;
    }

    // Pump only: the worker owns input polling, network Poll and simulation here.
    PumpWindowMessages();
    if (battle)
    {
        pacing::ConsumeBattleFrameSlot();
    }
    else
    {
        pacing::ConsumeActiveScreenFrameSlot();
    }
    return true;
}

// Netplay callers suppress a nested tick: it would advance outside the synced timeline.
bool BypassReentrantTick(const TickDepthGuard& guard, const char* label)
{
    if (!guard.reentered())
    {
        return false;
    }
    static thread_local DWORD s_lastLog = 0;
    const DWORD now = GetTickCount();
    if (now - s_lastLog > 1000)
    {
        lifecycle::Netplay("REENTRANT %s tick depth=%d — vanilla passthrough", label, g_tickDepth);
        s_lastLog = now;
    }
    return true;
}

bool RunNetplayFrontendTickGate()
{
    if (!caster::IsNetplayActive() || rollback::IsEnabled())
    {
        return false;
    }

    netplay::Poll();
    if (netplay::IsNetplayAborted())
    {
        MaybeLogTickPath(TickPath::LockstepGate);
        PumpWindowMessages();
        pacing::ConsumeActiveScreenFrameSlot();
        return true;
    }
    if (netplay::ShouldStallLockstepFrame())
    {
        MaybeLogTickPath(TickPath::LockstepGate);
        netplay::Poll();
        caster::RefreshLockstepWireInject();
        PumpWindowMessages();
        pacing::ConsumeActiveScreenFrameSlot();
        return true;
    }

    caster::OnNetplayPollFrame();
    MaintainNetplayCpuFlags();
    MaybeLogTickPath(TickPath::Vanilla);
    return false;
}

bool RunRollbackFrontendTick(HWND hWnd)
{
    if (!rollback::ShouldDriveFrontendTick())
    {
        return false;
    }

    g_lastGameWindow.store(hWnd);
    MaintainNetplayCpuFlags();
    MaybeLogTickPath(TickPath::RollbackFrontend);
    rollback::RunFrontendRollbackTick(hWnd);

    // No native tick ran, so pace here; when MainTick advanced, the native handler paced.
    PumpWindowMessages();
    if (rollback::GetLastMainTickIterations() <= 0)
    {
        pacing::ConsumeActiveScreenFrameSlot();
    }
    return true;
}

int __cdecl HookedCharSelectTick(HWND hWnd)
{
    TickDepthGuard tickGuard;
    if (BypassReentrantTick(tickGuard, "charselect"))
    {
        if (caster::IsNetplayActive() || caster::IsSpectatorActive())
        {
            return 1;
        }
        return g_originalCharSelectTick != nullptr ? g_originalCharSelectTick(hWnd) : 1;
    }

    pacing::PaceVisibleFrame();
    g_lastGameWindow.store(hWnd);
    watchdog::MarkGameThread();
    if (HoldGameThreadForOnlineExit(false))
    {
        return 1;
    }
    rollback::PrepareStageForNativeLoad();
    int spectatorResult = 1;
    if (spectator::player::DriveScreenTick(
            hWnd,
            trial::kScreenCharSelect,
            g_originalCharSelectTick,
            &spectatorResult))
    {
        return spectatorResult;
    }
    if (RunRollbackFrontendTick(hWnd))
    {
        return 1;
    }

    if (RunNetplayFrontendTickGate())
    {
        return 1;
    }

    return g_originalCharSelectTick != nullptr ? g_originalCharSelectTick(hWnd) : 1;
}

int __cdecl HookedMenuTick(HWND hWnd)
{
    TickDepthGuard tickGuard;
    if (BypassReentrantTick(tickGuard, "menu"))
    {
        if (caster::IsNetplayActive() || caster::IsSpectatorActive())
        {
            return 1;
        }
        return g_originalMenuTick != nullptr ? g_originalMenuTick(hWnd) : 1;
    }

    pacing::PaceVisibleFrame();
    g_lastGameWindow.store(hWnd);
    watchdog::MarkGameThread();
    if (HoldGameThreadForOnlineExit(false))
    {
        return 1;
    }
    int spectatorResult = 1;
    if (spectator::player::DriveScreenTick(
            hWnd,
            trial::kScreenMainMenu,
            g_originalMenuTick,
            &spectatorResult))
    {
        return spectatorResult;
    }
    if (RunRollbackFrontendTick(hWnd) || RunNetplayFrontendTickGate())
    {
        return 1;
    }
    return g_originalMenuTick != nullptr ? g_originalMenuTick(hWnd) : 1;
}

int __cdecl HookedTitleTick(HWND hWnd)
{
    TickDepthGuard tickGuard;
    if (BypassReentrantTick(tickGuard, "title"))
    {
        if (caster::IsNetplayActive() || caster::IsSpectatorActive())
        {
            return 1;
        }
        return g_originalTitleTick != nullptr ? g_originalTitleTick(hWnd) : 1;
    }

    pacing::PaceVisibleFrame();
    g_lastGameWindow.store(hWnd);
    watchdog::MarkGameThread();
    if (HoldGameThreadForOnlineExit(false))
    {
        return 1;
    }
    int spectatorResult = 1;
    if (spectator::player::DriveScreenTick(
            hWnd,
            trial::kScreenTitle,
            g_originalTitleTick,
            &spectatorResult))
    {
        return spectatorResult;
    }
    if (RunRollbackFrontendTick(hWnd) || RunNetplayFrontendTickGate())
    {
        return 1;
    }
    return g_originalTitleTick != nullptr ? g_originalTitleTick(hWnd) : 1;
}

int __cdecl HookedLoadingTick(HWND hWnd)
{
    TickDepthGuard tickGuard;
    if (BypassReentrantTick(tickGuard, "loading"))
    {
        if (caster::IsNetplayActive() || caster::IsSpectatorActive())
        {
            return 1;
        }
        return g_originalLoadingTick != nullptr ? g_originalLoadingTick(hWnd) : 1;
    }

    pacing::PaceVisibleFrame();
    g_lastGameWindow.store(hWnd);
    watchdog::MarkGameThread();
    if (HoldGameThreadForOnlineExit(false))
    {
        return 1;
    }
    int spectatorResult = 1;
    if (spectator::player::DriveScreenTick(
            hWnd,
            trial::kScreenLoading,
            g_originalLoadingTick,
            &spectatorResult))
    {
        return spectatorResult;
    }

    // Hold so the barrier imports the confirmed loading prefix before capture claims a frame.
    if (caster::IsNetplayActive() && rollback::IsArmedForNetplay()
        && rollback::IsBattleIntroBoundaryPending())
    {
        netplay::Poll();
        rollback::StartNetplaySession();
        if (!rollback::IsEnabled())
        {
            MaybeLogTickPath(TickPath::LockstepGate);
            caster::RefreshLockstepWireInject();
            PumpWindowMessages();
            pacing::ConsumeActiveScreenFrameSlot();
            return 1;
        }
    }
    if (RunRollbackFrontendTick(hWnd))
    {
        return 1;
    }

    if (RunNetplayFrontendTickGate())
    {
        return 1;
    }

    return g_originalLoadingTick != nullptr ? g_originalLoadingTick(hWnd) : 1;
}

int __cdecl HookedBattleTick(HWND hWnd)
{
    TickDepthGuard tickGuard;
    if (BypassReentrantTick(tickGuard, "battle"))
    {
        if (caster::IsNetplayActive() || caster::IsSpectatorActive())
        {
            return 1;
        }
        return g_originalBattleTick != nullptr ? g_originalBattleTick(hWnd) : 1;
    }

    pacing::PaceVisibleFrame();
    g_lastGameWindow.store(hWnd);
    watchdog::MarkGameThread();
    if (HoldGameThreadForOnlineExit(true))
    {
        return 1;
    }

    int spectatorResult = 1;
    if (spectator::player::DriveScreenTick(
            hWnd,
            trial::kScreenBattle,
            g_originalBattleTick,
            &spectatorResult))
    {
        return spectatorResult;
    }

    ProcessSavestateAtFrameBoundary();

    if (caster::IsNetplayActive() && !rollback::ShouldEngineDriveBattleTick())
    {
        netplay::Poll();
        if (netplay::IsNetplayAborted())
        {
            PumpWindowMessages();
            pacing::ConsumeBattleFrameSlot();
            return 1;
        }
    }

    MaintainNetplayCpuFlags();

    if (netplay::ShouldStallLockstepFrame())
    {
        MaybeLogTickPath(TickPath::LockstepBattle);
        caster::RefreshLockstepWireInject();
        PumpWindowMessages();
        pacing::ConsumeBattleFrameSlot();
        return 1;
    }

    rollback::OnBattleFrameBoundary(hWnd);

    // Hold the native battle tick until the rollback handoff engages.
    if (caster::IsNetplayActive()
        && rollback::IsArmedForNetplay()
        && rollback::MapMemorialScreenIndexFromGame() == 3
        && !rollback::ShouldEngineDriveBattleTick())
    {
        MaybeLogTickPath(TickPath::LockstepBattle);
        caster::RefreshLockstepWireInject();
        PumpWindowMessages();
        pacing::ConsumeBattleFrameSlot();
        return 1;
    }

    if (rollback::ShouldEngineDriveBattleTick())
    {
        MaybeLogTickPath(TickPath::RollbackBattle);
        if (g_paused.load())
        {
            if (!ConsumeFramestepRequest())
            {
                PumpWindowMessages();
                return 1;
            }
        }

        det::OnEngineFrameBegin();
        telemetry::OnBattleFrameBegin();
        rollback::RunBattleTick(hWnd);
        if (rollback::GetLastMainTickIterations() <= 0)
        {
            // The self-pacing native tick did not run: pace here or WinMain spins at 100% CPU.
            pacing::ConsumeBattleFrameSlot();
        }
        PumpWindowMessages();
        return 1;
    }

    caster::OnBattleFrameInput();
    if (caster::IsNetplayActive() && !rollback::ShouldEngineDriveBattleTick())
    {
        MaybeLogTickPath(TickPath::Vanilla);
    }

    if (g_paused.load())
    {
        if (!ConsumeFramestepRequest())
        {
            PumpWindowMessages();
            return 1;
        }
    }

    det::OnEngineFrameBegin();
    telemetry::OnBattleFrameBegin();
    return g_originalBattleTick != nullptr ? g_originalBattleTick(hWnd) : 1;
}

}

bool InitializeFramestep()
{
    if (g_initialized.exchange(true))
    {
        return true;
    }

    struct HookSpec
    {
        std::uintptr_t va;
        void* hook;
        void** original;
        const char* label;
    };

    const HookSpec specs[] = {
        {trial::kVaBattleTick, reinterpret_cast<void*>(&HookedBattleTick), reinterpret_cast<void**>(&g_originalBattleTick), "battle tick"},
        {trial::kVaCharSelectTick, reinterpret_cast<void*>(&HookedCharSelectTick), reinterpret_cast<void**>(&g_originalCharSelectTick), "char select tick"},
        {trial::kVaLoadingTick, reinterpret_cast<void*>(&HookedLoadingTick), reinterpret_cast<void**>(&g_originalLoadingTick), "loading tick"},
        {trial::kVaMenuTick, reinterpret_cast<void*>(&HookedMenuTick), reinterpret_cast<void**>(&g_originalMenuTick), "menu tick"},
        {trial::kVaTitleTick, reinterpret_cast<void*>(&HookedTitleTick), reinterpret_cast<void**>(&g_originalTitleTick), "title tick"},
    };

    for (const HookSpec& spec : specs)
    {
        void* const target = trial::ResolveVa(spec.va);
        if (target == nullptr)
        {
            lifecycle::Fault("hooks", "%s target unresolved", spec.label);
            log::Error("hooks", "%s target unresolved", spec.label);
            g_initialized = false;
            return false;
        }

        if (MH_CreateHook(target, spec.hook, spec.original) != MH_OK)
        {
            lifecycle::Fault("hooks", "%s hook create failed", spec.label);
            log::Error("hooks", "%s hook create failed", spec.label);
            g_initialized = false;
            return false;
        }

        if (MH_QueueEnableHook(target) != MH_OK)
        {
            lifecycle::Fault("hooks", "%s hook queue-enable failed", spec.label);
            log::Error("hooks", "%s hook queue-enable failed", spec.label);
            g_initialized = false;
            return false;
        }

        log::Info(
            "hooks",
            "%s hooked EFZ+0x%05X",
            spec.label,
            static_cast<unsigned>(trial::VaToRva(spec.va)));
    }

    lifecycle::Init("hooks", "screen tick hooks installed (battle/loading/charselect/menu/title)");
    rollback::SetHostCallbacks(
        g_originalBattleTick,
        g_originalLoadingTick,
        g_originalCharSelectTick,
        g_originalMenuTick,
        g_originalTitleTick,
        nullptr);
    return true;
}

void ShutdownFramestep()
{
    if (!g_initialized.exchange(false))
    {
        return;
    }

    g_paused = false;
    g_stepRequested = false;

    const std::uintptr_t targets[] = {
        trial::kVaBattleTick,
        trial::kVaCharSelectTick,
        trial::kVaLoadingTick,
        trial::kVaMenuTick,
        trial::kVaTitleTick,
    };

    for (const std::uintptr_t va : targets)
    {
        void* const target = trial::ResolveVa(va);
        if (target != nullptr)
        {
            MH_DisableHook(target);
            MH_RemoveHook(target);
        }
    }
}

void PollFramestepHotkeys()
{
    if (!g_initialized.load() || caster::IsNetplayActive() || caster::IsSpectatorActive())
    {
        return;
    }

    const auto pressed = [](const trialnet::shortcuts::Action action) {
        return !controls::ShortcutConflictsWithAnyFightBinding(action)
            && input::WasPressed(controls::ShortcutKey(action));
    };

    if (pressed(trialnet::shortcuts::Action::Pause))
    {
        g_paused.store(!g_paused.load());
        g_stepRequested = false;
        log::Info("frame", "framestep %s", g_paused.load() ? "PAUSED" : "RUNNING");
    }

    if (pressed(trialnet::shortcuts::Action::StepFrame))
    {
        if (!g_paused.load())
        {
            g_paused.store(true);
        }
        g_stepRequested.store(true);
    }
}

bool IsFramestepPaused()
{
    return g_paused.load();
}

bool ConsumeFramestepRequest()
{
    return g_stepRequested.exchange(false);
}

void RequestFramestepPause()
{
    g_paused.store(true);
    g_stepRequested.store(false);
}

void RequestFramestepAdvance()
{
    g_paused.store(true);
    g_stepRequested.store(true);
}

HWND GetLastGameWindow()
{
    return g_lastGameWindow.load();
}

void SetGameWindow(const HWND hwnd)
{
    if (hwnd != nullptr)
    {
        g_lastGameWindow.store(hwnd);
    }
}

BOOL CALLBACK FindProcessWindowProc(const HWND hwnd, const LPARAM lParam)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId())
    {
        return TRUE;
    }

    if (!IsWindowVisible(hwnd) || GetParent(hwnd) != nullptr)
    {
        return TRUE;
    }

    auto* const best = reinterpret_cast<HWND*>(lParam);
    if (*best == nullptr)
    {
        *best = hwnd;
    }
    return TRUE;
}

HWND ResolveGameWindow()
{
    const HWND cached = g_lastGameWindow.load();
    if (cached != nullptr && IsWindow(cached))
    {
        return cached;
    }

    HWND found = nullptr;
    EnumWindows(&FindProcessWindowProc, reinterpret_cast<LPARAM>(&found));
    if (found != nullptr)
    {
        SetGameWindow(found);
        log::Debug("window", "resolved EFZ hwnd=0x%p via EnumWindows", found);
    }
    return found;
}

}
