#include "netplay_bootstrap.h"

#include "caster_input.h"
#include "control_swap.h"
#include "debug_log.h"
#include "lifecycle_log.h"
#include "mem_write_log.h"
#include "determinism.h"
#include "menu_nav.h"
#include "netplay_navigation.h"
#include "netplay_sync.h"
#include "spectator_relay.h"
#include "spectator_player.h"
#include "rollback.h"
#include "test_harness.h"
#include "trial_addresses.h"
#include "win32_compat.h"

#include <atomic>

namespace thv::netplay
{
namespace
{

enum class Phase
{
    Idle,
    WaitGameReady,
    EnterVsHuman,
    Active,
};

std::atomic<Phase> g_phase{Phase::Idle};
std::atomic<bool> g_sessionActive{false};
std::atomic<bool> g_debugVsHuman{false};
bool g_isHost = false;
int g_localSide = 0;
DWORD g_sessionStartAt = 0;
DWORD g_screenStableAt = 0;
std::uint8_t g_lastStableScreen = 0xFFu;
DWORD g_lastEnterAttemptLog = 0;
DWORD g_lastEnterAttemptAt = 0;
std::uint32_t g_enterAttempts = 0;
std::uint8_t g_lastScreen = 0xFFu;
bool g_transitionKicked = false;
DWORD g_transitionKickAt = 0;
bool g_navSyncScheduled = false;
DWORD g_navSyncAt = 0;

constexpr DWORD kMinBootDelayMs = 800;
constexpr DWORD kMainMenuDelayMs = 500;
constexpr DWORD kTitleDelayMs = 1500;
constexpr DWORD kTransitionRetryMs = 10000;
constexpr DWORD kNavSyncLeadMs = 0;

std::uint8_t ReadScreenMode()
{
    const auto* const mode = static_cast<const std::uint8_t*>(trial::ResolveVa(trial::kVaBattleScreenMode));
    return mode != nullptr ? *mode : 0xFFu;
}

int ReadScreenSubState()
{
    const auto* const sub = static_cast<const int*>(trial::ResolveVa(trial::kVaScreenSubState));
    return sub != nullptr ? *sub : -1;
}

bool IsNetplayScreen(const std::uint8_t mode)
{
    return mode == trial::kScreenCharSelect || mode == trial::kScreenLoading || mode == trial::kScreenBattle;
}

bool IsOnCharSelect(const std::uint8_t mode)
{
    return mode == trial::kScreenCharSelect;
}

bool IsPreCharSelectScreen(const std::uint8_t mode)
{
    return mode == trial::kScreenMainMenu || mode == trial::kScreenTitle;
}

void WriteInt(const std::uintptr_t va, const int value, const char* field)
{
    auto* const ptr = static_cast<int*>(trial::ResolveVa(va));
    if (ptr == nullptr)
    {
        return;
    }

    const int before = *ptr;
    if (before != value)
    {
        memlog::LogScalarWrite(
            "bootstrap",
            va,
            field,
            static_cast<std::uint64_t>(before),
            static_cast<std::uint64_t>(value),
            4);
    }
    *ptr = value;
}

void WriteByte(const std::uintptr_t va, const std::uint8_t value, const char* field)
{
    auto* const ptr = static_cast<std::uint8_t*>(trial::ResolveVa(va));
    if (ptr == nullptr)
    {
        return;
    }

    const std::uint8_t before = *ptr;
    if (before != value)
    {
        memlog::LogScalarWrite("bootstrap", va, field, before, value, 1);
    }
    *ptr = value;
}

void MaintainVsHumanFlags()
{
    if (caster::GetTransportMode() != caster::TransportMode::Netplay
        && caster::GetTransportMode() != caster::TransportMode::Spectator)
    {
        return;
    }

    const std::uint8_t mode = ReadScreenMode();
    if (!IsNetplayScreen(mode))
    {
        return;
    }

    MaintainNetplayCpuFlags();
}

const char* PhaseName(const Phase phase)
{
    switch (phase)
    {
    case Phase::Idle:
        return "Idle";
    case Phase::WaitGameReady:
        return "WaitGameReady";
    case Phase::EnterVsHuman:
        return "EnterVsHuman";
    case Phase::Active:
        return "Active";
    default:
        return "?";
    }
}

void SetPhase(const Phase next)
{
    const Phase prev = g_phase.exchange(next);
    if (prev != next)
    {
        lifecycle::Phase("bootstrap", PhaseName(prev), PhaseName(next));
        log::Step("netplay", "bootstrap phase %s -> %s", PhaseName(prev), PhaseName(next));
        switch (next)
        {
        case Phase::WaitGameReady:
            nav::SetPhase(nav::Phase::AwaitTitle);
            nav::SetPacketStage(nav::PacketStage::NavTitle);
            break;
        case Phase::EnterVsHuman:
            nav::SetPhase(nav::Phase::EnterVsHuman);
            nav::SetPacketStage(nav::PacketStage::NavCharSelect);
            break;
        case Phase::Active:
            nav::SetPhase(nav::Phase::Active);
            break;
        default:
            break;
        }
    }
}

void EnableDirectInputForMenus()
{
    auto* const diActive = static_cast<std::uint32_t*>(trial::ResolveVa(trial::kVaDiActiveFlag));
    if (diActive != nullptr)
    {
        const std::uint32_t before = *diActive;
        if (before != 1u)
        {
            memlog::LogScalarWrite("bootstrap", trial::kVaDiActiveFlag, "di_active", before, 1u, 4);
        }
        *diActive = 1u;
    }
}

void SetPlayerVsHumanTypeBytes()
{
    auto* const p1 = static_cast<std::uint16_t*>(trial::ResolveVa(trial::kVaP1Player));
    auto* const p2 = static_cast<std::uint16_t*>(trial::ResolveVa(trial::kVaP2Player));
    if (p1 != nullptr)
    {
        auto* const typeByte = reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uint8_t*>(p1) + 1u);
        const std::uint8_t before = *typeByte;
        if (before != 0u)
        {
            memlog::LogScalarWrite("bootstrap", trial::kVaP1Player + 1u, "p1_type_byte", before, 0u, 1);
        }
        *typeByte = 0u;
    }
    if (p2 != nullptr)
    {
        auto* const typeByte = reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uint8_t*>(p2) + 1u);
        const std::uint8_t before = *typeByte;
        if (before != 0u)
        {
            memlog::LogScalarWrite("bootstrap", trial::kVaP2Player + 1u, "p2_type_byte", before, 0u, 1);
        }
        *typeByte = 0u;
    }
}

void SetVsHumanModeGlobals()
{
    WriteByte(trial::kVaMainMenuCursor, trial::kMainMenuVsHuman, "main_menu_cursor");
    WriteInt(trial::kVaVsHumanFlagP1, 1, "vs_human_p1");
    WriteInt(trial::kVaVsHumanFlagP2, 1, "vs_human_p2");
}

void ResetCharSelectState()
{
    WriteByte(trial::kVaCharSelectP1State, 0, "charsel_p1_state");
    WriteByte(trial::kVaCharSelectP2State, 0, "charsel_p2_state");
    WriteInt(trial::kVaCharSelectP1Pick, -1, "charsel_p1_pick");
    WriteInt(trial::kVaCharSelectP2Pick, -1, "charsel_p2_pick");
    WriteInt(trial::kVaCharSelectTimerP1, 0, "charsel_p1_timer");
    WriteInt(trial::kVaCharSelectTimerP2, 0, "charsel_p2_timer");

    auto* const selectSide = static_cast<int*>(trial::ResolveVa(trial::kVaCharSelectSide));
    if (selectSide != nullptr)
    {
        const int before = *selectSide;
        if (before != 0)
        {
            memlog::LogScalarWrite("bootstrap", trial::kVaCharSelectSide, "charsel_side", before, 0, 4);
        }
        *selectSide = 0;
    }
}

void TrackScreenMode(const std::uint8_t screen)
{
    if (g_lastScreen == screen)
    {
        return;
    }

    const std::uint8_t prev = g_lastScreen;
    g_lastScreen = screen;
    if (prev != 0xFFu)
    {
        nav::OnTrialScreenMode(prev, screen);
    }
}

void UpdateScreenStability(const std::uint8_t screen)
{
    if (screen != g_lastStableScreen)
    {
        g_lastStableScreen = screen;
        g_screenStableAt = GetTickCount();
        lifecycle::Netplay(
            "screen stable mode=%u (%s)",
            static_cast<unsigned>(screen),
            nav::TrialScreenName(screen));
        log::Step(
            "netplay",
            "screen stable at mode=%u (%s)",
            static_cast<unsigned>(screen),
            nav::TrialScreenName(screen));
    }
}

bool BootDelayElapsed()
{
    return g_sessionStartAt != 0 && GetTickCount() - g_sessionStartAt >= kMinBootDelayMs;
}

bool ScreenStableFor(const DWORD ms)
{
    return g_screenStableAt != 0 && GetTickCount() - g_screenStableAt >= ms;
}

bool SafeToKickTransition(const std::uint8_t screen)
{
    if (!BootDelayElapsed())
    {
        return false;
    }

    if (IsOnCharSelect(screen))
    {
        return true;
    }

    if (screen == trial::kScreenMainMenu && ScreenStableFor(kMainMenuDelayMs))
    {
        return true;
    }

    if (screen == trial::kScreenTitle && ScreenStableFor(kTitleDelayMs))
    {
        return true;
    }

    return false;
}

bool KickNativeVsHumanTransition(const std::uint8_t screen)
{
    if (IsOnCharSelect(screen) || !IsPreCharSelectScreen(screen))
    {
        return false;
    }

    ++g_enterAttempts;
    g_lastEnterAttemptAt = GetTickCount();

    MaintainVsHumanFlags();
    EnableDirectInputForMenus();
    SetVsHumanModeGlobals();
    SetPlayerVsHumanTypeBytes();
    ResetCharSelectState();

    // Never call DirectDraw/UI setup from the mod thread.
    const int subState = (screen == trial::kScreenTitle) ? trial::kScreenSubTitleVsHuman
                                                         : trial::kScreenSubMainMenuVsHuman;
    WriteInt(trial::kVaScreenSubState, subState, "screen_substate");
    WriteInt(trial::kVaScreenAnimCounter, trial::kNativeTransitionNearComplete, "screen_anim");

    lifecycle::Netplay(
        "native VS Human kick screen=%u (%s) sub=%d anim=%d local=P%d attempt=%u",
        static_cast<unsigned>(screen),
        nav::TrialScreenName(screen),
        subState,
        trial::kNativeTransitionNearComplete,
        g_localSide + 1,
        g_enterAttempts);
    log::Step(
        "netplay",
        "native VS Human kick screen=%u (%s) sub=%d anim=%d local=P%d attempt=%u",
        static_cast<unsigned>(screen),
        nav::TrialScreenName(screen),
        subState,
        trial::kNativeTransitionNearComplete,
        g_localSide + 1,
        g_enterAttempts);
    return true;
}

void LogTransitionPending(const std::uint8_t screen)
{
    const DWORD now = GetTickCount();
    if (now - g_lastEnterAttemptLog <= 2000)
    {
        return;
    }

    g_lastEnterAttemptLog = now;
    lifecycle::Netplay(
        "VS Human transition pending screen=%u (%s) sub=%d kicked=%d bootMs=%lu stableMs=%lu",
        static_cast<unsigned>(screen),
        nav::TrialScreenName(screen),
        ReadScreenSubState(),
        g_transitionKicked ? 1 : 0,
        g_sessionStartAt != 0 ? now - g_sessionStartAt : 0u,
        g_screenStableAt != 0 ? now - g_screenStableAt : 0u);
    log::Warn(
        "netplay",
        "VS Human transition pending screen=%u (%s) sub=%d kicked=%d bootMs=%lu stableMs=%lu",
        static_cast<unsigned>(screen),
        nav::TrialScreenName(screen),
        ReadScreenSubState(),
        g_transitionKicked ? 1 : 0,
        g_sessionStartAt != 0 ? now - g_sessionStartAt : 0u,
        g_screenStableAt != 0 ? now - g_screenStableAt : 0u);
}

bool ReadyToTeleport(const std::uint8_t screen)
{
    if (!BootDelayElapsed())
    {
        return false;
    }

    if (IsOnCharSelect(screen))
    {
        return true;
    }

    if (screen == trial::kScreenMainMenu && ScreenStableFor(kMainMenuDelayMs))
    {
        return true;
    }

    if (screen == trial::kScreenTitle && ScreenStableFor(kTitleDelayMs))
    {
        return true;
    }

    if (g_debugVsHuman.load() && ScreenStableFor(kMainMenuDelayMs))
    {
        return true;
    }

    return false;
}

bool NetplayGatesOpen()
{
    if (g_debugVsHuman.load())
    {
        return true;
    }

    if (!IsDeterminismSynced())
    {
        return false;
    }

    if (!IsRollbackRequired())
    {
        return true;
    }

    return IsRollbackNegotiated();
}

void ResetBootstrapTiming()
{
    g_sessionStartAt = GetTickCount();
    g_screenStableAt = 0;
    g_lastStableScreen = 0xFFu;
    g_lastEnterAttemptAt = 0;
    g_lastEnterAttemptLog = 0;
    g_enterAttempts = 0;
    g_lastScreen = 0xFFu;
    g_transitionKicked = false;
    g_transitionKickAt = 0;
    g_navSyncScheduled = false;
    g_navSyncAt = 0;
}

bool NavSyncReady()
{
    return g_navSyncScheduled
        && (g_navSyncAt == 0u
            || win32::TickDeadlineReached(GetTickCount(), g_navSyncAt));
}

void ScheduleNavSync(const bool hostSendsLaunchGo)
{
    if (g_navSyncScheduled)
    {
        return;
    }

    if (hostSendsLaunchGo)
    {
        SendLaunchGo();
    }

    g_navSyncAt = GetTickCount() + kNavSyncLeadMs;
    g_navSyncScheduled = true;
    lifecycle::Netplay("nav sync scheduled VS Human in %u ms", kNavSyncLeadMs);
    log::Step("netplay", "nav sync scheduled — VS Human in %u ms", kNavSyncLeadMs);
}

void MarkNavSyncReadyNow()
{
    g_navSyncAt = GetTickCount();
    g_navSyncScheduled = true;
}

void UpdateLaunchGoRetransmit(const Phase phase)
{
    const bool retransmit =
        g_isHost && !g_debugVsHuman.load() && (phase == Phase::WaitGameReady || phase == Phase::EnterVsHuman);
    SetLaunchGoRetransmit(retransmit && phase != Phase::Active);
}

}

void BeginSession(const session::LauncherSession& session)
{
    lifecycle::Netplay(
        "BeginSession mode=%d host=%d local=P%d port=%u peerPort=%u delay=%u bootstrap=%d rollback=%d autopunch=%d",
        static_cast<int>(session.mode),
        session.isHost ? 1 : 0,
        session.localIsP1 ? 1 : 2,
        static_cast<unsigned>(session.port),
        static_cast<unsigned>(session.peerPort),
        static_cast<unsigned>(session.inputDelay),
        static_cast<int>(session.bootstrap),
        session.requireRollback ? 1 : 0,
        session.usedAutopunch ? 1 : 0);
    log::Step(
        "netplay",
        "BeginSession mode=%d host=%d localP%d port=%u peerPort=%u delay=%u bootstrap=%d rollback=%d autopunch=%d",
        static_cast<int>(session.mode),
        session.isHost ? 1 : 0,
        session.localIsP1 ? 1 : 2,
        static_cast<unsigned>(session.port),
        static_cast<unsigned>(session.peerPort),
        static_cast<unsigned>(session.inputDelay),
        static_cast<int>(session.bootstrap),
        session.requireRollback ? 1 : 0,
        session.usedAutopunch ? 1 : 0);
    if (session.mode != session::LauncherMode::Host && session.mode != session::LauncherMode::Client)
    {
        Shutdown();
        return;
    }

    nav::Reset();
    g_debugVsHuman.store(false);
    g_isHost = session.isHost;
    g_localSide = session.localIsP1 ? 0 : 1;
    g_sessionActive.store(true);
    ResetBootstrapTiming();

    ConfigureNetplaySides(g_localSide);
    caster::SetTransportMode(caster::TransportMode::Netplay);
    det::RepinFpu();

    if (session.requireRollback)
    {
        rollback::ArmForNetplay();
        log::Info("netplay", "rollback armed — lockstep until battle handoff");
    }
    else
    {
        rollback::DisarmNetplay();
        rollback::SetEnabled(false);
    }
    rollback::SetInputDelay(session.inputDelay);
    rollback::SetPingMs(session.pingMs);

    if (!InitializeFromSession(session))
    {
        lifecycle::Exit("netplay", "sync UDP init failed", nullptr);
        log::Error("netplay", "sync UDP init failed");
        AbortNetplaySession("sync UDP or determinism handshake init failed");
        return;
    }
    spectator::BeginHostArchive(session);

    nav::SetPhase(nav::Phase::AwaitDeterminism);

    if (session.bootstrap == session::BootstrapMode::DebugVsHuman)
    {
        log::Step("netplay", "debug bootstrap — skip launch-go, teleport VS Human");
        g_debugVsHuman.store(true);
        SetPhase(Phase::EnterVsHuman);
        return;
    }

    SetPhase(Phase::WaitGameReady);
    log::Info(
        "netplay",
        "bootstrap %s — waiting for main menu/title",
        g_isHost ? "host" : "client");
}

void BeginDebugVsHuman(const int localSide)
{
    log::Step("netplay", "BeginDebugVsHuman local=P%d", localSide + 1);
    nav::Reset();
    g_debugVsHuman.store(true);
    g_isHost = false;
    g_localSide = (localSide == 0 || localSide == 1) ? localSide : 0;
    g_sessionActive.store(true);
    ResetBootstrapTiming();

    RestorePracticeBothHuman(g_localSide);
    caster::SetTransportMode(caster::TransportMode::Offline);
    det::RepinFpu();

    nav::SetPhase(nav::Phase::EnterVsHuman);
    nav::SetPacketStage(nav::PacketStage::NavCharSelect);
    SetPhase(Phase::EnterVsHuman);
}

void Shutdown()
{
    lifecycle::Phase("netplay", "active", "shutdown", PhaseName(g_phase.load()));
    log::Step("netplay", "Shutdown");
    spectator::player::Shutdown();
    if (!IsNetplayAborted())
    {
        // Send Quit before either source drains, or the peer truncates its viewers.
        SendQuit();
        NotifySpectatorsSourceEnded(false);
    }
    spectator::EndSession();
    g_sessionActive.store(false);
    g_debugVsHuman.store(false);
    g_lastScreen = 0xFFu;
    g_navSyncScheduled = false;
    g_navSyncAt = 0;
    SetLaunchGoRetransmit(false);
    caster::SetTransportMode(caster::TransportMode::Offline);
    rollback::DisarmNetplay();
    ResetNetplaySideConfig();
    SetPhase(Phase::Idle);
    nav::Reset();
    CloseSync();
}

void OnModTick(const HWND gameWindow)
{
    if (!g_sessionActive.load())
    {
        return;
    }

    if (IsNetplayAborted())
    {
        if (IsTerminalHandoffInProgress())
        {
            // Stay alive until the owner's terminal handoff ends.
            return;
        }
        lifecycle::Exit("netplay", GetNetplayAbortReason(), "bootstrap halted");
        log::FlushFile();
        if (gameWindow != nullptr)
        {
            PostMessageA(gameWindow, WM_CLOSE, 0, 0);
        }
        Sleep(50);
        ExitProcess(0);
    }

    if (!g_debugVsHuman.load())
    {
        lifecycle::Heartbeat();

        static std::uint32_t s_lastDiagMs = 0u;
        const std::uint32_t nowMs = static_cast<std::uint32_t>(GetTickCount());
        if (nowMs - s_lastDiagMs >= 1000u)
        {
            s_lastDiagMs = nowMs;
            LogDiagnosticHeartbeat();
            harness::MaybeEmitVerdict();
        }
    }

    const Phase phase = g_phase.load();
    UpdateLaunchGoRetransmit(phase);

    const std::uint8_t screen = ReadScreenMode();
    TrackScreenMode(screen);
    UpdateScreenStability(screen);

    const menu::NavScreen navScreen = menu::DetectNavScreen();
    switch (navScreen)
    {
    case menu::NavScreen::Loading:
        nav::SetPhase(nav::Phase::LoadingSync);
        nav::SetPacketStage(nav::PacketStage::NavLoading);
        break;
    case menu::NavScreen::CharSelect:
        if (phase == Phase::Active)
        {
            nav::SetPhase(nav::Phase::CharSelectLockstep);
        }
        break;
    case menu::NavScreen::Battle:
        nav::SetPhase(nav::Phase::BattleLockstep);
        break;
    default:
        break;
    }

    if (caster::IsNetplayActive()
        && screen == trial::kScreenBattle
        && rollback::IsArmedForNetplay())
    {
        rollback::StartNetplaySession();
    }

    if (phase == Phase::Active || phase == Phase::EnterVsHuman)
    {
        MaintainVsHumanFlags();
    }
    else if (caster::IsNetplayActive() && IsNetplayScreen(screen))
    {
        MaintainVsHumanFlags();
    }

    switch (phase)
    {
    case Phase::WaitGameReady:
        if (!NetplayGatesOpen())
        {
            break;
        }
        if (!g_isHost && LaunchGoReceived() && !g_navSyncScheduled)
        {
            MarkNavSyncReadyNow();
            lifecycle::Netplay("client launch-go ok nav sync ready");
            log::Info("netplay", "client launch-go ok — nav sync ready");
        }
        if (IsOnCharSelect(screen))
        {
            lifecycle::Phase("bootstrap", PhaseName(phase), "Active", "already on char select");
            log::Step("netplay", "already on char select — session active");
            SetPhase(Phase::Active);
            break;
        }
        if (ReadyToTeleport(screen))
        {
            if (!g_navSyncScheduled)
            {
                ScheduleNavSync(g_isHost);
            }
            if (NavSyncReady())
            {
                lifecycle::Netplay(
                    "teleport VS Human from screen=%u (%s)",
                    static_cast<unsigned>(screen),
                    nav::TrialScreenName(screen));
                log::Step(
                    "netplay",
                    "teleport VS Human from screen=%u (%s)",
                    static_cast<unsigned>(screen),
                    nav::TrialScreenName(screen));
                SetPhase(Phase::EnterVsHuman);
            }
        }
        break;

    case Phase::EnterVsHuman:
        if (!NetplayGatesOpen())
        {
            break;
        }
        if (IsOnCharSelect(screen))
        {
            g_transitionKicked = false;
            lifecycle::Netplay(
                "VS Human char select ready screen=%u sub=%d local=P%d attempts=%u",
                static_cast<unsigned>(screen),
                ReadScreenSubState(),
                g_localSide + 1,
                g_enterAttempts);
            log::Info(
                "netplay",
                "VS Human char select ready screen=%u sub=%d local=P%d attempts=%u",
                static_cast<unsigned>(screen),
                ReadScreenSubState(),
                g_localSide + 1,
                g_enterAttempts);
            SetPhase(Phase::Active);
            break;
        }
        if (!SafeToKickTransition(screen))
        {
            LogTransitionPending(screen);
            break;
        }
        if (g_transitionKicked)
        {
            const DWORD now = GetTickCount();
            if (g_transitionKickAt != 0 && now - g_transitionKickAt >= kTransitionRetryMs)
            {
                lifecycle::Fault(
                    "netplay",
                    "VS Human transition timeout screen=%u (%s) retrying kick",
                    static_cast<unsigned>(screen),
                    nav::TrialScreenName(screen));
                log::Warn(
                    "netplay",
                    "VS Human transition timed out on screen=%u (%s) — retrying kick",
                    static_cast<unsigned>(screen),
                    nav::TrialScreenName(screen));
                g_transitionKicked = false;
                g_transitionKickAt = 0;
            }
            else
            {
                LogTransitionPending(screen);
            }
            break;
        }
        if (KickNativeVsHumanTransition(screen))
        {
            g_transitionKicked = true;
            g_transitionKickAt = GetTickCount();
        }
        break;

    case Phase::Active:
        if (IsNetplayScreen(screen))
        {
            MaintainVsHumanFlags();
        }
        if (screen == trial::kScreenBattle && caster::IsNetplayActive())
        {
            nav::SetPhase(nav::Phase::BattleLockstep);
        }
        break;

    default:
        break;
    }
}

const char* GetBootstrapPhaseName()
{
    return PhaseName(g_phase.load());
}

bool IsSessionActive()
{
    return g_sessionActive.load();
}

void ForceEnterVsHuman()
{
    lifecycle::Netplay("ForceEnterVsHuman requested");
    log::Step("netplay", "ForceEnterVsHuman requested");
    g_debugVsHuman.store(true);
    g_transitionKicked = false;
    g_transitionKickAt = 0;
    SetPhase(Phase::EnterVsHuman);
}

}
