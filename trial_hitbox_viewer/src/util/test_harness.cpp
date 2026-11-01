#include "test_harness.h"

#include "caster_input.h"
#include "caster_session.h"
#include "debug_log.h"
#include "desync_check.h"
#include "lifecycle_log.h"
#include "menu_nav.h"
#include "netplay_sync.h"
#include "rng.h"
#include "rollback.h"
#include "spectator_relay.h"
#include "trial_addresses.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace thv::harness
{
namespace
{

struct Config
{
    bool inputScript = false;
    std::uint32_t inputSeed = 0x7E57C0DEu;
    std::uint32_t netDelayMs = 0;
    std::uint32_t netJitterMs = 0;
    std::uint32_t netLossPct = 0;
    std::uint32_t netShimSeed = 0x51A7E5EEu;
    std::uint32_t maxBattleFrames = 0;
    std::uint32_t forceKoAfterFrames = 0;
    std::uint32_t requiredMatches = 1;
    std::uint32_t gameplayEscAfterFrames = 0;
    bool earlySystemKeyProbe = false;
};

std::once_flag g_loadOnce;
Config g_config{};
std::atomic<bool> g_verdictEmitted{false};
std::atomic<DWORD> g_verdictAbortAtMs{0};

struct LifecycleState
{
    std::uint8_t lastMode = 0xFFu;
    std::uint8_t lastWinCount = 0u;
    bool haveWinCount = false;
    bool actualWinLatched = false;
    bool awaitingCharSelect = false;
    bool escapeIssuedForCurrentWin = false;
    DWORD actualWinDetectedAtMs = 0u;
    bool titleEscapeIssuedForCurrentWin = false;
    bool functionalF4Issued = false;
    bool functionalF5Issued = false;
    bool functionalF6Issued = false;
    bool functionalF7Issued = false;
    bool functionalF8Issued = false;
    bool frameStepIncIssued = false;
    bool frameStepDecIssued = false;
    bool liveDelayIncreaseApplied = false;
    bool liveDelayDecreaseApplied = false;
    bool functionalEpochSet = false;
    std::uint32_t functionalEpochFrame = 0u;
    bool earlyFunctionalEpochSet = false;
    std::uint32_t earlyFunctionalEpochFrame = 0u;
    bool earlyFunctionalF4Issued = false;
    bool earlyFunctionalF5Issued = false;
    bool gameplayEscIssued = false;
    bool gameplayEscReachedCharSelect = false;
    bool titleEpochSet = false;
    std::uint32_t titleEpochFrame = 0u;
    bool charSelectEpochSet = false;
    std::uint32_t charSelectEpochBase = 0u;
    std::uint32_t roundsCompleted = 0u;
    std::uint32_t completedMatches = 0u;
    std::uint32_t charSelectReturns = 0u;
    std::uint32_t matchesStarted = 0u;
    std::uint32_t totalBattleFrames = 0u;
    std::uint32_t peakRoundBattleFrames = 0u;
    std::uint32_t currentRoundBattleFrames = 0u;
    std::uint32_t lastBattleFrame = 0xFFFFFFFFu;
};

std::mutex g_lifecycleMutex;
LifecycleState g_lifecycle{};

template <typename T>
T ReadVa(const std::uintptr_t va, const T fallback = {})
{
    const auto* const value = static_cast<const T*>(trial::ResolveVa(va));
    return value != nullptr ? *value : fallback;
}

bool IsActualMatchWinScreen(const std::uint8_t mode, const int subState)
{
    if (mode != trial::kScreenLoading || (subState != 6 && subState != 8))
    {
        return false;
    }
    const std::int16_t p1Move = ReadVa<std::int16_t>(trial::kVaP1MoveId);
    const std::int16_t p2Move =
        ReadVa<std::int16_t>(trial::kVaP2Player + trial::kOffMoveId);
    return p1Move == trial::kWinScreenMoveIdB || p2Move == trial::kWinScreenMoveIdB;
}

void ObserveLifecycleUnlocked(
    const std::uint32_t currentFrame,
    const std::uint32_t matchStartFrame)
{
    LifecycleState& s = g_lifecycle;
    const std::uint8_t mode =
        ReadVa<std::uint8_t>(trial::kVaBattleScreenMode, 0xFFu);
    const int subState = ReadVa<int>(trial::kVaScreenSubState, -1);
    const std::uint8_t winCount = ReadVa<std::uint8_t>(trial::kVaWinCount, 0u);

    if (s.haveWinCount && winCount > s.lastWinCount && s.lastWinCount >= 1u)
    {
        s.roundsCompleted += static_cast<std::uint32_t>(winCount - s.lastWinCount);
        log::Critical(
            "harness",
            "HARNESS lifecycle native round win=%u totalRoundWins=%u",
            static_cast<unsigned>(winCount),
            s.roundsCompleted);
    }
    s.haveWinCount = true;
    s.lastWinCount = winCount;

    if (mode != s.lastMode)
    {
        if (mode == trial::kScreenBattle)
        {
            s.currentRoundBattleFrames = 0u;
            s.lastBattleFrame = 0xFFFFFFFFu;
            if (winCount == 1u)
            {
                s.actualWinLatched = false;
                s.escapeIssuedForCurrentWin = false;
                s.actualWinDetectedAtMs = 0u;
                ++s.matchesStarted;
                log::Critical(
                    "harness",
                    "HARNESS lifecycle match start=%u completed=%u",
                    s.matchesStarted,
                    s.completedMatches);
            }
        }
        if (mode == trial::kScreenCharSelect)
        {
            if (s.lastMode == trial::kScreenBattle && s.completedMatches == 0u)
            {
                s.gameplayEscReachedCharSelect = true;
                log::Critical(
                    "harness",
                    "HARNESS lifecycle reached CHAR_SELECT directly from gameplay");
            }
            s.actualWinLatched = false;
            s.escapeIssuedForCurrentWin = false;
            s.actualWinDetectedAtMs = 0u;
            s.titleEpochSet = false;
            s.charSelectEpochSet = false;
            if (s.awaitingCharSelect)
            {
                s.awaitingCharSelect = false;
                ++s.charSelectReturns;
                log::Critical(
                    "harness",
                    "HARNESS lifecycle returned CHAR_SELECT count=%u",
                    s.charSelectReturns);
            }
        }
        else
        {
            s.charSelectEpochSet = false;
            if (mode == trial::kScreenTitle && s.awaitingCharSelect)
            {
                s.titleEpochSet = true;
                s.titleEpochFrame = currentFrame;
            }
            else if (mode != trial::kScreenTitle)
            {
                s.titleEpochSet = false;
            }
        }
    }

    if (mode == trial::kScreenBattle && currentFrame != s.lastBattleFrame)
    {
        s.lastBattleFrame = currentFrame;
        ++s.totalBattleFrames;
        s.currentRoundBattleFrames = currentFrame >= matchStartFrame
            ? currentFrame - matchStartFrame
            : 0u;
        s.peakRoundBattleFrames =
            std::max(s.peakRoundBattleFrames, s.currentRoundBattleFrames);
        if (s.currentRoundBattleFrames != 0u
            && (s.currentRoundBattleFrames % 600u) == 0u)
        {
            log::Info(
                "harness",
                "natural-combat progress matchFrame=%u hp[P1=%d P2=%d] move[P1=%d P2=%d] x[P1=%.1f P2=%.1f]",
                s.currentRoundBattleFrames,
                ReadVa<std::int32_t>(trial::kVaP1Player + trial::kOffHp, -1),
                ReadVa<std::int32_t>(trial::kVaP2Player + trial::kOffHp, -1),
                static_cast<int>(ReadVa<std::int16_t>(trial::kVaP1MoveId, -1)),
                static_cast<int>(ReadVa<std::int16_t>(
                    trial::kVaP2Player + trial::kOffMoveId, -1)),
                ReadVa<double>(trial::kVaP1Player + trial::kOffPositionX, -1.0),
                ReadVa<double>(trial::kVaP2Player + trial::kOffPositionX, -1.0));
        }
    }

    const bool actualWin = IsActualMatchWinScreen(mode, subState);
    if (actualWin && !s.actualWinLatched)
    {
        s.actualWinLatched = true;
        s.awaitingCharSelect = true;
        s.escapeIssuedForCurrentWin = false;
        s.actualWinDetectedAtMs = GetTickCount();
        s.titleEscapeIssuedForCurrentWin = false;
        s.titleEpochSet = false;
        ++s.completedMatches;
        log::Critical(
            "harness",
            "HARNESS lifecycle actual WIN_SCREEN match=%u rounds=%u winCount=%u subState=%d",
            s.completedMatches,
            s.roundsCompleted,
            static_cast<unsigned>(winCount),
            subState);
    }

    s.lastMode = mode;
}

// Private LCG: CRT rand would drain the synced sim stream.
std::atomic<std::uint32_t> g_shimState{0x51A7E5EEu};

std::uint32_t NextShim()
{
    std::uint32_t prev = g_shimState.load(std::memory_order_relaxed);
    std::uint32_t next = 0;
    do
    {
        next = prev * 0x343FDu + 0x269EC3u;
    } while (!g_shimState.compare_exchange_weak(prev, next, std::memory_order_relaxed));
    return next;
}

void LoadConfig()
{
    const auto path = session::ResolveSessionIniPath();
    if (path.empty())
    {
        return;
    }
    const std::wstring file = path.wstring();
    const wchar_t* const section = L"test";
    g_config.inputScript = GetPrivateProfileIntW(section, L"input_script", 0, file.c_str()) != 0;
    g_config.inputSeed = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(section, L"input_seed", 0x7E57C0DE, file.c_str()));
    g_config.netDelayMs =
        static_cast<std::uint32_t>(GetPrivateProfileIntW(section, L"net_delay_ms", 0, file.c_str()));
    g_config.netJitterMs =
        static_cast<std::uint32_t>(GetPrivateProfileIntW(section, L"net_jitter_ms", 0, file.c_str()));
    g_config.netLossPct =
        static_cast<std::uint32_t>(GetPrivateProfileIntW(section, L"net_loss_pct", 0, file.c_str()));
    if (g_config.netLossPct > 90u)
    {
        g_config.netLossPct = 90u;
    }
    g_config.netShimSeed = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(
            section,
            L"net_shim_seed",
            static_cast<int>(0x51A7E5EEu),
            file.c_str()));
    g_shimState.store(g_config.netShimSeed, std::memory_order_relaxed);
    g_config.maxBattleFrames = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(section, L"max_battle_frames", 0, file.c_str()));
    g_config.forceKoAfterFrames = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(section, L"force_ko_after_frames", 0, file.c_str()));
    g_config.requiredMatches = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(section, L"required_matches", 1, file.c_str()));
    g_config.requiredMatches = std::clamp(g_config.requiredMatches, 1u, 32u);
    g_config.gameplayEscAfterFrames = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(section, L"gameplay_esc_after_frames", 0, file.c_str()));
    g_config.earlySystemKeyProbe =
        GetPrivateProfileIntW(section, L"early_system_key_probe", 0, file.c_str()) != 0;

    if (g_config.inputScript || g_config.netDelayMs != 0 || g_config.netLossPct != 0
        || g_config.maxBattleFrames != 0 || g_config.forceKoAfterFrames != 0)
    {
        lifecycle::Init(
            "harness",
            "test harness active script=%d seed=0x%08X netSeed=0x%08X delay=%ums jitter=%ums loss=%u%% maxFrames=%u forceKo=%u matches=%u gameplayEsc=%u earlySystem=%d",
            g_config.inputScript ? 1 : 0,
            g_config.inputSeed,
            g_config.netShimSeed,
            g_config.netDelayMs,
            g_config.netJitterMs,
            g_config.netLossPct,
            g_config.maxBattleFrames,
            g_config.forceKoAfterFrames,
            g_config.requiredMatches,
            g_config.gameplayEscAfterFrames,
            g_config.earlySystemKeyProbe ? 1 : 0);
        log::Info(
            "harness",
            "ACTIVE script=%d seed=0x%08X net[seed=0x%08X delay=%ums jitter=%ums loss=%u%%] maxFrames=%u forceKo=%u matches=%u gameplayEsc=%u earlySystem=%d",
            g_config.inputScript ? 1 : 0,
            g_config.inputSeed,
            g_config.netShimSeed,
            g_config.netDelayMs,
            g_config.netJitterMs,
            g_config.netLossPct,
            g_config.maxBattleFrames,
            g_config.forceKoAfterFrames,
            g_config.requiredMatches,
            g_config.gameplayEscAfterFrames,
            g_config.earlySystemKeyProbe ? 1 : 0);
    }
}

const Config& Cfg()
{
    std::call_once(g_loadOnce, LoadConfig);
    return g_config;
}

}

bool IsEnabled()
{
    const Config& c = Cfg();
    return c.inputScript || c.netDelayMs != 0 || c.netLossPct != 0 || c.maxBattleFrames != 0
        || c.forceKoAfterFrames != 0;
}

bool ScriptedInputEnabled()
{
    return Cfg().inputScript;
}

bool NetShimEnabled()
{
    const Config& c = Cfg();
    return c.netDelayMs != 0 || c.netJitterMs != 0 || c.netLossPct != 0;
}

std::uint8_t ScriptedLocalMask(const std::uint32_t frameIndex, const int localSide)
{
    // P2 only taps A: walking through each other would make both block forever.
    if (localSide == 1)
    {
        return ((frameIndex + (Cfg().inputSeed >> 5u)) % 24u) < 3u
            ? static_cast<std::uint8_t>(1u << 4u)
            : 0u;
    }

    const std::uint32_t phase = (frameIndex + Cfg().inputSeed) % 32u;
    std::uint8_t mask = static_cast<std::uint8_t>(1u << 3u);
    if ((phase % 8u) < 3u)
    {
        const std::uint32_t attack = phase / 8u;
        mask |= static_cast<std::uint8_t>(1u << (4u + attack));
        if (attack == 1u)
        {
            mask |= static_cast<std::uint8_t>(1u << 1u);
        }
    }
    return mask;
}

std::uint8_t ScriptedCharSelectMask(const std::uint32_t frameIndex)
{
    std::uint32_t epochFrame = frameIndex;
    bool preserveRematchCursor = false;
    {
        std::lock_guard lock(g_lifecycleMutex);
        if (!g_lifecycle.charSelectEpochSet)
        {
            g_lifecycle.charSelectEpochSet = true;
            g_lifecycle.charSelectEpochBase = frameIndex;
        }
        epochFrame = frameIndex - g_lifecycle.charSelectEpochBase;
        preserveRematchCursor = g_lifecycle.charSelectReturns > 0u;
    }

    constexpr std::uint32_t kSettleFrames = 90u;
    if (epochFrame < kSettleFrames)
    {
        return 0u;
    }

    const std::uint32_t rel = epochFrame - kSettleFrames;

    // Some slots crash the loading allocator: press Down once and tap A.
    constexpr std::uint32_t kNudge = 5u;
    if (!preserveRematchCursor && rel < kNudge)
    {
        return static_cast<std::uint8_t>(1u << 1);
    }
    // EFZ keeps the cursor after a match; moving Down again would land on an empty slot.
    const std::uint32_t confirmFrame = preserveRematchCursor ? rel : rel - kNudge;
    return (confirmFrame % 24u) < 3u ? static_cast<std::uint8_t>(1u << 4) : 0u;
}

std::uint8_t ScriptedSystemState(
    const std::uint32_t currentFrame,
    const std::uint32_t captureFrame,
    const std::uint32_t matchStartFrame,
    const int localSide)
{
    if (!ScriptedInputEnabled())
    {
        return 0u;
    }

    std::lock_guard lock(g_lifecycleMutex);
    ObserveLifecycleUnlocked(currentFrame, matchStartFrame);

    if (localSide == 0 && Cfg().gameplayEscAfterFrames > 0u
        && g_lifecycle.completedMatches == 0u && g_lifecycle.matchesStarted >= 1u
        && captureFrame >= matchStartFrame)
    {
        const std::uint32_t matchFrame = captureFrame - matchStartFrame;
        const std::uint32_t escFrame = Cfg().gameplayEscAfterFrames;
        if (matchFrame >= escFrame && matchFrame < escFrame + 4u)
        {
            if (!g_lifecycle.gameplayEscIssued)
            {
                g_lifecycle.gameplayEscIssued = true;
                log::Critical(
                    "harness",
                    "HARNESS gameplay ESC originated frame=%u matchFrame=%u",
                    captureFrame,
                    matchFrame);
            }
            return menu::kSystemStateEsc;
        }
    }

    if (localSide == 0 && Cfg().earlySystemKeyProbe
        && g_lifecycle.completedMatches == 0u && g_lifecycle.matchesStarted == 1u
        && captureFrame >= matchStartFrame)
    {
        if (!g_lifecycle.earlyFunctionalEpochSet)
        {
            g_lifecycle.earlyFunctionalEpochSet = true;
            g_lifecycle.earlyFunctionalEpochFrame = captureFrame;
        }
        const std::uint32_t probeFrame =
            captureFrame - g_lifecycle.earlyFunctionalEpochFrame;
        const auto pulse = [&](const std::uint32_t firstFrame,
                               const std::uint8_t bit,
                               bool& issued,
                               const char* const name) -> std::uint8_t
        {
            if (probeFrame < firstFrame || probeFrame >= firstFrame + 3u)
            {
                return 0u;
            }
            if (!issued)
            {
                issued = true;
                log::Critical(
                    "harness",
                    "HARNESS early system-key probe originated %s frame=%u",
                    name,
                    captureFrame);
            }
            return bit;
        };
        if (const std::uint8_t state = pulse(
                8u,
                menu::kSystemStateF4,
                g_lifecycle.earlyFunctionalF4Issued,
                "F4"))
        {
            return state;
        }
        if (const std::uint8_t state = pulse(
                20u,
                menu::kSystemStateF5,
                g_lifecycle.earlyFunctionalF5Issued,
                "F5"))
        {
            return state;
        }
    }

    if (localSide == 0 && g_lifecycle.completedMatches >= Cfg().requiredMatches
        && g_lifecycle.matchesStarted >= Cfg().requiredMatches + 1u
        && captureFrame >= matchStartFrame)
    {
        if (!g_lifecycle.functionalEpochSet)
        {
            g_lifecycle.functionalEpochSet = true;
            g_lifecycle.functionalEpochFrame = captureFrame;
        }
        const std::uint32_t matchFrame =
            captureFrame - g_lifecycle.functionalEpochFrame;
        const auto pulse = [&](const std::uint32_t firstFrame,
                               const std::uint8_t bit,
                               bool& issued,
                               const char* const name) -> std::uint8_t
        {
            if (matchFrame < firstFrame || matchFrame >= firstFrame + 3u)
            {
                return 0u;
            }
            if (!issued)
            {
                issued = true;
                log::Critical(
                    "harness",
                    "HARNESS gameplay system-key test originated %s frame=%u",
                    name,
                    captureFrame);
            }
            return bit;
        };

        if (const std::uint8_t state = pulse(
                8u, menu::kSystemStateF4, g_lifecycle.functionalF4Issued, "F4"))
        {
            return state;
        }
        if (const std::uint8_t state = pulse(
                20u, menu::kSystemStateF5, g_lifecycle.functionalF5Issued, "F5"))
        {
            return state;
        }
        if (const std::uint8_t state = pulse(
                32u, menu::kSystemStateF6, g_lifecycle.functionalF6Issued, "F6"))
        {
            return state;
        }
        if (const std::uint8_t state = pulse(
                44u, menu::kSystemStateF7, g_lifecycle.functionalF7Issued, "F7"))
        {
            return state;
        }
        if (const std::uint8_t state = pulse(
                56u, menu::kSystemStateF8, g_lifecycle.functionalF8Issued, "F8"))
        {
            return state;
        }
        if (const std::uint8_t state = pulse(
                68u, menu::kSystemStateIncFsf, g_lifecycle.frameStepIncIssued, "Numpad +"))
        {
            return state;
        }
        if (const std::uint8_t state = pulse(
                80u, menu::kSystemStateDecFsf, g_lifecycle.frameStepDecIssued, "Numpad -"))
        {
            return state;
        }
    }

    // Only P1 originates: P2 must receive it over SyncData.
    const int animCounter = ReadVa<int>(trial::kVaScreenAnimCounter, -1);
    const DWORD winAgeMs = g_lifecycle.actualWinDetectedAtMs != 0u
        ? GetTickCount() - g_lifecycle.actualWinDetectedAtMs
        : 0u;
    if (localSide == 0 && g_lifecycle.actualWinLatched && winAgeMs >= 2000u
        && !g_lifecycle.escapeIssuedForCurrentWin)
    {
        g_lifecycle.escapeIssuedForCurrentWin = true;
        log::Critical(
            "harness",
            "HARNESS lifecycle P1 sent synchronized ESC at win age=%lums anim=%d",
            static_cast<unsigned long>(winAgeMs),
            animCounter);
        return menu::kSystemStateEsc;
    }

    const std::uint8_t mode =
        ReadVa<std::uint8_t>(trial::kVaBattleScreenMode, 0xFFu);
    if (localSide == 0 && g_lifecycle.awaitingCharSelect
        && mode == trial::kScreenTitle && g_lifecycle.titleEpochSet
        && currentFrame - g_lifecycle.titleEpochFrame >= 120u
        && !g_lifecycle.titleEscapeIssuedForCurrentWin)
    {
        g_lifecycle.titleEscapeIssuedForCurrentWin = true;
        log::Critical(
            "harness",
            "HARNESS lifecycle P1 sent synchronized title ESC after %u frames",
            currentFrame - g_lifecycle.titleEpochFrame);
        return menu::kSystemStateEsc;
    }
    return 0u;
}

int ConsumeScriptedLiveDelayDirection(const int localSide)
{
    if (!ScriptedInputEnabled() || localSide != 0)
    {
        return 0;
    }
    std::lock_guard lock(g_lifecycleMutex);
    if (g_lifecycle.frameStepIncIssued && !g_lifecycle.liveDelayIncreaseApplied)
    {
        g_lifecycle.liveDelayIncreaseApplied = true;
        log::Critical("harness", "HARNESS consumed local Increase Input Delay shortcut");
        return 1;
    }
    if (g_lifecycle.frameStepDecIssued && !g_lifecycle.liveDelayDecreaseApplied)
    {
        g_lifecycle.liveDelayDecreaseApplied = true;
        log::Critical("harness", "HARNESS consumed local Decrease Input Delay shortcut");
        return -1;
    }
    return 0;
}

NetShimDecision DecideOutgoing()
{
    NetShimDecision decision{};
    const Config& c = Cfg();
    if (c.netLossPct > 0 && (NextShim() % 100u) < c.netLossPct)
    {
        decision.drop = true;
        return decision;
    }
    if (c.netDelayMs > 0 || c.netJitterMs > 0)
    {
        std::uint32_t delay = c.netDelayMs;
        if (c.netJitterMs > 0)
        {
            const std::uint32_t span = 2u * c.netJitterMs + 1u;
            const std::int32_t offset =
                static_cast<std::int32_t>(NextShim() % span) - static_cast<std::int32_t>(c.netJitterMs);
            const std::int32_t total = static_cast<std::int32_t>(delay) + offset;
            delay = total > 0 ? static_cast<std::uint32_t>(total) : 0u;
        }
        decision.delayMs = delay;
    }
    return decision;
}

void MaybeForceLifecycleProgress(
    const std::uint32_t currentFrame,
    const std::uint32_t matchStartFrame)
{
    const std::uint32_t threshold = Cfg().forceKoAfterFrames;
    if (threshold == 0u || currentFrame < matchStartFrame
        || currentFrame - matchStartFrame < threshold)
    {
        return;
    }

    auto* const p2 = static_cast<std::uint8_t*>(trial::ResolveVa(trial::kVaP2Player));
    if (p2 == nullptr)
    {
        return;
    }
    auto* const hp = reinterpret_cast<std::int32_t*>(p2 + trial::kOffHp);
    if (*hp > 0)
    {
        *hp = 0;
        lifecycle::Netplay(
            "HARNESS forced native KO frame=%u roundStart=%u",
            currentFrame,
            matchStartFrame);
        log::Info(
            "harness",
            "forced P2 HP=0 at frame=%u roundStart=%u",
            currentFrame,
            matchStartFrame);
    }
}

void MaybeEmitVerdict()
{
    const Config& c = Cfg();
    if (c.maxBattleFrames == 0)
    {
        return;
    }
    if (g_verdictEmitted.load(std::memory_order_acquire))
    {
        const DWORD abortAt = g_verdictAbortAtMs.load(std::memory_order_acquire);
        if (abortAt != 0u
            && static_cast<std::int32_t>(GetTickCount() - abortAt) >= 0)
        {
            netplay::CompleteHarnessSession();
        }
        return;
    }
    if (!rollback::IsEnabled())
    {
        return;
    }

    const rollback::Status status = rollback::GetStatus();
    if (status.currentFrame <= status.matchStartFrame)
    {
        return;
    }
    const std::uint32_t battleFrames = status.currentFrame - status.matchStartFrame;

    LifecycleState lifecycle{};
    {
        std::lock_guard lock(g_lifecycleMutex);
        lifecycle = g_lifecycle;
    }
    const bool functionalSystemProofComplete = caster::GetLocalSide() != 0
        || (lifecycle.functionalEpochSet && lifecycle.frameStepDecIssued);
    const bool nextMatchLive = lifecycle.matchesStarted >= c.requiredMatches + 1u
        && lifecycle.completedMatches >= c.requiredMatches
        && lifecycle.charSelectReturns >= c.requiredMatches
        && functionalSystemProofComplete
        && battleFrames >= 120u;
    const bool gameplayEscComplete = c.gameplayEscAfterFrames > 0u
        && lifecycle.gameplayEscReachedCharSelect;
    const bool hitFrameCap = lifecycle.totalBattleFrames >= c.maxBattleFrames;

    if (!nextMatchLive && !gameplayEscComplete && !hitFrameCap)
    {
        return;
    }
    if (g_verdictEmitted.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    const char* const reason = gameplayEscComplete
        ? "GAMEPLAY_ESC_CHARSELECT"
        : (nextMatchLive ? "REMATCH_LIVE" : "FRAME_CAP");
    const std::uint32_t mismatches = rollback::desync::Mismatches();
    const std::uint32_t resim = rollback::desync::ResimDivergences();
    const det::rng::Status rng = det::rng::GetStatus();
    const spectator::Status spectatorStatus = spectator::GetStatus();
    const bool spectatorOk = !spectatorStatus.hostArchiveActive
        || (spectatorStatus.rejectedGaps == 0u
            && spectatorStatus.retainedGames == lifecycle.matchesStarted
            && spectatorStatus.retainedFrames != 0u);
    const bool pass = (nextMatchLive || gameplayEscComplete)
        && mismatches == 0u && resim == 0u && spectatorOk;

    log::Critical(
        "harness",
        "HARNESS lifecycle complete via %s totalBattleFrames=%u peakRound=%u rounds=%u "
        "winscreens=%u charSelectReturns=%u matchesStarted=%u",
        reason,
        lifecycle.totalBattleFrames,
        lifecycle.peakRoundBattleFrames,
        lifecycle.roundsCompleted,
        lifecycle.completedMatches,
        lifecycle.charSelectReturns,
        lifecycle.matchesStarted);
    // One greppable line — the runner script keys off "HARNESS VERDICT".
    log::Critical(
        "harness",
        "HARNESS VERDICT %s reason=%s frames=%u totalBattleFrames=%u rounds=%u winscreens=%u "
        "charSelectReturns=%u matchesStarted=%u desyncMismatches=%u lastMismatchFrame=%u resimDiverge=%u "
        "rollbacks=%u mispredicts=%u inputs[L=%zu R=%zu] delayAdjust=%d rng[shadow=%08X calls=%u visual=%u] "
        "spectator[active=%d games=%zu frames=%zu gaps=%zu]",
        pass ? "PASS" : "FAIL",
        reason,
        battleFrames,
        lifecycle.totalBattleFrames,
        lifecycle.roundsCompleted,
        lifecycle.completedMatches,
        lifecycle.charSelectReturns,
        lifecycle.matchesStarted,
        mismatches,
        rollback::desync::LastMismatchFrame(),
        resim,
        status.rollbacksTotal,
        status.mispredictsTotal,
        status.localInputCount,
        status.remoteInputCount,
        status.delayAdjust,
        rng.shadowState,
        rng.randCalls,
        rng.visualRandCalls,
        spectatorStatus.hostArchiveActive ? 1 : 0,
        spectatorStatus.retainedGames,
        spectatorStatus.retainedFrames,
        spectatorStatus.rejectedGaps);
    lifecycle::Netplay(
        "HARNESS VERDICT %s frames=%u mismatches=%u resim=%u rollbacks=%u",
        pass ? "PASS" : "FAIL",
        battleFrames,
        mismatches,
        resim,
        status.rollbacksTotal);
    log::FlushFile();
    g_verdictAbortAtMs.store(GetTickCount() + 10000u, std::memory_order_release);
}

}
