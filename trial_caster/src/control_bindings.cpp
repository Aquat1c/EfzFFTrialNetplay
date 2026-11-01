#include "control_bindings.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace trial_caster
{
namespace
{

constexpr const wchar_t* kP1Defaults[kFightSlotCount] = {
    L"UP",
    L"DOWN",
    L"LEFT",
    L"RIGHT",
    L"Z",
    L"X",
    L"C",
    L"V",
};

constexpr const wchar_t* kP2Defaults[kFightSlotCount] = {
    L"KP_5",
    L"KP_2",
    L"KP_1",
    L"KP_3",
    L"M",
    L"COMMA",
    L"PERIOD",
    L"SLASH",
};

constexpr const wchar_t* kP1PadDefaults[kFightSlotCount] = {
    L"DPAD_UP",
    L"DPAD_DOWN",
    L"DPAD_LEFT",
    L"DPAD_RIGHT",
    L"SOUTH",
    L"EAST",
    L"WEST",
    L"NORTH",
};

constexpr const wchar_t* kP2PadDefaults[kFightSlotCount] = {
    L"",
    L"",
    L"",
    L"",
    L"",
    L"",
    L"",
    L"",
};

constexpr const wchar_t* kSlotKeys[kFightSlotCount] = {
    L"up",
    L"down",
    L"left",
    L"right",
    L"a",
    L"b",
    L"c",
    L"d",
};

bool WriteSection(
    const std::wstring& file,
    const wchar_t* section,
    const PlayerBindings& bindings)
{
    bool ok = WritePrivateProfileStringW(
        section,
        L"source",
        trialnet::input::SourceIniName(bindings.source),
        file.c_str()) != FALSE;

    for (int i = 0; i < kFightSlotCount; ++i)
    {
        const auto idx = static_cast<std::size_t>(i);
        ok = (WritePrivateProfileStringW(
            section,
            kSlotKeys[i],
            bindings.keys[idx].c_str(),
            file.c_str()) != FALSE) && ok;

        const std::wstring padKey = std::wstring(kSlotKeys[i]) + L"_pad";
        ok = (WritePrivateProfileStringW(
            section,
            padKey.c_str(),
            bindings.pad[idx].c_str(),
            file.c_str()) != FALSE) && ok;
    }
    return ok;
}

bool WriteShortcutSection(
    const std::wstring& file,
    const trialnet::shortcuts::Bindings& bindings)
{
    bool ok = true;
    for (const trialnet::shortcuts::Descriptor& descriptor : trialnet::shortcuts::kDescriptors)
    {
        const std::wstring value = trialnet::shortcuts::NormalizeBindingToken(
            bindings[descriptor.action]);
        ok = (WritePrivateProfileStringW(
            L"shortcuts",
            descriptor.iniKey,
            value.c_str(),
            file.c_str()) != FALSE) && ok;
    }
    return ok;
}

void LoadShortcutsFromCasterIni(
    const std::wstring& iniFile,
    trialnet::shortcuts::Bindings* const out)
{
    if (out == nullptr)
    {
        return;
    }

    for (const trialnet::shortcuts::Descriptor& descriptor : trialnet::shortcuts::kDescriptors)
    {
        wchar_t value[64] = {};
        GetPrivateProfileStringW(
            L"shortcuts",
            descriptor.iniKey,
            descriptor.defaultBinding,
            value,
            static_cast<DWORD>(std::size(value)),
            iniFile.c_str());
        (*out)[descriptor.action] = value[0] != L'\0'
            ? trialnet::shortcuts::NormalizeBindingToken(value)
            : std::wstring(descriptor.defaultBinding);
    }
}

void LoadPlayerFromCasterIni(
    const std::wstring& iniFile,
    const wchar_t* keyPrefix,
    const wchar_t* const keyDefaults[kFightSlotCount],
    const wchar_t* const padDefaults[kFightSlotCount],
    PlayerBindings* out)
{
    wchar_t source[32] = {};
    const std::wstring sourceKey = std::wstring(keyPrefix) + L"source";
    GetPrivateProfileStringW(
        L"controls",
        sourceKey.c_str(),
        L"keyboard",
        source,
        static_cast<DWORD>(std::size(source)),
        iniFile.c_str());
    out->source = trialnet::input::ParseSource(source);

    for (int i = 0; i < kFightSlotCount; ++i)
    {
        const auto idx = static_cast<std::size_t>(i);
        wchar_t buf[64] = {};
        const std::wstring keyName = std::wstring(keyPrefix) + kSlotKeys[i];
        GetPrivateProfileStringW(
            L"controls",
            keyName.c_str(),
            keyDefaults[i],
            buf,
            static_cast<DWORD>(std::size(buf)),
            iniFile.c_str());
        out->keys[idx] = buf[0] != L'\0' ? buf : keyDefaults[i];

        const std::wstring padName = keyName + L"_pad";
        GetPrivateProfileStringW(
            L"controls",
            padName.c_str(),
            padDefaults[i],
            buf,
            static_cast<DWORD>(std::size(buf)),
            iniFile.c_str());
        // NONE is an intentional clear; an empty pad value falls back to its default.
        out->pad[idx] = buf[0] != L'\0' || padDefaults[i][0] == L'\0' ? buf : padDefaults[i];
    }
}

constexpr int kStickDeadzone = 8000;

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }

    std::string utf8(text.size() * 4u, '\0');
    const int len = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.c_str(),
        -1,
        utf8.data(),
        static_cast<int>(utf8.size()),
        nullptr,
        nullptr);
    if (len <= 1)
    {
        return {};
    }
    utf8.resize(static_cast<std::size_t>(len - 1));
    return utf8;
}

void ToUpperAscii(char* text)
{
    if (text == nullptr)
    {
        return;
    }

    for (char* cursor = text; *cursor != '\0'; ++cursor)
    {
        if (*cursor >= 'a' && *cursor <= 'z')
        {
            *cursor = static_cast<char>(*cursor - 'a' + 'A');
        }
    }
}

SDL_Scancode ScancodeFromBindingName(const std::wstring& text)
{
    if (text.empty())
    {
        return SDL_SCANCODE_UNKNOWN;
    }

    std::string utf8 = WideToUtf8(text);
    SDL_Scancode scancode = SDL_GetScancodeFromName(utf8.c_str());
    if (scancode != SDL_SCANCODE_UNKNOWN)
    {
        return scancode;
    }

    ToUpperAscii(utf8.data());
    return SDL_GetScancodeFromName(utf8.c_str());
}

bool AxisPastDeadzone(const Sint16 value)
{
    return std::abs(static_cast<int>(value)) >= kStickDeadzone;
}

SDL_GamepadButton SdlButtonFromStandard(const trialnet::input::StandardButton button)
{
    using Button = trialnet::input::StandardButton;
    switch (button)
    {
    case Button::South: return SDL_GAMEPAD_BUTTON_SOUTH;
    case Button::East: return SDL_GAMEPAD_BUTTON_EAST;
    case Button::West: return SDL_GAMEPAD_BUTTON_WEST;
    case Button::North: return SDL_GAMEPAD_BUTTON_NORTH;
    case Button::Back: return SDL_GAMEPAD_BUTTON_BACK;
    case Button::Guide: return SDL_GAMEPAD_BUTTON_GUIDE;
    case Button::Start: return SDL_GAMEPAD_BUTTON_START;
    case Button::LeftStick: return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case Button::RightStick: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case Button::LeftShoulder: return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case Button::RightShoulder: return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case Button::DpadUp: return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case Button::DpadDown: return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case Button::DpadLeft: return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case Button::DpadRight: return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    case Button::Misc1: return SDL_GAMEPAD_BUTTON_MISC1;
    case Button::RightPaddle1: return SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1;
    case Button::LeftPaddle1: return SDL_GAMEPAD_BUTTON_LEFT_PADDLE1;
    case Button::RightPaddle2: return SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2;
    case Button::LeftPaddle2: return SDL_GAMEPAD_BUTTON_LEFT_PADDLE2;
    case Button::Touchpad: return SDL_GAMEPAD_BUTTON_TOUCHPAD;
    case Button::Misc2: return SDL_GAMEPAD_BUTTON_MISC2;
    case Button::Misc3: return SDL_GAMEPAD_BUTTON_MISC3;
    case Button::Misc4: return SDL_GAMEPAD_BUTTON_MISC4;
    case Button::Misc5: return SDL_GAMEPAD_BUTTON_MISC5;
    case Button::Misc6: return SDL_GAMEPAD_BUTTON_MISC6;
    default: return SDL_GAMEPAD_BUTTON_INVALID;
    }
}

bool EvalPadBindingImpl(
    const std::wstring& padName,
    SDL_Gamepad* pad,
    SDL_Joystick* joystick)
{
    if (padName.empty())
    {
        return false;
    }

    std::string name = WideToUtf8(padName);
    ToUpperAscii(name.data());

    if (name == "LEFT_STICK_UP")
    {
        if (pad == nullptr) return false;
        const Sint16 axis = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        return AxisPastDeadzone(axis) && axis < 0;
    }
    if (name == "LEFT_STICK_DOWN")
    {
        if (pad == nullptr) return false;
        const Sint16 axis = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        return AxisPastDeadzone(axis) && axis > 0;
    }
    if (name == "LEFT_STICK_LEFT")
    {
        if (pad == nullptr) return false;
        const Sint16 axis = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        return AxisPastDeadzone(axis) && axis < 0;
    }
    if (name == "LEFT_STICK_RIGHT")
    {
        if (pad == nullptr) return false;
        const Sint16 axis = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        return AxisPastDeadzone(axis) && axis > 0;
    }
    auto gamepadAxis = [pad](const SDL_GamepadAxis axis, const int sign)
    {
        if (pad == nullptr) return false;
        const Sint16 value = SDL_GetGamepadAxis(pad, axis);
        return AxisPastDeadzone(value) && (sign < 0 ? value < 0 : value > 0);
    };
    if (name == "RIGHT_STICK_UP") return gamepadAxis(SDL_GAMEPAD_AXIS_RIGHTY, -1);
    if (name == "RIGHT_STICK_DOWN") return gamepadAxis(SDL_GAMEPAD_AXIS_RIGHTY, 1);
    if (name == "RIGHT_STICK_LEFT") return gamepadAxis(SDL_GAMEPAD_AXIS_RIGHTX, -1);
    if (name == "RIGHT_STICK_RIGHT") return gamepadAxis(SDL_GAMEPAD_AXIS_RIGHTX, 1);
    if (name == "LEFT_TRIGGER") return gamepadAxis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1);
    if (name == "RIGHT_TRIGGER") return gamepadAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1);

    int rawIndex = -1;
    char direction[16] = {};
    if (sscanf_s(name.c_str(), "JOY_BUTTON_%d", &rawIndex) == 1 && rawIndex >= 0)
    {
        return joystick != nullptr && rawIndex < SDL_GetNumJoystickButtons(joystick)
            && SDL_GetJoystickButton(joystick, rawIndex);
    }
    if (sscanf_s(name.c_str(), "JOY_AXIS_%d_%15s", &rawIndex, direction,
            static_cast<unsigned>(std::size(direction))) == 2 && rawIndex >= 0)
    {
        if (joystick == nullptr || rawIndex >= SDL_GetNumJoystickAxes(joystick)) return false;
        const Sint16 value = SDL_GetJoystickAxis(joystick, rawIndex);
        const int sign = std::strcmp(direction, "NEG") == 0 ? -1
            : std::strcmp(direction, "POS") == 0 ? 1 : 0;
        return sign != 0 && AxisPastDeadzone(value) && (sign < 0 ? value < 0 : value > 0);
    }
    if (sscanf_s(name.c_str(), "JOY_HAT_%d_%15s", &rawIndex, direction,
            static_cast<unsigned>(std::size(direction))) == 2 && rawIndex >= 0)
    {
        if (joystick == nullptr || rawIndex >= SDL_GetNumJoystickHats(joystick)) return false;
        int mask = 0;
        if (std::strcmp(direction, "UP") == 0) mask = SDL_HAT_UP;
        else if (std::strcmp(direction, "DOWN") == 0) mask = SDL_HAT_DOWN;
        else if (std::strcmp(direction, "LEFT") == 0) mask = SDL_HAT_LEFT;
        else if (std::strcmp(direction, "RIGHT") == 0) mask = SDL_HAT_RIGHT;
        return mask != 0 && (SDL_GetJoystickHat(joystick, rawIndex) & mask) != 0;
    }

    const SDL_GamepadButton button =
        SdlButtonFromStandard(trialnet::input::ParseStandardButton(padName));
    if (button == SDL_GAMEPAD_BUTTON_INVALID || pad == nullptr)
    {
        return false;
    }

    return SDL_GetGamepadButton(pad, button);
}

bool IsKeyboardSlotDownImpl(const PlayerBindings& bindings, const int slot)
{
    if (slot < 0 || slot >= kFightSlotCount)
    {
        return false;
    }

    const SDL_Scancode scancode = ScancodeFromBindingName(bindings.keys[static_cast<std::size_t>(slot)]);
    if (scancode == SDL_SCANCODE_UNKNOWN)
    {
        return false;
    }

    const bool* const state = SDL_GetKeyboardState(nullptr);
    if (state == nullptr)
    {
        return false;
    }

    return state[scancode];
}

bool IsAssignedToken(const std::wstring& token)
{
    return !token.empty() && token != L"NONE" && token != L"OFF" && token != L"_";
}

bool AddUniqueBinding(
    std::vector<std::pair<std::wstring, std::wstring>>* const assigned,
    const std::wstring& token,
    const std::wstring& owner,
    std::wstring* const error)
{
    if (assigned == nullptr || !IsAssignedToken(token))
    {
        return true;
    }
    for (const auto& [existingToken, existingOwner] : *assigned)
    {
        if (existingToken == token)
        {
            if (error != nullptr)
            {
                *error = owner + L" and " + existingOwner + L" both use " + token + L".";
            }
            return false;
        }
    }
    assigned->emplace_back(token, owner);
    return true;
}

}

ControlBindings DefaultControlBindings()
{
    ControlBindings bindings{};
    bindings.p1.source = trialnet::input::Source::Keyboard;
    bindings.p2.source = trialnet::input::Source::Keyboard;
    for (int i = 0; i < kFightSlotCount; ++i)
    {
        const auto idx = static_cast<std::size_t>(i);
        bindings.p1.keys[idx] = kP1Defaults[i];
        bindings.p2.keys[idx] = kP2Defaults[i];
        bindings.p1.pad[idx] = kP1PadDefaults[i];
        bindings.p2.pad[idx] = L"";
    }
    return bindings;
}

void LoadControlsFromCasterIni(const std::wstring& iniFile, ControlBindings* out)
{
    if (out == nullptr)
    {
        return;
    }

    const ControlBindings defaults = DefaultControlBindings();
    *out = defaults;
    LoadPlayerFromCasterIni(iniFile, L"p1_", kP1Defaults, kP1PadDefaults, &out->p1);
    LoadPlayerFromCasterIni(iniFile, L"p2_", kP2Defaults, kP2PadDefaults, &out->p2);
    LoadShortcutsFromCasterIni(iniFile, &out->shortcuts);
}

bool SaveControlsToCasterIni(const std::wstring& iniFile, const ControlBindings& bindings)
{
    bool ok = WritePrivateProfileStringW(
        L"controls", L"p1_source", trialnet::input::SourceIniName(bindings.p1.source), iniFile.c_str())
        != FALSE;
    ok = (WritePrivateProfileStringW(
        L"controls", L"p2_source", trialnet::input::SourceIniName(bindings.p2.source), iniFile.c_str())
        != FALSE) && ok;

    for (int i = 0; i < kFightSlotCount; ++i)
    {
        const auto idx = static_cast<std::size_t>(i);
        const std::wstring p1Key = std::wstring(L"p1_") + kSlotKeys[i];
        const std::wstring p2Key = std::wstring(L"p2_") + kSlotKeys[i];
        ok = (WritePrivateProfileStringW(
            L"controls",
            p1Key.c_str(),
            bindings.p1.keys[idx].c_str(),
            iniFile.c_str()) != FALSE) && ok;
        ok = (WritePrivateProfileStringW(
            L"controls",
            p2Key.c_str(),
            bindings.p2.keys[idx].c_str(),
            iniFile.c_str()) != FALSE) && ok;
        ok = (WritePrivateProfileStringW(
            L"controls",
            (p1Key + L"_pad").c_str(),
            bindings.p1.pad[idx].c_str(),
            iniFile.c_str()) != FALSE) && ok;
        ok = (WritePrivateProfileStringW(
            L"controls",
            (p2Key + L"_pad").c_str(),
            bindings.p2.pad[idx].c_str(),
            iniFile.c_str()) != FALSE) && ok;
    }
    return WriteShortcutSection(iniFile, bindings.shortcuts) && ok;
}

bool WriteControlsToSessionIni(const std::wstring& iniFile, const ControlBindings& bindings)
{
    const bool p1 = WriteSection(iniFile, L"controls_p1", bindings.p1);
    const bool p2 = WriteSection(iniFile, L"controls_p2", bindings.p2);
    const bool shortcuts = WriteShortcutSection(iniFile, bindings.shortcuts);
    return p1 && p2 && shortcuts;
}

bool ValidateControlBindings(const ControlBindings& bindings, std::wstring* const error)
{
    if (error != nullptr)
    {
        error->clear();
    }

    std::vector<std::pair<std::wstring, std::wstring>> keyboardBindings;
    const PlayerBindings* const players[] = {&bindings.p1, &bindings.p2};
    for (std::size_t playerIndex = 0; playerIndex < std::size(players); ++playerIndex)
    {
        for (int slot = 0; slot < kFightSlotCount; ++slot)
        {
            const std::wstring token = trialnet::shortcuts::NormalizeBindingToken(
                players[playerIndex]->keys[static_cast<std::size_t>(slot)]);
            const std::wstring owner = L"Player " + std::to_wstring(playerIndex + 1u)
                + L" " + kSlotLabels[slot];
            if (!AddUniqueBinding(&keyboardBindings, token, owner, error))
            {
                return false;
            }
        }
    }

    for (const trialnet::shortcuts::Descriptor& descriptor : trialnet::shortcuts::kDescriptors)
    {
        const std::wstring token = trialnet::shortcuts::NormalizeBindingToken(
            bindings.shortcuts[descriptor.action]);
        if (!AddUniqueBinding(&keyboardBindings, token, descriptor.label, error))
        {
            return false;
        }
    }

    for (std::size_t playerIndex = 0; playerIndex < std::size(players); ++playerIndex)
    {
        std::vector<std::pair<std::wstring, std::wstring>> padBindings;
        for (int slot = 0; slot < kFightSlotCount; ++slot)
        {
            const std::wstring token = trialnet::input::CanonicalizePadToken(
                players[playerIndex]->pad[static_cast<std::size_t>(slot)]);
            const std::wstring owner = L"Player " + std::to_wstring(playerIndex + 1u)
                + L" controller " + kSlotLabels[slot];
            if (!AddUniqueBinding(&padBindings, token, owner, error))
            {
                return false;
            }
        }
    }
    return true;
}

int ControllerDeviceIndexForPlayer(const ControlBindings& bindings, const int playerIndex)
{
    if (playerIndex < 0 || playerIndex > 1)
    {
        return -1;
    }
    const bool p1Controller = bindings.p1.source == trialnet::input::Source::Controller;
    const bool p2Controller = bindings.p2.source == trialnet::input::Source::Controller;
    if ((playerIndex == 0 && !p1Controller) || (playerIndex == 1 && !p2Controller))
    {
        return -1;
    }
    return p1Controller && p2Controller ? playerIndex : 0;
}

bool EvalPadBinding(
    const std::wstring& padName,
    SDL_Gamepad* pad,
    SDL_Joystick* joystick)
{
    return EvalPadBindingImpl(padName, pad, joystick);
}

bool IsKeyboardSlotDown(const PlayerBindings& bindings, const int slot)
{
    return IsKeyboardSlotDownImpl(bindings, slot);
}

bool IsSelectedSlotDown(
    const PlayerBindings& bindings,
    const int slot,
    SDL_Gamepad* pad,
    SDL_Joystick* joystick)
{
    if (bindings.source == trialnet::input::Source::Controller)
    {
        return slot >= 0 && slot < kFightSlotCount
            && EvalPadBindingImpl(bindings.pad[static_cast<std::size_t>(slot)], pad, joystick);
    }
    return IsKeyboardSlotDownImpl(bindings, slot);
}

std::uint8_t SampleWireMaskPreview(
    const PlayerBindings& bindings,
    SDL_Gamepad* pad,
    SDL_Joystick* joystick)
{
    std::uint8_t mask = 0u;
    for (int slot = 0; slot < kFightSlotCount; ++slot)
    {
        const bool down = IsSelectedSlotDown(bindings, slot, pad, joystick);
        if (down)
        {
            mask |= static_cast<std::uint8_t>(1u << slot);
        }
    }
    return mask;
}

}
