#include "window_telemetry.h"

#include "caster_session.h"
#include "debug_log.h"
#include "game_pacing.h"
#include "lifecycle_log.h"
#include "netplay_bootstrap.h"
#include "netplay_lockstep.h"
#include "netplay_sync.h"
#include "render_gate.h"
#include "rollback.h"
#include "spectator_player.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>

namespace thv::window_telemetry
{
namespace
{

constexpr ULONGLONG kRefreshMs = 250u;
constexpr DWORD kGameThreadQuiesceTimeoutMs = 2000u;
constexpr DWORD kNativeCloseGraceMs = 500u;
constexpr wchar_t kFallbackTitle[] = L"ETERNAL FIGHTER ZERO";

enum class OnlineExitState : std::uint8_t
{
    Idle,
    Requested,
    GameThreadQuiesced,
    Draining,
    Complete,
};

HWND g_window = nullptr;
std::array<wchar_t, 256> g_originalTitle{};
bool g_originalCaptured = false;
ULONGLONG g_lastSampleMs = 0u;
std::uint64_t g_lastPresents = 0u;
std::uint32_t g_lastSimFrame = 0u;
double g_fps = 0.0;
double g_tps = 0.0;
std::atomic<WNDPROC> g_originalWindowProc{nullptr};
std::atomic<OnlineExitState> g_exitState{OnlineExitState::Idle};
std::atomic<DWORD> g_exitRequestedAtMs{0u};

void CompleteOnlineExit()
{
    g_exitState.store(OnlineExitState::Complete, std::memory_order_release);
    SetLastError(ERROR_SUCCESS);
    const bool posted = g_window != nullptr && IsWindow(g_window)
        && PostMessageW(g_window, WM_CLOSE, 0u, 0) != FALSE;
    const DWORD postError = GetLastError();
    if (posted)
    {
        lifecycle::Phase(
            "window-exit", "draining", "complete", "native WM_CLOSE reposted after handoff");
    }
    else
    {
        lifecycle::Fault(
            "window-exit",
            "native WM_CLOSE repost failed window=%p error=%lu; process exit fallback",
            static_cast<void*>(g_window),
            static_cast<unsigned long>(postError));
    }

    // Not the completion signal: WndProc may post WM_QUIT.
    const DWORD nativeDeadline = GetTickCount() + kNativeCloseGraceMs;
    while (posted && g_window != nullptr && IsWindow(g_window)
        && static_cast<std::int32_t>(nativeDeadline - GetTickCount()) > 0)
    {
        Sleep(10u);
    }
    log::Info(
        "netplay",
        "terminal handoff complete; native close grace finished posted=%d window_alive=%d; exiting process",
        posted ? 1 : 0,
        g_window != nullptr && IsWindow(g_window) ? 1 : 0);
    log::FlushFile();
    ExitProcess(0u);
}

bool RequestOnlineExit()
{
    if (!spectator::player::IsActive()
        && (!netplay::IsSessionActive() || netplay::IsNetplayAborted()))
    {
        return false;
    }
    OnlineExitState expected = OnlineExitState::Idle;
    if (g_exitState.compare_exchange_strong(
            expected,
            OnlineExitState::Requested,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        g_exitRequestedAtMs.store(GetTickCount(), std::memory_order_release);
        lifecycle::Phase(
            "window-exit",
            "idle",
            "requested",
            "WM_CLOSE deferred until a stable game-thread boundary");
        log::Info(
            "netplay",
            "native window exit requested; waiting for game-thread quiescence before spectator drain");
    }
    return true;
}

void ServiceOnlineExitRequest()
{
    OnlineExitState state = g_exitState.load(std::memory_order_acquire);
    if (state == OnlineExitState::Idle || state == OnlineExitState::Complete
        || state == OnlineExitState::Draining)
    {
        return;
    }

    if (state == OnlineExitState::Requested)
    {
        const DWORD requestedAt = g_exitRequestedAtMs.load(std::memory_order_acquire);
        if (requestedAt == 0u
            || static_cast<std::int32_t>(GetTickCount() - requestedAt)
                < static_cast<std::int32_t>(kGameThreadQuiesceTimeoutMs))
        {
            return;
        }

        // Send only the mutex-serialized player terminal; never a long archive drain here.
        OnlineExitState expected = OnlineExitState::Requested;
        if (!g_exitState.compare_exchange_strong(
                expected,
                OnlineExitState::Draining,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            return;
        }
        lifecycle::Fault(
            "window-exit",
            "game-thread quiescence timed out after %lu ms; spectator tail drain skipped",
            static_cast<unsigned long>(kGameThreadQuiesceTimeoutMs));
        log::Warn(
            "netplay",
            "native window exit could not quiesce game thread; sending player quit without racing spectator archive state");
        if (netplay::IsSessionActive() && !netplay::IsNetplayAborted())
        {
            netplay::SendQuit();
        }
        log::FlushFile();
        CompleteOnlineExit();
        return;
    }

    OnlineExitState expected = OnlineExitState::GameThreadQuiesced;
    if (!g_exitState.compare_exchange_strong(
            expected,
            OnlineExitState::Draining,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        return;
    }
    lifecycle::Phase(
        "window-exit",
        "game-thread-quiesced",
        "draining",
        "mod thread owns player/spectator terminal handoff");
    if (spectator::player::IsActive())
    {
        spectator::player::Shutdown();
    }
    if (netplay::IsSessionActive() && !netplay::IsNetplayAborted())
    {
        log::Info("netplay", "native window exit: sending player/spectator terminal handoff");
        netplay::SendQuit();
        netplay::NotifySpectatorsSourceEnded(false);
        log::FlushFile();
    }
    CompleteOnlineExit();
}

LRESULT CALLBACK LifecycleWindowProc(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam)
{
    if (message == WM_CLOSE
        && g_exitState.load(std::memory_order_acquire) != OnlineExitState::Complete)
    {
        // No spectator drain on the WndProc thread: defer close until the worker reposts.
        if (RequestOnlineExit())
        {
            return 0;
        }
    }
    else if (message == WM_ENDSESSION && wParam != 0u)
    {
        // System shutdown cannot wait for a long archive drain.
        if (netplay::IsSessionActive() && !netplay::IsNetplayAborted())
        {
            netplay::SendQuit();
        }
    }
    const WNDPROC original = g_originalWindowProc.load(std::memory_order_acquire);
    return original != nullptr
        ? CallWindowProcW(original, window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);
}

void RestoreWindowProc()
{
    const WNDPROC original = g_originalWindowProc.load(std::memory_order_acquire);
    if (g_window != nullptr && IsWindow(g_window) && original != nullptr
        && reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_window, GWLP_WNDPROC))
            == &LifecycleWindowProc)
    {
        SetWindowLongPtrW(
            g_window,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(original));
    }
    // Keep the target published: an in-flight WndProc call can outlive restoration.
}

void CaptureOriginalTitle(const HWND window)
{
    if (window == g_window && g_originalCaptured)
    {
        return;
    }

    RestoreWindowProc();
    g_window = window;
    g_originalCaptured = false;
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR observed = GetWindowLongPtrW(window, GWLP_WNDPROC);
    const DWORD observeError = GetLastError();
    if (observed == 0 && observeError != ERROR_SUCCESS)
    {
        log::Warn(
            "netplay",
            "could not read native window procedure before lifecycle subclass error=%lu",
            static_cast<unsigned long>(observeError));
        return;
    }
    const WNDPROC preRead = reinterpret_cast<WNDPROC>(observed);
    if (preRead == nullptr || preRead == &LifecycleWindowProc)
    {
        log::Warn("netplay", "native window procedure was null/already subclassed; lifecycle hook skipped");
        return;
    }
    // Publish a valid target before exposing the callback.
    g_originalWindowProc.store(preRead, std::memory_order_release);
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(
        window,
        GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&LifecycleWindowProc));
    if (previous != 0 || GetLastError() == ERROR_SUCCESS)
    {
        const WNDPROC returned = reinterpret_cast<WNDPROC>(previous);
        if (returned != nullptr && returned != &LifecycleWindowProc)
        {
            g_originalWindowProc.store(returned, std::memory_order_release);
        }
        else
        {
            // Never create LifecycleWindowProc recursion.
            (void)SetWindowLongPtrW(
                window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(preRead));
            log::Warn("netplay", "native window subclass returned an invalid upstream; lifecycle hook rolled back");
            return;
        }
    }
    else
    {
        // Retaining preRead keeps an in-flight callback's forwarding target valid.
        log::Warn("netplay", "could not install native window-exit lifecycle hook");
        return;
    }
    g_originalTitle.fill(L'\0');
    const int copied = GetWindowTextW(window, g_originalTitle.data(),
        static_cast<int>(g_originalTitle.size()));
    if (copied <= 0)
    {
        std::wcsncpy(g_originalTitle.data(), kFallbackTitle, g_originalTitle.size() - 1u);
    }
    g_originalCaptured = true;
    g_lastSampleMs = 0u;
    g_lastPresents = GetPresentedFrameCount();
    g_lastSimFrame = 0u;
    g_fps = 0.0;
    g_tps = 0.0;
    // Title recapture must not cancel a requested terminal handoff.
    if (g_exitState.load(std::memory_order_acquire) == OnlineExitState::Idle)
    {
        g_exitRequestedAtMs.store(0u, std::memory_order_release);
    }
}

std::uint32_t DisplayFrame(const rollback::Status& rb, const netplay::lockstep::Stats& ls)
{
    return rb.active ? rb.currentFrame : ls.simFrame;
}

}

void Poll(const HWND gameWindow)
{
    ServiceOnlineExitRequest();
    if (IsOnlineExitPending())
    {
        return;
    }
    if (gameWindow == nullptr || !IsWindow(gameWindow))
    {
        return;
    }

    CaptureOriginalTitle(gameWindow);

    const session::LauncherSession* const launched = session::GetCachedSession();
    const bool active = netplay::IsSessionActive();
    if (launched == nullptr && !active)
    {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (g_lastSampleMs != 0u && now - g_lastSampleMs < kRefreshMs)
    {
        return;
    }

    const rollback::Status rb = rollback::GetStatus();
    const netplay::lockstep::Stats ls = netplay::lockstep::GetStats();
    const std::uint32_t simFrame = DisplayFrame(rb, ls);
    const std::uint64_t presents = GetPresentedFrameCount();

    if (g_lastSampleMs != 0u && now > g_lastSampleMs)
    {
        const double seconds = static_cast<double>(now - g_lastSampleMs) / 1000.0;
        const std::uint64_t presentDelta = presents >= g_lastPresents ? presents - g_lastPresents : 0u;
        const std::uint32_t simDelta = simFrame >= g_lastSimFrame ? simFrame - g_lastSimFrame : 0u;
        const double instantFps = static_cast<double>(presentDelta) / seconds;
        const double instantTps = static_cast<double>(simDelta) / seconds;
        g_fps = g_fps == 0.0 ? instantFps : g_fps * 0.65 + instantFps * 0.35;
        g_tps = g_tps == 0.0 ? instantTps : g_tps * 0.65 + instantTps * 0.35;
    }

    g_lastSampleMs = now;
    g_lastPresents = presents;
    g_lastSimFrame = simFrame;

    const std::uint32_t ping = launched != nullptr ? launched->pingMs : netplay::lockstep::GetPingMs();
    int delay = netplay::GetNegotiatedInputDelay();
    int rollbackFrames = netplay::GetNegotiatedRollbackFrames();
    if (delay < 0)
    {
        delay = launched != nullptr ? launched->inputDelay : rb.inputDelay;
    }
    if (rollbackFrames < 0)
    {
        rollbackFrames = launched != nullptr ? launched->rollbackFrames : 0;
    }
    if (rb.active)
    {
        delay = rb.inputDelay;
    }

    wchar_t title[256] = {};
    std::swprintf(title, std::size(title),
        L"ETERNAL FIGHTER ZERO | %u FPS mode | %u ms | F %u/%u | %.1f render %.1f game | D%d R%d | RB %u",
        static_cast<unsigned>(pacing::GetSessionTargetFps()),
        ping,
        simFrame,
        rb.active ? rb.confirmedFrame : ls.peerAckFrame,
        std::clamp(g_fps, 0.0, 999.9),
        std::clamp(g_tps, 0.0, 999.9),
        std::max(0, delay),
        std::max(0, rollbackFrames),
        rb.rollbacksTotal);
    SetWindowTextW(gameWindow, title);
}

bool QuiesceGameThreadForOnlineExit()
{
    OnlineExitState state = g_exitState.load(std::memory_order_acquire);
    if (state == OnlineExitState::Requested)
    {
        OnlineExitState expected = OnlineExitState::Requested;
        if (g_exitState.compare_exchange_strong(
                expected,
                OnlineExitState::GameThreadQuiesced,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            lifecycle::Phase(
                "window-exit",
                "requested",
                "game-thread-quiesced",
                "simulation, input polling, and confirmed recording frozen");
            log::Info(
                "netplay",
                "game thread quiesced at top-level screen boundary; spectator drain may begin");
            state = OnlineExitState::GameThreadQuiesced;
        }
        else
        {
            state = expected;
        }
    }
    return state == OnlineExitState::GameThreadQuiesced
        || state == OnlineExitState::Draining
        || state == OnlineExitState::Complete;
}

bool IsOnlineExitPending()
{
    return g_exitState.load(std::memory_order_acquire) != OnlineExitState::Idle;
}

void Shutdown()
{
    if (g_window != nullptr && IsWindow(g_window) && g_originalCaptured)
    {
        SetWindowTextW(g_window, g_originalTitle.data());
    }
    RestoreWindowProc();
    g_window = nullptr;
    g_originalTitle.fill(L'\0');
    g_originalCaptured = false;
    g_lastSampleMs = 0u;
    g_exitRequestedAtMs.store(0u, std::memory_order_release);
    g_exitState.store(OnlineExitState::Idle, std::memory_order_release);
}

}
