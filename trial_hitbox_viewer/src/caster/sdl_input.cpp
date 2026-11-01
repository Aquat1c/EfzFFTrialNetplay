#include "sdl_input.h"

#include "caster_input.h"
#include "control_config.h"
#include "control_swap.h"
#include "controller_device_routing.h"
#include "debug_log.h"
#include "framestep.h"
#include "lifecycle_log.h"
#include "menu_nav.h"
#include "offline_input_routing.h"
#include "rollback.h"
#include "test_harness.h"
#include "trial_input_codec.h"
#include "trial_addresses.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>

#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace thv::input
{
namespace
{

constexpr std::uint8_t kDiKeyDown = 0x80u;
constexpr int kStickDeadzone = 8000;

std::atomic<bool> g_initialized{false};
std::atomic<bool> g_gamepadEnabled{false};

std::array<bool, SDL_SCANCODE_COUNT> g_current{};
std::array<bool, SDL_SCANCODE_COUNT> g_previous{};
std::array<bool, SDL_SCANCODE_COUNT> g_pressed{};

struct ControllerHandle
{
    SDL_JoystickID id = 0;
    SDL_Gamepad* gamepad = nullptr;
    SDL_Joystick* joystick = nullptr;
    bool ownsJoystick = false;
    char name[128]{};
};

std::array<ControllerHandle, 2> g_playerControllers{};
std::array<bool, 2> g_controllerSources{};
std::mutex g_controllerMutex;
std::atomic<Uint64> g_nextControllerScanMs{0};
std::atomic<Uint64> g_lastGameThreadControllerPumpMs{~Uint64{0}};

std::atomic<std::uint8_t> g_mergedSystemState{0};
std::atomic<std::uint8_t> g_spectatorPendingSystemEdges{0};
bool g_escOutputLatched = false;
bool g_f4OutputLatched = false;
bool g_f5OutputLatched = false;
bool g_f6OutputLatched = false;
bool g_f7OutputLatched = false;
bool g_f8OutputLatched = false;
bool g_incFsfOutputLatched = false;
bool g_decFsfOutputLatched = false;

bool IsGameFocused()
{
    const HWND game = ResolveGameWindow();
    if (game == nullptr)
    {
        return false;
    }
    return GetForegroundWindow() == game;
}

int VkFromScancode(SDL_Scancode scancode);

SDL_Scancode SlotScancode(const int playerIndex, const int slot)
{
    const SDL_Scancode configured = controls::KeyForPlayer(playerIndex, slot);
    if (slot < 0 || slot >= kFightSlotCount)
    {
        return SDL_SCANCODE_UNKNOWN;
    }
    return configured != SDL_SCANCODE_UNKNOWN && VkFromScancode(configured) != 0
        ? configured
        : SDL_SCANCODE_UNKNOWN;
}

int VkFromScancode(SDL_Scancode scancode)
{
    switch (scancode)
    {
    case SDL_SCANCODE_UP:
        return VK_UP;
    case SDL_SCANCODE_DOWN:
        return VK_DOWN;
    case SDL_SCANCODE_LEFT:
        return VK_LEFT;
    case SDL_SCANCODE_RIGHT:
        return VK_RIGHT;
    case SDL_SCANCODE_ESCAPE:
        return VK_ESCAPE;
    case SDL_SCANCODE_RETURN:
        return VK_RETURN;
    case SDL_SCANCODE_BACKSPACE:
        return VK_BACK;
    case SDL_SCANCODE_TAB:
        return VK_TAB;
    case SDL_SCANCODE_SPACE:
        return VK_SPACE;
    case SDL_SCANCODE_PAGEUP:
        return VK_PRIOR;
    case SDL_SCANCODE_PAGEDOWN:
        return VK_NEXT;
    case SDL_SCANCODE_HOME:
        return VK_HOME;
    case SDL_SCANCODE_END:
        return VK_END;
    case SDL_SCANCODE_INSERT:
        return VK_INSERT;
    case SDL_SCANCODE_DELETE:
        return VK_DELETE;
    case SDL_SCANCODE_CAPSLOCK:
        return VK_CAPITAL;
    case SDL_SCANCODE_SCROLLLOCK:
        return VK_SCROLL;
    case SDL_SCANCODE_NUMLOCKCLEAR:
        return VK_NUMLOCK;
    case SDL_SCANCODE_PRINTSCREEN:
        return VK_SNAPSHOT;
    case SDL_SCANCODE_PAUSE:
        return VK_PAUSE;
    case SDL_SCANCODE_LSHIFT:
        return VK_LSHIFT;
    case SDL_SCANCODE_RSHIFT:
        return VK_RSHIFT;
    case SDL_SCANCODE_LCTRL:
        return VK_LCONTROL;
    case SDL_SCANCODE_RCTRL:
        return VK_RCONTROL;
    case SDL_SCANCODE_LALT:
        return VK_LMENU;
    case SDL_SCANCODE_RALT:
        return VK_RMENU;
    case SDL_SCANCODE_LGUI:
        return VK_LWIN;
    case SDL_SCANCODE_RGUI:
        return VK_RWIN;
    case SDL_SCANCODE_APPLICATION:
        return VK_APPS;
    case SDL_SCANCODE_COMMA:
        return VK_OEM_COMMA;
    case SDL_SCANCODE_PERIOD:
        return VK_OEM_PERIOD;
    case SDL_SCANCODE_SLASH:
        return VK_OEM_2;
    case SDL_SCANCODE_SEMICOLON:
        return VK_OEM_1;
    case SDL_SCANCODE_MINUS:
        return VK_OEM_MINUS;
    case SDL_SCANCODE_EQUALS:
        return VK_OEM_PLUS;
    case SDL_SCANCODE_LEFTBRACKET:
        return VK_OEM_4;
    case SDL_SCANCODE_RIGHTBRACKET:
        return VK_OEM_6;
    case SDL_SCANCODE_BACKSLASH:
        return VK_OEM_5;
    case SDL_SCANCODE_NONUSBACKSLASH:
        return VK_OEM_102;
    case SDL_SCANCODE_APOSTROPHE:
        return VK_OEM_7;
    case SDL_SCANCODE_GRAVE:
        return VK_OEM_3;
    case SDL_SCANCODE_F1:
        return VK_F1;
    case SDL_SCANCODE_KP_0:
        return VK_NUMPAD0;
    case SDL_SCANCODE_KP_1:
        return VK_NUMPAD1;
    case SDL_SCANCODE_KP_2:
        return VK_NUMPAD2;
    case SDL_SCANCODE_KP_3:
        return VK_NUMPAD3;
    case SDL_SCANCODE_KP_4:
        return VK_NUMPAD4;
    case SDL_SCANCODE_KP_5:
        return VK_NUMPAD5;
    case SDL_SCANCODE_KP_6:
        return VK_NUMPAD6;
    case SDL_SCANCODE_KP_7:
        return VK_NUMPAD7;
    case SDL_SCANCODE_KP_8:
        return VK_NUMPAD8;
    case SDL_SCANCODE_KP_9:
        return VK_NUMPAD9;
    case SDL_SCANCODE_KP_ENTER:
        return VK_RETURN;
    case SDL_SCANCODE_KP_PLUS:
        return VK_ADD;
    case SDL_SCANCODE_KP_MINUS:
        return VK_SUBTRACT;
    case SDL_SCANCODE_KP_MULTIPLY:
        return VK_MULTIPLY;
    case SDL_SCANCODE_KP_DIVIDE:
        return VK_DIVIDE;
    case SDL_SCANCODE_KP_PERIOD:
        return VK_DECIMAL;
    case SDL_SCANCODE_KP_COMMA:
        return VK_SEPARATOR;
    default:
        if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
        {
            return static_cast<int>('A' + (scancode - SDL_SCANCODE_A));
        }
        if (scancode >= SDL_SCANCODE_0 && scancode <= SDL_SCANCODE_9)
        {
            return static_cast<int>('0' + (scancode - SDL_SCANCODE_0));
        }
        if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12)
        {
            return VK_F1 + static_cast<int>(scancode - SDL_SCANCODE_F1);
        }
        if (scancode >= SDL_SCANCODE_F13 && scancode <= SDL_SCANCODE_F24)
        {
            return VK_F13 + static_cast<int>(scancode - SDL_SCANCODE_F13);
        }
        return 0;
    }
}

bool ReadWin32Down(SDL_Scancode scancode)
{
    const int vk = VkFromScancode(scancode);
    if (vk == 0)
    {
        return false;
    }
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void TrackScancode(const SDL_Scancode scancode)
{
    if (scancode == SDL_SCANCODE_UNKNOWN || scancode < 0 || scancode >= SDL_SCANCODE_COUNT)
    {
        return;
    }

    g_current[static_cast<std::size_t>(scancode)] = ReadWin32Down(scancode);
}

void UpdateEdges()
{
    for (int i = 0; i < SDL_SCANCODE_COUNT; ++i)
    {
        g_pressed[static_cast<std::size_t>(i)] =
            g_current[static_cast<std::size_t>(i)] && !g_previous[static_cast<std::size_t>(i)];
        g_previous[static_cast<std::size_t>(i)] = g_current[static_cast<std::size_t>(i)];
    }
}

void ReadKeyboardState()
{
    g_current.fill(false);

    if (!IsGameFocused())
    {
        UpdateEdges();
        return;
    }

    // EFZ owns foreground focus: poll Win32 key state, not SDL_GetKeyboardState.

    static constexpr SDL_Scancode kModHotkeys[] = {
        SDL_SCANCODE_F1,
        SDL_SCANCODE_PAGEUP,
        SDL_SCANCODE_PAGEDOWN,
        SDL_SCANCODE_HOME,
        SDL_SCANCODE_SPACE,
        SDL_SCANCODE_P,
        SDL_SCANCODE_T,
        SDL_SCANCODE_U,
        SDL_SCANCODE_J,
        SDL_SCANCODE_COMMA,
        SDL_SCANCODE_PERIOD,
        SDL_SCANCODE_L,
        SDL_SCANCODE_O,
        SDL_SCANCODE_I,
        SDL_SCANCODE_F5,
        SDL_SCANCODE_F6,
        SDL_SCANCODE_F7,
        SDL_SCANCODE_F9,
    };
    for (const SDL_Scancode scancode : kModHotkeys)
    {
        TrackScancode(scancode);
    }
    for (const trialnet::shortcuts::Descriptor& descriptor : trialnet::shortcuts::kDescriptors)
    {
        TrackScancode(controls::ShortcutKey(descriptor.action));
    }

    for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
    {
        if (controls::SourceForPlayer(playerIndex) != trialnet::input::Source::Keyboard)
        {
            continue;
        }
        for (int slot = 0; slot < kFightSlotCount; ++slot)
        {
            TrackScancode(SlotScancode(playerIndex, slot));
        }
    }

    UpdateEdges();
}

bool AxisPastDeadzone(const Sint16 value)
{
    return std::abs(static_cast<int>(value)) >= kStickDeadzone;
}

void CloseControllerHandleLocked(ControllerHandle* const handle)
{
    if (handle == nullptr)
    {
        return;
    }

    if (handle->ownsJoystick && handle->joystick != nullptr)
    {
        SDL_CloseJoystick(handle->joystick);
    }
    handle->joystick = nullptr;
    handle->ownsJoystick = false;

    if (handle->gamepad != nullptr)
    {
        SDL_CloseGamepad(handle->gamepad);
        handle->gamepad = nullptr;
    }
    handle->id = 0;
    handle->name[0] = '\0';
}

void CloseAllControllersLocked()
{
    for (ControllerHandle& handle : g_playerControllers)
    {
        CloseControllerHandleLocked(&handle);
    }
}

bool OpenControllerHandleLocked(
    const SDL_JoystickID id,
    const int playerIndex,
    ControllerHandle* const out)
{
    if (id == 0 || out == nullptr)
    {
        return false;
    }

    ControllerHandle opened{};
    opened.id = id;
    if (SDL_IsGamepad(id))
    {
        opened.gamepad = SDL_OpenGamepad(id);
        if (opened.gamepad != nullptr)
        {
            opened.joystick = SDL_GetGamepadJoystick(opened.gamepad);
        }
        if (opened.joystick == nullptr && opened.gamepad != nullptr)
        {
            SDL_CloseGamepad(opened.gamepad);
            opened.gamepad = nullptr;
        }
    }

    bool requiresMappedInterface = false;
    if (playerIndex >= 0 && playerIndex <= 1)
    {
        for (int slot = 0; slot < kFightSlotCount; ++slot)
        {
            const controls::PadSourceKind kind = controls::PadForPlayer(playerIndex, slot).kind;
            requiresMappedInterface = requiresMappedInterface
                || kind == controls::PadSourceKind::GamepadButton
                || kind == controls::PadSourceKind::GamepadAxis;
        }
    }

    // Raw fallback only for explicit JOY_* bindings.
    if (opened.joystick == nullptr && (!SDL_IsGamepad(id) || !requiresMappedInterface))
    {
        opened.joystick = SDL_OpenJoystick(id);
        opened.ownsJoystick = opened.joystick != nullptr;
    }
    if (opened.joystick == nullptr)
    {
        if (SDL_IsGamepad(id) && requiresMappedInterface)
        {
            log::Warn(
                "input",
                "P%d mapped controller id=%u could not open through SDL gamepad API; retrying",
                playerIndex + 1,
                static_cast<unsigned>(id));
        }
        return false;
    }

    const char* const name = SDL_GetJoystickName(opened.joystick);
    if (name != nullptr)
    {
        std::snprintf(opened.name, sizeof(opened.name), "%s", name);
    }
    *out = opened;
    return true;
}

bool ControllerAssignmentsCompleteLocked()
{
    for (int player = 0; player < 2; ++player)
    {
        if (g_controllerSources[static_cast<std::size_t>(player)]
            && g_playerControllers[static_cast<std::size_t>(player)].joystick == nullptr)
        {
            return false;
        }
    }
    return true;
}

void RefreshControllerAssignments(const bool forceRebuild = false)
{
    if (!g_gamepadEnabled.load())
    {
        return;
    }

    const std::array<bool, 2> wanted = {
        controls::SourceForPlayer(0) == trialnet::input::Source::Controller,
        controls::SourceForPlayer(1) == trialnet::input::Source::Controller,
    };
    const Uint64 now = SDL_GetTicks();

    std::lock_guard<std::mutex> lock(g_controllerMutex);
    const bool sourcesChanged = wanted != g_controllerSources;
    if (sourcesChanged)
    {
        CloseAllControllersLocked();
        g_controllerSources = wanted;
        lifecycle::Input(
            "controller-route",
            "configured P1=%s P2=%s mode=%s",
            wanted[0] ? "Controller" : "Keyboard",
            wanted[1] ? "Controller" : "Keyboard",
            caster::IsNetplayActive() ? "online-local-P1-bank" : "offline-player-banks");
    }
    else if (forceRebuild)
    {
        // Do not rebuild both handles on one disconnect: assignments can swap.
        for (ControllerHandle& handle : g_playerControllers)
        {
            if (handle.joystick != nullptr && !SDL_JoystickConnected(handle.joystick))
            {
                CloseControllerHandleLocked(&handle);
            }
        }
    }

    if (!wanted[0] && !wanted[1])
    {
        g_nextControllerScanMs.store(0);
        return;
    }
    if (ControllerAssignmentsCompleteLocked())
    {
        g_nextControllerScanMs.store(0);
        return;
    }
    if (!forceRebuild && !sourcesChanged && now < g_nextControllerScanMs.load())
    {
        return;
    }
    g_nextControllerScanMs.store(now + 1000u);

    int count = 0;
    SDL_JoystickID* const ids = SDL_GetJoysticks(&count);
    if (ids == nullptr || count <= 0)
    {
        if (ids != nullptr)
        {
            SDL_free(ids);
        }
        return;
    }

    std::vector<trialnet::input::ControllerDeviceCandidate> candidates;
    candidates.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        candidates.push_back({
            static_cast<trialnet::input::ControllerDeviceId>(ids[i]),
            SDL_IsGamepad(ids[i]),
        });
    }
    const std::vector<trialnet::input::ControllerDeviceId> ordered =
        trialnet::input::PreferredControllerDeviceOrder(candidates);

    std::vector<int> targetPlayers =
        trialnet::input::ControllerPlayerOrder(wanted[0], wanted[1]);
    for (auto it = targetPlayers.begin(); it != targetPlayers.end();)
    {
        if (g_playerControllers[static_cast<std::size_t>(*it)].joystick != nullptr)
        {
            it = targetPlayers.erase(it);
        }
        else
        {
            ++it;
        }
    }

    std::size_t targetIndex = 0u;
    for (const trialnet::input::ControllerDeviceId candidate : ordered)
    {
        if (targetIndex >= targetPlayers.size())
        {
            break;
        }

        bool alreadyAssigned = false;
        for (const ControllerHandle& existing : g_playerControllers)
        {
            if (existing.id == static_cast<SDL_JoystickID>(candidate)
                && existing.joystick != nullptr)
            {
                alreadyAssigned = true;
                break;
            }
        }
        if (alreadyAssigned)
        {
            continue;
        }

        const int player = targetPlayers[targetIndex];
        ControllerHandle opened{};
        if (!OpenControllerHandleLocked(
                static_cast<SDL_JoystickID>(candidate), player, &opened))
        {
            continue;
        }

        ControllerHandle& destination = g_playerControllers[static_cast<std::size_t>(player)];
        destination = opened;
        ++targetIndex;
        log::Info(
            "input",
            "P%d controller opened: id=%u name=%s mapped=%d axes=%d hats=%d buttons=%d",
            player + 1,
            static_cast<unsigned>(destination.id),
            destination.name[0] != '\0' ? destination.name : "(unnamed)",
            destination.gamepad != nullptr ? 1 : 0,
            SDL_GetNumJoystickAxes(destination.joystick),
            SDL_GetNumJoystickHats(destination.joystick),
            SDL_GetNumJoystickButtons(destination.joystick));
        lifecycle::Input(
            "controller-route",
            "P%d attached id=%u name=%s interface=%s",
            player + 1,
            static_cast<unsigned>(destination.id),
            destination.name[0] != '\0' ? destination.name : "(unnamed)",
            destination.gamepad != nullptr ? "SDL Gamepad" : "SDL Joystick");
    }

    SDL_free(ids);
    if (ControllerAssignmentsCompleteLocked())
    {
        g_nextControllerScanMs.store(0);
    }
}

void CloseController()
{
    std::lock_guard<std::mutex> lock(g_controllerMutex);
    CloseAllControllersLocked();
    g_controllerSources = {};
    g_nextControllerScanMs.store(SDL_GetTicks() + 250u);
    g_lastGameThreadControllerPumpMs.store(~Uint64{0});
}

bool AnyControllerDisconnected()
{
    std::lock_guard<std::mutex> lock(g_controllerMutex);
    for (const ControllerHandle& handle : g_playerControllers)
    {
        if (handle.joystick != nullptr && !SDL_JoystickConnected(handle.joystick))
        {
            return true;
        }
    }
    return false;
}

bool EvalPadBinding(const int playerIndex, const controls::PadBinding& binding)
{
    if (!binding.active || playerIndex < 0 || playerIndex > 1)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_controllerMutex);
    const ControllerHandle& controller =
        g_playerControllers[static_cast<std::size_t>(playerIndex)];

    if (binding.kind == controls::PadSourceKind::GamepadAxis && controller.gamepad != nullptr)
    {
        const Sint16 axis = SDL_GetGamepadAxis(
            controller.gamepad,
            binding.gamepadAxis);
        if (!AxisPastDeadzone(axis))
        {
            return false;
        }

        return binding.direction < 0 ? axis < 0 : axis > 0;
    }
    if (binding.kind == controls::PadSourceKind::GamepadButton && controller.gamepad != nullptr)
    {
        return SDL_GetGamepadButton(controller.gamepad, binding.button);
    }
    if (controller.joystick == nullptr || binding.index < 0)
    {
        return false;
    }
    if (binding.kind == controls::PadSourceKind::JoystickButton)
    {
        return binding.index < SDL_GetNumJoystickButtons(controller.joystick)
            && SDL_GetJoystickButton(controller.joystick, binding.index);
    }
    if (binding.kind == controls::PadSourceKind::JoystickAxis)
    {
        if (binding.index >= SDL_GetNumJoystickAxes(controller.joystick)) return false;
        const Sint16 axis = SDL_GetJoystickAxis(controller.joystick, binding.index);
        return AxisPastDeadzone(axis) && (binding.direction < 0 ? axis < 0 : axis > 0);
    }
    if (binding.kind == controls::PadSourceKind::JoystickHat)
    {
        return binding.index < SDL_GetNumJoystickHats(controller.joystick)
            && (SDL_GetJoystickHat(controller.joystick, binding.index) & binding.direction) != 0;
    }
    return false;
}

bool PadSlotDownForPlayerImpl(const int playerIndex, const int slot)
{
    if (controls::SourceForPlayer(playerIndex) != trialnet::input::Source::Controller
        || !IsGameFocused() || slot < 0 || slot >= kFightSlotCount
        || !trialnet::input::ControllerPlayerMaySample(
            playerIndex,
            caster::IsNetplayActive(),
            caster::LocalBindingPlayerIndex()))
    {
        return false;
    }

    // Online always uses the P1 binding bank; do not compare this index to the side.
    return EvalPadBinding(playerIndex, controls::PadForPlayer(playerIndex, slot));
}

bool IsKeyboardSlotDown(const int slot, const int playerIndex)
{
    if (slot < 0 || slot >= kFightSlotCount || playerIndex < 0 || playerIndex > 1)
    {
        return false;
    }

    if (controls::SourceForPlayer(playerIndex) != trialnet::input::Source::Keyboard
        || !IsGameFocused())
    {
        return false;
    }

    const SDL_Scancode scancode = SlotScancode(playerIndex, slot);
    return scancode != SDL_SCANCODE_UNKNOWN && ReadWin32Down(scancode);
}

std::uint8_t* DeviceTypesForPlayer(const int playerIndex)
{
    const std::uintptr_t va =
        (playerIndex == 1) ? trial::kVaDiDeviceTypesP2 : trial::kVaDiDeviceTypesP1;
    return static_cast<std::uint8_t*>(trial::ResolveVa(va));
}

std::uint8_t* KeyCodesForPlayer(const int playerIndex)
{
    const std::uintptr_t va = (playerIndex == 1) ? trial::kVaDiKeyCodesP2 : trial::kVaDiKeyCodesP1;
    return static_cast<std::uint8_t*>(trial::ResolveVa(va));
}

std::uint8_t DikFromScancode(const SDL_Scancode scancode)
{
    const int vk = VkFromScancode(scancode);
    if (vk == 0)
    {
        return 0u;
    }
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC_EX);
    if (scan == 0u)
    {
        return 0u;
    }
    std::uint8_t dik = static_cast<std::uint8_t>(scan & 0xFFu);
    if ((scan & 0xFF00u) == 0xE000u)
    {
        dik = static_cast<std::uint8_t>(dik | 0x80u);
    }
    return dik;
}

void CanonicalizeOfflinePlayerDiBindings(const int playerIndex)
{
    if (playerIndex < 0 || playerIndex > 1)
    {
        return;
    }
    std::uint8_t* const deviceTypes = DeviceTypesForPlayer(playerIndex);
    std::uint8_t* const keyCodes = KeyCodesForPlayer(playerIndex);
    if (deviceTypes == nullptr || keyCodes == nullptr)
    {
        return;
    }

    std::array<std::uint8_t, trialnet::input::kOfflineFightSlotCount> keyboardDiks{};
    std::array<bool, trialnet::input::kOfflineFightSlotCount> controllerBindingsActive{};
    for (int slot = 0; slot < kFightSlotCount; ++slot)
    {
        keyboardDiks[static_cast<std::size_t>(slot)] =
            DikFromScancode(controls::KeyForPlayer(playerIndex, slot));
        controllerBindingsActive[static_cast<std::size_t>(slot)] =
            controls::PadForPlayer(playerIndex, slot).active;
    }
    const trialnet::input::Source source = controls::SourceForPlayer(playerIndex);
    const trialnet::input::OfflineDiBindingPlan plan =
        trialnet::input::BuildOfflineDiBindingPlan(
            playerIndex,
            source,
            keyboardDiks,
            controllerBindingsActive);
    std::memcpy(deviceTypes, plan.deviceTypes.data(), plan.deviceTypes.size());
    std::memcpy(keyCodes, plan.keyCodes.data(), plan.keyCodes.size());

    static bool logged[2] = {};
    if (!logged[playerIndex])
    {
        logged[playerIndex] = true;
        log::Info(
            "input",
            "P%d offline source canonicalized to %s-only DirectInput slots",
            playerIndex + 1,
            source == trialnet::input::Source::Controller ? "controller" : "keyboard");
    }
}

}

// No unwinding objects in scope, so __try is legal.
bool TryBringUpGamepad()
{
    __try
    {
        constexpr SDL_InitFlags kControllerFlags = SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK;
        const SDL_InitFlags initialized = SDL_WasInit(kControllerFlags);
        const SDL_InitFlags missing = kControllerFlags & ~initialized;
        if (missing != 0u)
        {
            SDL_InitSubSystem(missing);
        }
        const bool ok = SDL_WasInit(SDL_INIT_JOYSTICK) != 0u;
        g_gamepadEnabled.store(ok);
        if (ok)
        {
            RefreshControllerAssignments();
        }
        return ok;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool Initialize()
{
    if (g_initialized.exchange(true))
    {
        return true;
    }

    // Never parse SDL key names from a global constructor.
    controls::InitializeDefaults();

    // SDL gamepad init can crash or hang headless harness runs.
    if (harness::ScriptedInputEnabled())
    {
        g_gamepadEnabled.store(false);
        log::Info("input", "keyboard=Win32 gamepad=off (harness scripted input)");
        return true;
    }

    const bool gamepadOk = TryBringUpGamepad();
    g_gamepadEnabled.store(gamepadOk);
    if (!gamepadOk)
    {
        log::Warn("input", "SDL gamepad init unavailable/faulted — keyboard-only");
    }

    log::Info(
        "input",
        "P1=arrows+ZXCV P2=numpad5123+m,./ keyboard=Win32 gamepad=%s",
        g_gamepadEnabled.load() ? "SDL" : "off");
    return true;
}

void Shutdown()
{
    if (!g_initialized.exchange(false))
    {
        return;
    }

    CloseController();
    g_gamepadEnabled.store(false);
    g_current = {};
    g_previous = {};
    g_pressed = {};
    g_mergedSystemState.store(0u);
    g_spectatorPendingSystemEdges.store(0u);
    g_escOutputLatched = false;
    g_f4OutputLatched = false;
    g_f5OutputLatched = false;
    g_f6OutputLatched = false;
    g_f7OutputLatched = false;
    g_f8OutputLatched = false;
    g_incFsfOutputLatched = false;
    g_decFsfOutputLatched = false;
    g_lastGameThreadControllerPumpMs.store(~Uint64{0});
}

void Poll()
{
    if (!g_initialized.load())
    {
        return;
    }

    RefreshControllerAssignments(AnyControllerDisconnected());

    ReadKeyboardState();
}

void PumpControllerEventsOnGameThread()
{
    if (!g_initialized.load() || !g_gamepadEnabled.load())
    {
        return;
    }
    if (controls::SourceForPlayer(0) != trialnet::input::Source::Controller
        && controls::SourceForPlayer(1) != trialnet::input::Source::Controller)
    {
        return;
    }

    // Pump at most once per ms so both players see one coherent sample.
    const Uint64 now = SDL_GetTicks();
    Uint64 previous = g_lastGameThreadControllerPumpMs.load(std::memory_order_relaxed);
    if (previous == now
        || !g_lastGameThreadControllerPumpMs.compare_exchange_strong(
            previous,
            now,
            std::memory_order_acq_rel,
            std::memory_order_relaxed))
    {
        return;
    }

    // No PollEvent: it would steal events from other in-process SDL mods.
    SDL_PumpEvents();
    RefreshControllerAssignments(AnyControllerDisconnected());
}

void SyncKeyboardFromSdl()
{
}

bool IsDown(SDL_Scancode scancode)
{
    if (scancode < 0 || scancode >= SDL_SCANCODE_COUNT)
    {
        return false;
    }
    return g_current[static_cast<std::size_t>(scancode)];
}

bool WasPressed(SDL_Scancode scancode)
{
    if (scancode < 0 || scancode >= SDL_SCANCODE_COUNT)
    {
        return false;
    }
    return g_pressed[static_cast<std::size_t>(scancode)];
}

bool IsDownNow(const SDL_Scancode scancode)
{
    return scancode >= 0 && scancode < SDL_SCANCODE_COUNT
        && IsGameFocused() && ReadWin32Down(scancode);
}

bool IsSlotDownForPlayer(const int slot, const int playerIndex)
{
    return IsKeyboardSlotDown(slot, playerIndex);
}

bool IsGamepadSlotDown(const int slot)
{
    return PadSlotDownForPlayerImpl(caster::LocalBindingPlayerIndex(), slot);
}

bool IsGamepadSlotDownForPlayer(const int playerIndex, const int slot)
{
    return PadSlotDownForPlayerImpl(playerIndex, slot);
}

bool IsConfiguredSlotDownForPlayer(const int playerIndex, const int slot)
{
    if (playerIndex < 0 || playerIndex > 1 || slot < 0 || slot >= kFightSlotCount)
    {
        return false;
    }
    return trialnet::input::SampleConfiguredFightSource(
        controls::SourceForPlayer(playerIndex),
        [=]() { return IsKeyboardSlotDown(slot, playerIndex); },
        [=]() { return PadSlotDownForPlayerImpl(playerIndex, slot); });
}

void SetMergedSystemState(const std::uint8_t mergedState)
{
    const std::uint8_t previous = g_mergedSystemState.exchange(mergedState);
    if (caster::IsSpectatorActive())
    {
        constexpr std::uint8_t edgeMask = menu::kSystemStateEsc
            | menu::kSystemStateIncFsf | menu::kSystemStateDecFsf;
        const std::uint8_t rising = static_cast<std::uint8_t>(
            mergedState & static_cast<std::uint8_t>(~previous) & edgeMask);
        g_spectatorPendingSystemEdges.fetch_or(rising);
    }
    else
    {
        g_spectatorPendingSystemEdges.store(0u);
    }
}

void ApplySystemStateToDiBuffer(std::uint8_t* out256)
{
    if (out256 == nullptr)
    {
        return;
    }

    constexpr std::uint8_t kDikEscape = 0x01u;
    constexpr std::uint8_t kDikF4 = 0x3Eu;
    constexpr std::uint8_t kDikF5 = 0x3Fu;
    constexpr std::uint8_t kDikF6 = 0x40u;
    constexpr std::uint8_t kDikF7 = 0x41u;
    constexpr std::uint8_t kDikF8 = 0x42u;
    constexpr std::uint8_t kDikSubtract = 0x4Au;
    constexpr std::uint8_t kDikAdd = 0x4Eu;

    const std::uint8_t merged = menu::FilterOnlineSystemState(static_cast<std::uint8_t>(
        g_mergedSystemState.load() | g_spectatorPendingSystemEdges.load()));
    if ((merged & menu::kSystemStateEsc) != 0u)
    {
        if (!g_escOutputLatched)
        {
            out256[kDikEscape] = kDiKeyDown;
            g_escOutputLatched = true;
            lifecycle::Input(
                "system",
                "emit ESC edge merged=0x%02X",
                static_cast<unsigned>(merged));
            rollback::AcknowledgeSystemStateApplied(menu::kSystemStateEsc);
            g_spectatorPendingSystemEdges.fetch_and(
                static_cast<std::uint8_t>(~menu::kSystemStateEsc));
        }
    }
    else
    {
        if (g_escOutputLatched)
        {
            lifecycle::Input("system", "release ESC edge latch");
        }
        g_escOutputLatched = false;
    }

    // Recreate the F4/F5 level on every DirectInput poll of this sim frame.
    const bool f4Down = (merged & menu::kSystemStateF4) != 0u;
    const bool f5Down = (merged & menu::kSystemStateF5) != 0u;
    const bool f6Down = (merged & menu::kSystemStateF6) != 0u;
    const bool f7Down = (merged & menu::kSystemStateF7) != 0u;
    const bool f8Down = (merged & menu::kSystemStateF8) != 0u;
    if (f4Down)
    {
        out256[kDikF4] = kDiKeyDown;
    }
    if (f5Down)
    {
        out256[kDikF5] = kDiKeyDown;
    }
    if (f6Down)
    {
        out256[kDikF6] = kDiKeyDown;
    }
    if (f7Down)
    {
        out256[kDikF7] = kDiKeyDown;
    }
    if (f8Down)
    {
        out256[kDikF8] = kDiKeyDown;
    }
    const bool incFsfDown = (merged & menu::kSystemStateIncFsf) != 0u;
    const bool decFsfDown = (merged & menu::kSystemStateDecFsf) != 0u;
    if (incFsfDown)
    {
        out256[kDikAdd] = kDiKeyDown;
        g_spectatorPendingSystemEdges.fetch_and(
            static_cast<std::uint8_t>(~menu::kSystemStateIncFsf));
    }
    if (decFsfDown)
    {
        out256[kDikSubtract] = kDiKeyDown;
        g_spectatorPendingSystemEdges.fetch_and(
            static_cast<std::uint8_t>(~menu::kSystemStateDecFsf));
    }
    if (f4Down != g_f4OutputLatched)
    {
        lifecycle::Input("system", "%s F4 level merged=0x%02X", f4Down ? "emit" : "release", static_cast<unsigned>(merged));
        g_f4OutputLatched = f4Down;
    }
    if (f5Down != g_f5OutputLatched)
    {
        lifecycle::Input("system", "%s F5 level merged=0x%02X", f5Down ? "emit" : "release", static_cast<unsigned>(merged));
        g_f5OutputLatched = f5Down;
    }
    if (f6Down != g_f6OutputLatched)
    {
        lifecycle::Input("system", "%s F6 level merged=0x%02X", f6Down ? "emit" : "release", static_cast<unsigned>(merged));
        g_f6OutputLatched = f6Down;
    }
    if (f7Down != g_f7OutputLatched)
    {
        lifecycle::Input("system", "%s F7 level merged=0x%02X", f7Down ? "emit" : "release", static_cast<unsigned>(merged));
        g_f7OutputLatched = f7Down;
    }
    if (f8Down != g_f8OutputLatched)
    {
        lifecycle::Input("system", "%s F8 level merged=0x%02X", f8Down ? "emit" : "release", static_cast<unsigned>(merged));
        g_f8OutputLatched = f8Down;
    }
    if (incFsfDown != g_incFsfOutputLatched)
    {
        lifecycle::Input(
            "system",
            "%s Numpad + edge merged=0x%02X",
            incFsfDown ? "emit" : "release",
            static_cast<unsigned>(merged));
        g_incFsfOutputLatched = incFsfDown;
    }
    if (decFsfDown != g_decFsfOutputLatched)
    {
        lifecycle::Input(
            "system",
            "%s Numpad - edge merged=0x%02X",
            decFsfDown ? "emit" : "release",
            static_cast<unsigned>(merged));
        g_decFsfOutputLatched = decFsfDown;
    }
}

void ApplyOfflinePracticeBindingsToDiBuffer(std::uint8_t* const out256)
{
    if (out256 == nullptr || caster::IsNetplayActive() || caster::IsSpectatorActive())
    {
        return;
    }

    constexpr std::uint8_t kDikF4 = 0x3Eu;
    constexpr std::uint8_t kDikF5 = 0x3Fu;
    out256[kDikF4] = 0u;
    out256[kDikF5] = 0u;

    const auto inject = [out256](
                            const trialnet::shortcuts::Action action,
                            const std::uint8_t dik) {
        if (!controls::ShortcutConflictsWithAnyFightBinding(action)
            && IsDownNow(controls::ShortcutKey(action)))
        {
            out256[dik] = kDiKeyDown;
        }
    };
    inject(trialnet::shortcuts::Action::TrainingMode, kDikF4);
    inject(trialnet::shortcuts::Action::RestoreResources, kDikF5);
}

void MergePlayerIntoDiBuffer(std::uint8_t* out256, const int playerIndex)
{
    if (out256 == nullptr || playerIndex < 0 || playerIndex > 1)
    {
        return;
    }

    const std::uint8_t* const deviceTypes = DeviceTypesForPlayer(playerIndex);
    const std::uint8_t* const keyCodes = KeyCodesForPlayer(playerIndex);
    if (deviceTypes == nullptr || keyCodes == nullptr)
    {
        return;
    }

    std::array<bool, trialnet::input::kOfflineFightSlotCount> slotDown{};
    for (int slot = 0; slot < kFightSlotCount; ++slot)
    {
        if (deviceTypes[slot] != trialnet::input::kDiKeyboardDevice || keyCodes[slot] == 0u)
        {
            continue;
        }
        slotDown[static_cast<std::size_t>(slot)] =
            IsConfiguredSlotDownForPlayer(playerIndex, slot);
    }
    trialnet::input::MergeManagedDiSlots(
        out256,
        deviceTypes,
        keyCodes,
        slotDown.data(),
        slotDown.size());
}

void MergeWireMaskIntoDiBuffer(std::uint8_t* out256, const int playerIndex, const std::uint8_t wireMask)
{
    if (out256 == nullptr || playerIndex < 0 || playerIndex > 1 || wireMask == 0u)
    {
        return;
    }

    const std::uint8_t* const keyCodes = KeyCodesForPlayer(playerIndex);
    if (keyCodes == nullptr)
    {
        return;
    }

    std::uint32_t slots[codec::kSlotCount] = {};
    codec::UnpackWireMask(wireMask, slots);

    for (int slot = 0; slot < kFightSlotCount; ++slot)
    {
        if (slots[slot] == 0u)
        {
            continue;
        }

        const std::uint8_t dik = keyCodes[slot];
        if (dik < 256u)
        {
            out256[dik] = kDiKeyDown;
        }
    }
}

void OverlayDiKeyboardBuffer(std::uint8_t* out256, const int playerIndex)
{
    if (out256 == nullptr || playerIndex < 0 || playerIndex > 1)
    {
        return;
    }

    MergePlayerIntoDiBuffer(out256, playerIndex);
    ApplySystemStateToDiBuffer(out256);
}

void ClearPlayerFromDiBuffer(std::uint8_t* out256, const int playerIndex)
{
    if (out256 == nullptr || playerIndex < 0 || playerIndex > 1)
    {
        return;
    }
    const std::uint8_t* const deviceTypes = DeviceTypesForPlayer(playerIndex);
    const std::uint8_t* const keyCodes = KeyCodesForPlayer(playerIndex);
    if (deviceTypes == nullptr || keyCodes == nullptr)
    {
        return;
    }
    trialnet::input::ClearManagedDiSlots(
        out256,
        deviceTypes,
        keyCodes,
        trialnet::input::kOfflineFightSlotCount);
}

void ReplacePlayerInDiBuffer(
    std::uint8_t* out256,
    const int playerIndex,
    const std::uint8_t selectedWireMask)
{
    CanonicalizeOfflinePlayerDiBindings(playerIndex);
    ClearPlayerFromDiBuffer(out256, playerIndex);
    const std::uint8_t* const deviceTypes = DeviceTypesForPlayer(playerIndex);
    const std::uint8_t* const keyCodes = KeyCodesForPlayer(playerIndex);
    trialnet::input::MergeManagedDiMask(
        out256,
        deviceTypes,
        keyCodes,
        selectedWireMask,
        trialnet::input::kOfflineFightSlotCount);
}

void ReplaceBothPlayersInDiBuffer(
    std::uint8_t* out256,
    const std::uint8_t p1SelectedWireMask,
    const std::uint8_t p2SelectedWireMask)
{
    CanonicalizeOfflinePlayerDiBindings(0);
    CanonicalizeOfflinePlayerDiBindings(1);
    ClearPlayerFromDiBuffer(out256, 0);
    ClearPlayerFromDiBuffer(out256, 1);
    trialnet::input::MergeManagedDiMask(
        out256,
        DeviceTypesForPlayer(0),
        KeyCodesForPlayer(0),
        p1SelectedWireMask,
        trialnet::input::kOfflineFightSlotCount);
    trialnet::input::MergeManagedDiMask(
        out256,
        DeviceTypesForPlayer(1),
        KeyCodesForPlayer(1),
        p2SelectedWireMask,
        trialnet::input::kOfflineFightSlotCount);
}

void OverlayDiKeyboardBufferBothPlayers(std::uint8_t* out256)
{
    if (out256 == nullptr)
    {
        return;
    }

    MergePlayerIntoDiBuffer(out256, 0);
    MergePlayerIntoDiBuffer(out256, 1);
    ApplySystemStateToDiBuffer(out256);
}

void MergeLocalNetplayBindingsIntoDiBuffer(std::uint8_t* out256)
{
    if (out256 == nullptr)
    {
        return;
    }

    // Client flips playerIndex so its P2 poll reads the P1 table.
    constexpr int kLocalDiPlayer = 0;
    const int sampleIndex = caster::LocalBindingPlayerIndex();
    const std::uint8_t* const deviceTypes = DeviceTypesForPlayer(kLocalDiPlayer);
    const std::uint8_t* const keyCodes = KeyCodesForPlayer(kLocalDiPlayer);
    if (deviceTypes == nullptr || keyCodes == nullptr)
    {
        return;
    }

    for (int slot = 0; slot < kFightSlotCount; ++slot)
    {
        if (deviceTypes[slot] != 0u)
        {
            continue;
        }

        const bool down = IsConfiguredSlotDownForPlayer(sampleIndex, slot);
        if (!down)
        {
            continue;
        }

        const std::uint8_t dik = keyCodes[slot];
        if (dik < 256u)
        {
            out256[dik] = kDiKeyDown;
        }
    }
}

void OverlayDiKeyboardBufferNetplay(
    std::uint8_t* out256,
    const std::uint8_t localWireMask,
    const std::uint8_t remoteWireMask,
    const bool hasRemoteInput)
{
    if (out256 == nullptr)
    {
        return;
    }

    MergeWireMaskIntoDiBuffer(out256, 0, localWireMask);
    if (hasRemoteInput)
    {
        // Remote wire is always in the P2 table.
        MergeWireMaskIntoDiBuffer(out256, 1, remoteWireMask);
    }
    ApplySystemStateToDiBuffer(out256);
}

void FillDiKeyboardBuffer(std::uint8_t* out256, const int playerIndex)
{
    if (out256 == nullptr || playerIndex < 0 || playerIndex > 1)
    {
        return;
    }

    std::memset(out256, 0, 256u);
    OverlayDiKeyboardBuffer(out256, playerIndex);
}

void FillDiKeyboardBufferBothPlayers(std::uint8_t* out256)
{
    if (out256 == nullptr)
    {
        return;
    }

    std::memset(out256, 0, 256u);
    OverlayDiKeyboardBufferBothPlayers(out256);
}

void FillDiKeyboardBufferNetplay(
    std::uint8_t* out256,
    const std::uint8_t localWireMask,
    const std::uint8_t remoteWireMask,
    const bool hasRemoteInput)
{
    if (out256 == nullptr)
    {
        return;
    }

    std::memset(out256, 0, 256u);
    OverlayDiKeyboardBufferNetplay(out256, localWireMask, remoteWireMask, hasRemoteInput);
}

bool HasGamepad()
{
    const int playerIndex = caster::LocalBindingPlayerIndex();
    if (playerIndex < 0 || playerIndex > 1)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_controllerMutex);
    return g_playerControllers[static_cast<std::size_t>(playerIndex)].joystick != nullptr;
}

const char* GamepadName()
{
    thread_local char copy[128] = {};
    const int playerIndex = caster::LocalBindingPlayerIndex();
    if (playerIndex < 0 || playerIndex > 1)
    {
        copy[0] = '\0';
        return copy;
    }
    std::lock_guard<std::mutex> lock(g_controllerMutex);
    std::snprintf(
        copy,
        sizeof(copy),
        "%s",
        g_playerControllers[static_cast<std::size_t>(playerIndex)].name);
    return copy;
}

}
