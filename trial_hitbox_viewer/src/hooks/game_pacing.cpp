#include "game_pacing.h"

#include "debug_log.h"
#include "trial_addresses.h"
#include "win32_compat.h"

#include <mmsystem.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>

namespace thv::pacing
{
namespace
{

using SetWrapperFrameLimitFn = BOOL(__stdcall*)(UINT fps);

std::atomic<std::uint8_t> g_sessionTargetFps{0u};
std::atomic<bool> g_modOwnsPacing{false};
std::atomic<std::uint32_t> g_pacingGeneration{1u};

struct ThreadDeadline
{
    std::uint32_t generation = 0u;
    std::uint8_t targetFps = 0u;
    LONGLONG frequency = 0;
    LONGLONG deadline = 0;
    LONGLONG fractional = 0;
    LONGLONG cadenceStart = 0;
    LONGLONG nativeFpsSampleStart = 0;
    std::uint32_t cadenceFrames = 0u;
    std::uint32_t lateFrames = 0u;
    std::uint32_t deadlineResets = 0u;
    LONGLONG maxLateTicks = 0;
    bool initialized = false;
    // A later Consume* stall in the same outer handler must not sleep again.
    bool outerSlotPrePaced = false;
};

thread_local ThreadDeadline g_threadDeadline{};

SetWrapperFrameLimitFn ResolveWrapperFrameLimit()
{
    const HMODULE wrapper = GetModuleHandleW(L"ddraw.dll");
    return wrapper != nullptr
        ? reinterpret_cast<SetWrapperFrameLimitFn>(
              GetProcAddress(wrapper, "EFZDDRAW_SetFrameLimit"))
        : nullptr;
}

void SetNativeBattleBypass(const bool enabled)
{
    auto* const busy = static_cast<int*>(trial::ResolveVa(trial::kVaFramePacingBusyFlag));
    if (busy != nullptr)
    {
        *busy = enabled ? 1 : 0;
    }
}

void BypassNativeLimiterForOwnedFrame()
{
    if (!g_modOwnsPacing.load(std::memory_order_acquire))
    {
        return;
    }

    SetNativeBattleBypass(true);
    auto* const lastTick = static_cast<DWORD*>(trial::ResolveVa(trial::kVaFramePacingLastTick));
    if (lastTick != nullptr)
    {
        *lastTick = timeGetTime() - 32u;
    }
}

void ServiceOwnedNativeFpsHud(
    ThreadDeadline* const state,
    const LONGLONG nowQpc)
{
    if (state == nullptr || state->frequency <= 0 || nowQpc <= 0)
    {
        return;
    }

    auto* const displayed = static_cast<volatile std::int32_t*>(
        trial::ResolveVa(trial::kVaNativeFpsLatch));
    auto* const pending = static_cast<volatile std::int32_t*>(
        trial::ResolveVa(trial::kVaNativeFpsCounter));
    auto* const nativeAnchor = static_cast<volatile double*>(
        trial::ResolveVa(trial::kVaNativeFpsSampleAnchor));
    if (displayed == nullptr || pending == nullptr || nativeAnchor == nullptr)
    {
        return;
    }

    if (state->nativeFpsSampleStart == 0)
    {
        state->nativeFpsSampleStart = nowQpc;
        *pending = 0;
    }
    else if (nowQpc - state->nativeFpsSampleStart >= state->frequency)
    {
        // Own only the one-second latch while mod pacing is active.
        const LONGLONG elapsedTicks = nowQpc - state->nativeFpsSampleStart;
        const std::int32_t measuredSubsteps = *pending;
        const LONGLONG nonNegativeSubsteps = (std::max)(0, measuredSubsteps);
        const LONGLONG normalizedSubsteps = elapsedTicks > 0
            ? (nonNegativeSubsteps * state->frequency + elapsedTicks / 2) / elapsedTicks
            : nonNegativeSubsteps;
        const std::int32_t displayedFrames = static_cast<std::int32_t>(
            (normalizedSubsteps + 1) / 3);
        *displayed = displayedFrames * 3;
        *pending = 0;
        state->nativeFpsSampleStart = nowQpc;
        log::Debug(
            "pacing",
            "native FPS latch rawSubsteps=%d normalizedSubsteps=%lld elapsedTicks=%lld displayed=%d",
            measuredSubsteps,
            static_cast<long long>(normalizedSubsteps),
            static_cast<long long>(elapsedTicks),
            displayedFrames);
    }

    *nativeAnchor = static_cast<double>(GetTickCount());
}

void AdvanceDeadline(ThreadDeadline* const state)
{
    if (state == nullptr || state->targetFps == 0u || state->frequency <= 0)
    {
        return;
    }

    const LONGLONG fps = static_cast<LONGLONG>(state->targetFps);
    state->deadline += state->frequency / fps;
    state->fractional += state->frequency % fps;
    if (state->fractional >= fps)
    {
        state->deadline += state->fractional / fps;
        state->fractional %= fps;
    }
}

void WaitForQpcDeadline(const LONGLONG deadline, const LONGLONG frequency)
{
    static const bool s_wineRuntime = win32::IsWineRuntime();
    if (frequency <= 0)
    {
        return;
    }

    for (;;)
    {
        LARGE_INTEGER now{};
        if (!QueryPerformanceCounter(&now) || now.QuadPart >= deadline)
        {
            return;
        }

        const LONGLONG remaining = deadline - now.QuadPart;
        const LONGLONG remainingMs = (remaining * 1000LL) / frequency;
        if (remainingMs > 2LL)
        {
            Sleep(static_cast<DWORD>(remainingMs - 1LL));
        }
        else if (remaining * 2000LL > frequency)
        {
            if (!SwitchToThread() && s_wineRuntime)
            {
                Sleep(0u);
            }
        }
        else
        {
            YieldProcessor();
        }
    }
}

void WaitForOwnedFrameSlot()
{
    const std::uint8_t target = g_sessionTargetFps.load(std::memory_order_acquire);
    const std::uint32_t generation = g_pacingGeneration.load(std::memory_order_acquire);
    if (!g_modOwnsPacing.load(std::memory_order_acquire)
        || (target != 60u && target != 64u))
    {
        return;
    }

    LARGE_INTEGER frequency{};
    LARGE_INTEGER now{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0
        || !QueryPerformanceCounter(&now))
    {
        return;
    }

    ThreadDeadline& state = g_threadDeadline;
    if (!state.initialized || state.generation != generation
        || state.targetFps != target || state.frequency != frequency.QuadPart)
    {
        state = {};
        state.initialized = true;
        state.generation = generation;
        state.targetFps = target;
        state.frequency = frequency.QuadPart;
        state.deadline = now.QuadPart;
        state.cadenceStart = now.QuadPart;
        state.nativeFpsSampleStart = 0;
        ServiceOwnedNativeFpsHud(&state, now.QuadPart);
        state.cadenceFrames = 0u;
        AdvanceDeadline(&state);
        return;
    }

    const LONGLONG nominalTicks = state.frequency / static_cast<LONGLONG>(target);
    if (now.QuadPart > state.deadline + nominalTicks)
    {
        // Never burst visible sim frames to repay lost time.
        state.deadline = now.QuadPart;
        state.fractional = 0;
        ++state.deadlineResets;
        AdvanceDeadline(&state);
    }
    else
    {
        const LONGLONG awaitedDeadline = state.deadline;
        WaitForQpcDeadline(awaitedDeadline, state.frequency);
        LARGE_INTEGER afterWait{};
        if (QueryPerformanceCounter(&afterWait) && afterWait.QuadPart > awaitedDeadline)
        {
            const LONGLONG late = afterWait.QuadPart - awaitedDeadline;
            state.maxLateTicks = (std::max)(state.maxLateTicks, late);
            if (late * 1000LL >= state.frequency)
            {
                ++state.lateFrames;
            }
        }
        AdvanceDeadline(&state);
    }

    LARGE_INTEGER fpsSampleNow{};
    if (QueryPerformanceCounter(&fpsSampleNow))
    {
        ServiceOwnedNativeFpsHud(&state, fpsSampleNow.QuadPart);
    }

    ++state.cadenceFrames;
    if (state.cadenceFrames >= static_cast<std::uint32_t>(target) * 10u)
    {
        LARGE_INTEGER sampled{};
        if (QueryPerformanceCounter(&sampled) && sampled.QuadPart > state.cadenceStart)
        {
            const double seconds = static_cast<double>(sampled.QuadPart - state.cadenceStart)
                / static_cast<double>(state.frequency);
            const double actual = seconds > 0.0
                ? static_cast<double>(state.cadenceFrames) / seconds
                : 0.0;
            const double maxLateUs = static_cast<double>(state.maxLateTicks) * 1000000.0
                / static_cast<double>(state.frequency);
            const auto* const nativeFpsLatch = static_cast<const std::int32_t*>(
                trial::ResolveVa(trial::kVaNativeFpsLatch));
            const auto* const nativeFpsCounter = static_cast<const std::int32_t*>(
                trial::ResolveVa(trial::kVaNativeFpsCounter));
            const auto* const nativePacingBypass = static_cast<const std::int32_t*>(
                trial::ResolveVa(trial::kVaFramePacingBusyFlag));
            const int nativeFpsRaw = nativeFpsLatch != nullptr ? *nativeFpsLatch : -1;
            log::Info(
                "pacing",
                "pacing cadence target=%u measured=%.3f frames=%u seconds=%.3f nativeHud=%.1f nativeRaw=%d nativePartial=%d bypass=%d lateFrames=%u maxLateUs=%.1f deadlineResets=%u owner=mod",
                static_cast<unsigned>(target),
                actual,
                state.cadenceFrames,
                seconds,
                nativeFpsRaw >= 0 ? static_cast<double>(nativeFpsRaw) / 3.0 : -1.0,
                nativeFpsRaw,
                nativeFpsCounter != nullptr ? *nativeFpsCounter : -1,
                nativePacingBypass != nullptr ? *nativePacingBypass : -1,
                state.lateFrames,
                maxLateUs,
                state.deadlineResets);
        }
        state.cadenceFrames = 0u;
        state.lateFrames = 0u;
        state.deadlineResets = 0u;
        state.maxLateTicks = 0;
        state.cadenceStart = sampled.QuadPart > 0 ? sampled.QuadPart : now.QuadPart;
    }
}

void CooperativeFrameWait(
    const DWORD anchor,
    const unsigned char threshold,
    const volatile int* const busyFlag)
{
    static const bool s_wineRuntime = win32::IsWineRuntime();
    for (;;)
    {
        if (busyFlag != nullptr && *busyFlag != 0)
        {
            return;
        }
        const DWORD elapsed = timeGetTime() - anchor;
        if (elapsed >= threshold)
        {
            return;
        }
        const DWORD remaining = static_cast<DWORD>(threshold) - elapsed;
        if (s_wineRuntime && remaining > 2u)
        {
            Sleep(1u);
        }
        else if (s_wineRuntime)
        {
            // SwitchToThread returns zero when idle; looping straight back spins a core on Wine.
            if (!SwitchToThread())
            {
                Sleep(1u);
            }
        }
        else
        {
            YieldProcessor();
        }
    }
}

void ConsumeFrameSlot(const std::uintptr_t toggleVa, const bool battleBusyLoop)
{
    if (g_modOwnsPacing.load(std::memory_order_acquire))
    {
        if (g_threadDeadline.outerSlotPrePaced)
        {
            g_threadDeadline.outerSlotPrePaced = false;
        }
        else
        {
            WaitForOwnedFrameSlot();
        }
        BypassNativeLimiterForOwnedFrame();
        return;
    }

    auto* const lastTick = static_cast<DWORD*>(trial::ResolveVa(trial::kVaFramePacingLastTick));
    auto* const toggle = static_cast<const int*>(trial::ResolveVa(toggleVa));
    if (lastTick == nullptr || toggle == nullptr)
    {
        return;
    }

    // Anchor a fresh slot so repeated stall ticks do not tight-spin the CPU.
    *lastTick = timeGetTime();

    const auto threshold = static_cast<unsigned char>(16 - *toggle);
    if (battleBusyLoop)
    {
        auto* const busy = static_cast<int*>(trial::ResolveVa(trial::kVaFramePacingBusyFlag));
        if (busy != nullptr)
        {
            CooperativeFrameWait(*lastTick, threshold, busy);
            return;
        }
    }

    CooperativeFrameWait(*lastTick, threshold, nullptr);
}

std::uintptr_t ToggleForActiveScreen()
{
    const auto* const mode = static_cast<const std::uint8_t*>(trial::ResolveVa(trial::kVaBattleScreenMode));
    if (mode == nullptr)
    {
        return trial::kVaFramePacingToggle;
    }

    switch (*mode)
    {
    case trial::kScreenMainMenu:
        return trial::kVaFramePacingToggleMenu;
    case trial::kScreenBattle:
        return trial::kVaFramePacingToggle;
    case trial::kScreenCharSelect:
        return trial::kVaFramePacingToggleCharSelect;
    case trial::kScreenLoading:
        return trial::kVaFramePacingToggleLoading;
    case trial::kScreenTitle:
        return trial::kVaFramePacingToggleTitle;
    default:
        return trial::kVaFramePacingToggle;
    }
}

}

bool ConfigureSessionTargetFps(const std::uint8_t targetFps)
{
    if (targetFps != 60u && targetFps != 64u)
    {
        log::Error(
            "pacing",
            "pacing configuration rejected target=%u (supported: 60 or 64)",
            static_cast<unsigned>(targetFps));
        return false;
    }

    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
    {
        log::Error(
            "pacing",
            "pacing configuration rejected target=%u QueryPerformanceFrequency unavailable",
            static_cast<unsigned>(targetFps));
        return false;
    }

    const SetWrapperFrameLimitFn setWrapperFrameLimit = ResolveWrapperFrameLimit();
    const bool wrapperDisabled = setWrapperFrameLimit != nullptr
        && setWrapperFrameLimit(0u) != FALSE;
    if (!wrapperDisabled)
    {
        // Fail closed without the limiter handoff: stacked waits make 64 Hz impossible.
        g_sessionTargetFps.store(0u, std::memory_order_release);
        g_modOwnsPacing.store(false, std::memory_order_release);
        SetNativeBattleBypass(false);
        g_pacingGeneration.fetch_add(1u, std::memory_order_acq_rel);
        log::Error(
            "pacing",
            "pacing configuration failed target=%u wrapperThrottle=uncontrollable export=%s",
            static_cast<unsigned>(targetFps),
            setWrapperFrameLimit != nullptr ? "present" : "missing");
        return false;
    }

    g_sessionTargetFps.store(targetFps, std::memory_order_release);
    g_modOwnsPacing.store(wrapperDisabled, std::memory_order_release);
    g_pacingGeneration.fetch_add(1u, std::memory_order_acq_rel);
    SetNativeBattleBypass(wrapperDisabled);

    log::Info(
        "pacing",
        "pacing configured target=%u owner=mod wrapperThrottle=disabled runtime=%s qpc=%lld",
        static_cast<unsigned>(targetFps),
        win32::IsWineRuntime() ? "wine" : "windows",
        static_cast<long long>(frequency.QuadPart));
    return true;
}

void ResetSessionTargetFps()
{
    const SetWrapperFrameLimitFn setWrapperFrameLimit = ResolveWrapperFrameLimit();
    if (g_modOwnsPacing.load(std::memory_order_acquire) && setWrapperFrameLimit != nullptr)
    {
        (void)setWrapperFrameLimit(60u);
    }
    g_modOwnsPacing.store(false, std::memory_order_release);
    g_sessionTargetFps.store(0u, std::memory_order_release);
    SetNativeBattleBypass(false);
    g_pacingGeneration.fetch_add(1u, std::memory_order_acq_rel);
    g_threadDeadline = {};
    log::Info("pacing", "pacing reset owner=native wrapperThrottle=60");
}

std::uint8_t GetSessionTargetFps()
{
    return g_sessionTargetFps.load(std::memory_order_acquire);
}

NativeFpsAccountingState CaptureNativeFpsAccounting()
{
    const auto* const displayed = static_cast<const volatile std::int32_t*>(
        trial::ResolveVa(trial::kVaNativeFpsLatch));
    const auto* const pending = static_cast<const volatile std::int32_t*>(
        trial::ResolveVa(trial::kVaNativeFpsCounter));
    const auto* const anchor = static_cast<const volatile double*>(
        trial::ResolveVa(trial::kVaNativeFpsSampleAnchor));

    NativeFpsAccountingState state{};
    if (displayed == nullptr || pending == nullptr || anchor == nullptr)
    {
        return state;
    }

    state.displayedSubsteps = *displayed;
    state.pendingSubsteps = *pending;
    state.sampleAnchorMs = *anchor;
    state.valid = true;
    return state;
}

void RestoreNativeFpsAccounting(const NativeFpsAccountingState& state)
{
    if (!state.valid)
    {
        return;
    }

    auto* const displayed = static_cast<volatile std::int32_t*>(
        trial::ResolveVa(trial::kVaNativeFpsLatch));
    auto* const pending = static_cast<volatile std::int32_t*>(
        trial::ResolveVa(trial::kVaNativeFpsCounter));
    auto* const anchor = static_cast<volatile double*>(
        trial::ResolveVa(trial::kVaNativeFpsSampleAnchor));
    if (displayed == nullptr || pending == nullptr || anchor == nullptr)
    {
        return;
    }

    // Restore the epoch before its counts; volatile keeps these writes from coalescing.
    *anchor = state.sampleAnchorMs;
    *displayed = state.displayedSubsteps;
    *pending = state.pendingSubsteps;
}

ScopedNativeFpsAccountingIsolation::ScopedNativeFpsAccountingIsolation(const bool hidden)
{
    if (hidden)
    {
        state_ = CaptureNativeFpsAccounting();
    }
}

ScopedNativeFpsAccountingIsolation::~ScopedNativeFpsAccountingIsolation()
{
    RestoreNativeFpsAccounting(state_);
}

void PaceVisibleFrame()
{
    g_threadDeadline.outerSlotPrePaced = false;
    if (!g_modOwnsPacing.load(std::memory_order_acquire))
    {
        return;
    }
    WaitForOwnedFrameSlot();
    g_threadDeadline.outerSlotPrePaced = true;
    BypassNativeLimiterForOwnedFrame();
}

void ConsumeActiveScreenFrameSlot()
{
    const std::uintptr_t toggleVa = ToggleForActiveScreen();
    ConsumeFrameSlot(toggleVa, toggleVa == trial::kVaFramePacingToggle);
}

void ConsumeBattleFrameSlot()
{
    ConsumeFrameSlot(trial::kVaFramePacingToggle, true);
}

void BypassNextNativeFrameWait()
{
    auto* const lastTick = static_cast<DWORD*>(trial::ResolveVa(trial::kVaFramePacingLastTick));
    if (lastTick != nullptr)
    {
        *lastTick = timeGetTime() - 32u;
    }
}

std::uint32_t CaptureNativeFrameWaitAnchor()
{
    const auto* const lastTick = static_cast<const DWORD*>(
        trial::ResolveVa(trial::kVaFramePacingLastTick));
    return lastTick != nullptr ? static_cast<std::uint32_t>(*lastTick)
                               : static_cast<std::uint32_t>(timeGetTime());
}

void RestoreNextNativeFrameWaitAnchor(const std::uint32_t anchorMs)
{
    if (g_modOwnsPacing.load(std::memory_order_acquire))
    {
        // Hidden replay may have overwritten the anchor; restoring would wait twice.
        BypassNativeLimiterForOwnedFrame();
        return;
    }
    auto* const lastTick = static_cast<DWORD*>(trial::ResolveVa(trial::kVaFramePacingLastTick));
    if (lastTick != nullptr)
    {
        *lastTick = static_cast<DWORD>(anchorMs);
    }
}

void WaitUntilMs(const std::uint32_t deadlineMs, void (*pollFn)())
{
    if (deadlineMs == 0u)
    {
        return;
    }

    while (!win32::TickDeadlineReached(
        static_cast<std::uint32_t>(GetTickCount()), deadlineMs))
    {
        if (pollFn != nullptr)
        {
            pollFn();
        }
        ConsumeActiveScreenFrameSlot();
    }
}

}
