#include "menu_nav.h"

#include "caster_input.h"
#include "control_config.h"
#include "debug_log.h"
#include "framestep.h"
#include "sdl_input.h"
#include "trial_addresses.h"

#include <windows.h>

#include <atomic>

namespace thv::menu
{
namespace
{

std::uint8_t ReadScreenMode()
{
    const auto* const mode = static_cast<const std::uint8_t*>(trial::ResolveVa(trial::kVaBattleScreenMode));
    return mode != nullptr ? *mode : 0xFFu;
}

std::atomic<bool> g_onlinePracticeKeysAllowed{false};

}

const char* NavScreenName(const NavScreen screen)
{
    switch (screen)
    {
    case NavScreen::MainMenu:
        return "main_menu";
    case NavScreen::Title:
        return "title";
    case NavScreen::CharSelect:
        return "char_select";
    case NavScreen::Loading:
        return "loading";
    case NavScreen::Battle:
        return "battle";
    case NavScreen::Post:
        return "post";
    default:
        return "none";
    }
}

bool IsPostMatchScreen()
{
    const auto* const p1Move = static_cast<const std::int16_t*>(trial::ResolveVa(trial::kVaP1MoveId));
    const auto* const p2Move = static_cast<const std::int16_t*>(trial::ResolveVa(trial::kVaP2Player + trial::kOffMoveId));
    if (p1Move != nullptr
        && (*p1Move == trial::kWinScreenMoveIdA || *p1Move == trial::kWinScreenMoveIdB))
    {
        return true;
    }
    if (p2Move != nullptr
        && (*p2Move == trial::kWinScreenMoveIdA || *p2Move == trial::kWinScreenMoveIdB))
    {
        return true;
    }

    const std::uint8_t mode = ReadScreenMode();
    if (mode != trial::kScreenLoading)
    {
        return false;
    }

    const auto* const sub = static_cast<const int*>(trial::ResolveVa(trial::kVaScreenSubState));
    return sub != nullptr && *sub >= trial::kPostMatchSubStateMin && *sub <= trial::kPostMatchSubStateMax;
}

NavScreen DetectNavScreen()
{
    if (IsPostMatchScreen())
    {
        const std::uint8_t mode = ReadScreenMode();
        if (mode == trial::kScreenBattle || mode == trial::kScreenLoading)
        {
            return NavScreen::Post;
        }
    }

    const std::uint8_t mode = ReadScreenMode();
    switch (mode)
    {
    case 0u:
        return NavScreen::MainMenu;
    case 4u:
        return NavScreen::Title;
    case 2u:
        return NavScreen::CharSelect;
    case 3u:
        return NavScreen::Loading;
    case 1u:
        return NavScreen::Battle;
    default:
        return NavScreen::None;
    }
}

bool IsSyncEligible(const NavScreen screen)
{
    return screen == NavScreen::MainMenu || screen == NavScreen::Title || screen == NavScreen::CharSelect
        || screen == NavScreen::Loading || screen == NavScreen::Battle || screen == NavScreen::Post;
}

bool IsFightInputAllowed(const NavScreen screen)
{
    return screen == NavScreen::Loading || screen == NavScreen::Battle || screen == NavScreen::Post;
}

bool UsesLockstepWireDiOverlay(const NavScreen screen)
{
    return screen == NavScreen::CharSelect || screen == NavScreen::Loading;
}

bool IsFightInputAllowedNow()
{
    return IsFightInputAllowed(DetectNavScreen());
}

bool IsSyncEligibleNow()
{
    return IsSyncEligible(DetectNavScreen());
}

std::uint8_t SampleLocalSystemState()
{
    const HWND gameWindow = thv::ResolveGameWindow();
    if (gameWindow == nullptr || GetForegroundWindow() != gameWindow)
    {
        return 0;
    }

    std::uint8_t state = 0;

    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0)
    {
        state |= kSystemStateEsc;
    }

    const bool battle = DetectNavScreen() == NavScreen::Battle;
    const int bindingPlayer = caster::LocalBindingPlayerIndex();
    const auto shortcutDown = [bindingPlayer](const trialnet::shortcuts::Action action) {
        return !controls::ShortcutConflictsWithFightBinding(action, bindingPlayer)
            && input::IsDownNow(controls::ShortcutKey(action));
    };
    if (battle && shortcutDown(trialnet::shortcuts::Action::TrainingMode))
    {
        state |= kSystemStateF4;
    }
    if (battle && shortcutDown(trialnet::shortcuts::Action::RestoreResources))
    {
        state |= kSystemStateF5;
    }
    return FilterOnlineSystemState(state);
}

void SetOnlinePracticeKeysAllowed(const bool allowed)
{
    g_onlinePracticeKeysAllowed.store(allowed, std::memory_order_release);
    log::Info(
        "system",
        "online practice keys %s (wire mask=0x%02X)",
        allowed ? "negotiated on" : "off",
        static_cast<unsigned>(allowed ? kOnlineKnownSystemMask : kSystemStateEsc));
}

bool OnlinePracticeKeysAllowed()
{
    return g_onlinePracticeKeysAllowed.load(std::memory_order_acquire);
}

std::uint8_t AllowedOnlineSystemMask()
{
    return AllowedOnlineSystemMaskForPolicy(OnlinePracticeKeysAllowed());
}

std::uint8_t FilterOnlineSystemState(const std::uint8_t state)
{
    return FilterOnlineSystemStateForPolicy(state, OnlinePracticeKeysAllowed());
}

std::uint8_t FilterRecordedSystemState(const std::uint8_t state)
{
    return static_cast<std::uint8_t>(state & kOnlineKnownSystemMask);
}

bool ApplyTrialPracticeSystemState(const std::uint8_t rawState)
{
    const std::uint8_t state = static_cast<std::uint8_t>(
        rawState & kOnlinePracticeSystemMask);
    if (state == 0u)
    {
        return true;
    }

    auto* const p1 = static_cast<std::uint8_t*>(trial::ResolveVa(trial::kVaP1Player));
    auto* const p2 = static_cast<std::uint8_t*>(trial::ResolveVa(trial::kVaP2Player));
    if (p1 == nullptr || p2 == nullptr)
    {
        return false;
    }

    if ((state & kSystemStateF4) != 0u)
    {
        *reinterpret_cast<std::int32_t*>(p1 + trial::kOffTrainingModeFlag) = 1;
        *reinterpret_cast<std::int32_t*>(p2 + trial::kOffTrainingModeFlag) = 1;
    }
    if ((state & kSystemStateF5) != 0u)
    {
        *reinterpret_cast<std::int32_t*>(p1 + trial::kOffHp) = 9999;
        *reinterpret_cast<std::int32_t*>(p2 + trial::kOffHp) = 9999;
        *reinterpret_cast<double*>(p1 + trial::kOffEForce) = 1000.0;
        *reinterpret_cast<double*>(p2 + trial::kOffEForce) = 1000.0;
        *reinterpret_cast<std::int16_t*>(p1 + trial::kOffSpStock) = 4000;
        *reinterpret_cast<std::int16_t*>(p2 + trial::kOffSpStock) = 4000;
    }
    return true;
}

}
