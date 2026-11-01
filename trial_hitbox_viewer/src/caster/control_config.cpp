#include "control_config.h"

#include "debug_log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gamepad.h>

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

namespace thv::controls
{
namespace
{

// POD only: SDL_GetScancodeFromName is unsafe under the loader lock.
Bindings g_bindings{};

constexpr const wchar_t* kP1Defaults[kFightSlotCount] = {
    L"UP", L"DOWN", L"LEFT", L"RIGHT", L"Z", L"X", L"C", L"V"};
constexpr const wchar_t* kP2Defaults[kFightSlotCount] = {
    L"KP_5", L"KP_2", L"KP_1", L"KP_3", L"M", L"COMMA", L"PERIOD", L"SLASH"};
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
    L"", L"", L"", L"", L"", L"", L"", L""};

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

SDL_Scancode ParseKeyName(const std::wstring& text)
{
    if (text.empty())
    {
        return SDL_SCANCODE_UNKNOWN;
    }

    const std::wstring normalized = trialnet::shortcuts::NormalizeBindingToken(text);
    if (normalized == L"NONE" || normalized == L"OFF" || normalized == L"-")
    {
        return SDL_SCANCODE_UNKNOWN;
    }
    if (normalized == L"KP_PLUS") return SDL_SCANCODE_KP_PLUS;
    if (normalized == L"KP_MINUS") return SDL_SCANCODE_KP_MINUS;
    if (normalized == L"KP_0") return SDL_SCANCODE_KP_0;
    if (normalized == L"LEFTBRACKET") return SDL_SCANCODE_LEFTBRACKET;
    if (normalized == L"RIGHTBRACKET") return SDL_SCANCODE_RIGHTBRACKET;
    if (normalized == L"MINUS") return SDL_SCANCODE_MINUS;
    if (normalized == L"EQUALS") return SDL_SCANCODE_EQUALS;
    if (normalized == L"BACKSLASH") return SDL_SCANCODE_BACKSLASH;
    if (normalized == L"SEMICOLON") return SDL_SCANCODE_SEMICOLON;
    if (normalized == L"APOSTROPHE") return SDL_SCANCODE_APOSTROPHE;
    if (normalized == L"GRAVE") return SDL_SCANCODE_GRAVE;
    if (normalized == L"COMMA") return SDL_SCANCODE_COMMA;
    if (normalized == L"PERIOD") return SDL_SCANCODE_PERIOD;
    if (normalized == L"SLASH") return SDL_SCANCODE_SLASH;
    if (normalized == L"PAGEUP") return SDL_SCANCODE_PAGEUP;
    if (normalized == L"PAGEDOWN") return SDL_SCANCODE_PAGEDOWN;

    std::string utf8 = WideToUtf8(normalized);
    SDL_Scancode scancode = SDL_GetScancodeFromName(utf8.c_str());
    if (scancode != SDL_SCANCODE_UNKNOWN)
    {
        return scancode;
    }

    ToUpperAscii(utf8.data());
    return SDL_GetScancodeFromName(utf8.c_str());
}

bool LoadShortcuts(const std::wstring& iniFile, Bindings* const out)
{
    if (out == nullptr)
    {
        return false;
    }

    constexpr wchar_t kMissing[] = L"{TRIAL_NETPLAY_MISSING_SHORTCUT}";
    bool valid = true;
    for (const trialnet::shortcuts::Descriptor& descriptor : trialnet::shortcuts::kDescriptors)
    {
        wchar_t raw[96] = {};
        GetPrivateProfileStringW(
            L"shortcuts",
            descriptor.iniKey,
            kMissing,
            raw,
            static_cast<DWORD>(std::size(raw)),
            iniFile.c_str());
        const bool missing = std::wcscmp(raw, kMissing) == 0;
        const std::wstring token = missing ? descriptor.defaultBinding : raw;
        const std::wstring normalized = trialnet::shortcuts::NormalizeBindingToken(token);
        const bool cleared = normalized == L"NONE" || normalized == L"OFF" || normalized == L"-";
        const SDL_Scancode key = cleared ? SDL_SCANCODE_UNKNOWN : ParseKeyName(normalized);
        out->shortcuts[trialnet::shortcuts::Index(descriptor.action)] = key;
        if (missing || (!cleared && key == SDL_SCANCODE_UNKNOWN))
        {
            valid = false;
            log::Error(
                "controls",
                "required shortcut %ls is %s (%ls)",
                descriptor.iniKey,
                missing ? "missing" : "invalid",
                token.c_str());
        }
    }
    return valid;
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

PadBinding ParsePadName(const std::wstring& text)
{
    PadBinding binding = {};
    if (text.empty())
    {
        return binding;
    }

    std::string name = WideToUtf8(text);
    ToUpperAscii(name.data());

    if (name == "NONE" || name == "OFF" || name == "-")
    {
        return binding;
    }

    auto gamepadAxis = [&binding](const SDL_GamepadAxis axis, const int direction)
    {
        binding.active = true;
        binding.kind = PadSourceKind::GamepadAxis;
        binding.gamepadAxis = axis;
        binding.direction = direction;
    };

    if (name == "LEFT_STICK_UP")
    {
        gamepadAxis(SDL_GAMEPAD_AXIS_LEFTY, -1);
        return binding;
    }
    if (name == "LEFT_STICK_DOWN")
    {
        gamepadAxis(SDL_GAMEPAD_AXIS_LEFTY, 1);
        return binding;
    }
    if (name == "LEFT_STICK_LEFT")
    {
        gamepadAxis(SDL_GAMEPAD_AXIS_LEFTX, -1);
        return binding;
    }
    if (name == "LEFT_STICK_RIGHT")
    {
        gamepadAxis(SDL_GAMEPAD_AXIS_LEFTX, 1);
        return binding;
    }
    if (name == "RIGHT_STICK_UP") { gamepadAxis(SDL_GAMEPAD_AXIS_RIGHTY, -1); return binding; }
    if (name == "RIGHT_STICK_DOWN") { gamepadAxis(SDL_GAMEPAD_AXIS_RIGHTY, 1); return binding; }
    if (name == "RIGHT_STICK_LEFT") { gamepadAxis(SDL_GAMEPAD_AXIS_RIGHTX, -1); return binding; }
    if (name == "RIGHT_STICK_RIGHT") { gamepadAxis(SDL_GAMEPAD_AXIS_RIGHTX, 1); return binding; }
    if (name == "LEFT_TRIGGER") { gamepadAxis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1); return binding; }
    if (name == "RIGHT_TRIGGER") { gamepadAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1); return binding; }

    int rawIndex = -1;
    char direction[16] = {};
    if (std::sscanf(name.c_str(), "JOY_BUTTON_%d", &rawIndex) == 1 && rawIndex >= 0)
    {
        binding.active = true;
        binding.kind = PadSourceKind::JoystickButton;
        binding.index = rawIndex;
        return binding;
    }
    if (std::sscanf(name.c_str(), "JOY_AXIS_%d_%15s", &rawIndex, direction) == 2 && rawIndex >= 0)
    {
        if (std::strcmp(direction, "NEG") == 0 || std::strcmp(direction, "POS") == 0)
        {
            binding.active = true;
            binding.kind = PadSourceKind::JoystickAxis;
            binding.index = rawIndex;
            binding.direction = std::strcmp(direction, "NEG") == 0 ? -1 : 1;
        }
        return binding;
    }
    if (std::sscanf(name.c_str(), "JOY_HAT_%d_%15s", &rawIndex, direction) == 2 && rawIndex >= 0)
    {
        if (std::strcmp(direction, "UP") == 0) binding.direction = SDL_HAT_UP;
        else if (std::strcmp(direction, "DOWN") == 0) binding.direction = SDL_HAT_DOWN;
        else if (std::strcmp(direction, "LEFT") == 0) binding.direction = SDL_HAT_LEFT;
        else if (std::strcmp(direction, "RIGHT") == 0) binding.direction = SDL_HAT_RIGHT;
        if (binding.direction != 0)
        {
            binding.active = true;
            binding.kind = PadSourceKind::JoystickHat;
            binding.index = rawIndex;
        }
        return binding;
    }

    const SDL_GamepadButton button =
        SdlButtonFromStandard(trialnet::input::ParseStandardButton(text));
    if (button != SDL_GAMEPAD_BUTTON_INVALID)
    {
        binding.active = true;
        binding.kind = PadSourceKind::GamepadButton;
        binding.button = button;
    }

    return binding;
}

std::wstring ReadIniKey(
    const wchar_t* section,
    const wchar_t* key,
    const wchar_t* def,
    const std::wstring& file)
{
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(section, key, def, buf, static_cast<DWORD>(std::size(buf)), file.c_str());
    return buf;
}

void LoadPlayer(
    const std::wstring& iniFile,
    const wchar_t* section,
    const wchar_t* const keyDefaults[kFightSlotCount],
    const wchar_t* const padDefaults[kFightSlotCount],
    PlayerBindings* out)
{
    out->source = trialnet::input::ParseSource(ReadIniKey(section, L"source", L"keyboard", iniFile));

    const wchar_t* slotKeys[kFightSlotCount] = {
        L"up", L"down", L"left", L"right", L"a", L"b", L"c", L"d"};
    for (int i = 0; i < kFightSlotCount; ++i)
    {
        const std::wstring keyName = ReadIniKey(section, slotKeys[i], keyDefaults[i], iniFile);
        const std::wstring normalizedKey = trialnet::input::NormalizeAsciiToken(keyName);
        const bool explicitlyCleared = normalizedKey == L"NONE" || normalizedKey == L"OFF"
            || normalizedKey == L"-";
        SDL_Scancode scancode = explicitlyCleared
            ? SDL_SCANCODE_UNKNOWN
            : ParseKeyName(keyName);
        if (scancode == SDL_SCANCODE_UNKNOWN && !explicitlyCleared)
        {
            scancode = ParseKeyName(keyDefaults[i]);
        }
        out->keys[static_cast<std::size_t>(i)] = scancode;

        const std::wstring padKey = std::wstring(slotKeys[i]) + L"_pad";
        const std::wstring padName = ReadIniKey(section, padKey.c_str(), padDefaults[i], iniFile);
        out->pad[static_cast<std::size_t>(i)] = ParsePadName(padName);
        const std::wstring normalized = trialnet::input::NormalizeAsciiToken(padName);
        if (!normalized.empty() && normalized != L"NONE" && normalized != L"OFF" && normalized != L"-"
            && !out->pad[static_cast<std::size_t>(i)].active)
        {
            log::Warn(
                "controls",
                "P%d %ls controller binding '%ls' is not recognized",
                std::wcscmp(section, L"controls_p2") == 0 ? 2 : 1,
                slotKeys[i],
                padName.c_str());
        }
    }
}

bool SamePadBinding(const PadBinding& left, const PadBinding& right)
{
    return left.active && right.active
        && left.kind == right.kind
        && left.button == right.button
        && left.gamepadAxis == right.gamepadAxis
        && left.index == right.index
        && left.direction == right.direction;
}

bool ValidateResolvedBindings(const Bindings& bindings)
{
    std::vector<SDL_Scancode> keyboardOwners;
    const PlayerBindings* const players[] = {&bindings.p1, &bindings.p2};
    for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
    {
        const PlayerBindings& player = *players[playerIndex];
        if (player.source == trialnet::input::Source::Keyboard)
        {
            for (int slot = 0; slot < kFightSlotCount; ++slot)
            {
                const SDL_Scancode key = player.keys[static_cast<std::size_t>(slot)];
                if (key == SDL_SCANCODE_UNKNOWN)
                {
                    continue;
                }
                for (const SDL_Scancode existing : keyboardOwners)
                {
                    if (existing == key)
                    {
                        log::Error(
                            "controls",
                            "ambiguous keyboard binding player=%d slot=%d key=%s",
                            playerIndex + 1,
                            slot,
                            SDL_GetScancodeName(key));
                        return false;
                    }
                }
                keyboardOwners.push_back(key);
            }
        }
        else
        {
            for (int slot = 0; slot < kFightSlotCount; ++slot)
            {
                const PadBinding& current = player.pad[static_cast<std::size_t>(slot)];
                if (!current.active)
                {
                    continue;
                }
                for (int prior = 0; prior < slot; ++prior)
                {
                    if (SamePadBinding(current, player.pad[static_cast<std::size_t>(prior)]))
                    {
                        log::Error(
                            "controls",
                            "ambiguous controller binding player=%d slots=%d/%d",
                            playerIndex + 1,
                            prior,
                            slot);
                        return false;
                    }
                }
            }
        }
    }

    for (const trialnet::shortcuts::Descriptor& descriptor : trialnet::shortcuts::kDescriptors)
    {
        const SDL_Scancode key = bindings.shortcuts[trialnet::shortcuts::Index(descriptor.action)];
        if (key == SDL_SCANCODE_UNKNOWN)
        {
            continue;
        }
        for (const SDL_Scancode existing : keyboardOwners)
        {
            if (existing == key)
            {
                log::Error(
                    "controls",
                    "ambiguous shortcut binding %ls key=%s",
                    descriptor.iniKey,
                    SDL_GetScancodeName(key));
                return false;
            }
        }
        keyboardOwners.push_back(key);
    }
    return true;
}

}

Bindings DefaultBindings()
{
    Bindings bindings{};
    bindings.p1.source = trialnet::input::Source::Keyboard;
    bindings.p2.source = trialnet::input::Source::Keyboard;
    for (int i = 0; i < kFightSlotCount; ++i)
    {
        bindings.p1.keys[static_cast<std::size_t>(i)] = ParseKeyName(kP1Defaults[i]);
        bindings.p2.keys[static_cast<std::size_t>(i)] = ParseKeyName(kP2Defaults[i]);
        bindings.p1.pad[static_cast<std::size_t>(i)] = ParsePadName(kP1PadDefaults[i]);
        bindings.p2.pad[static_cast<std::size_t>(i)] = ParsePadName(kP2PadDefaults[i]);
    }
    for (const trialnet::shortcuts::Descriptor& descriptor : trialnet::shortcuts::kDescriptors)
    {
        bindings.shortcuts[trialnet::shortcuts::Index(descriptor.action)] =
            ParseKeyName(descriptor.defaultBinding);
    }
    return bindings;
}

void InitializeDefaults()
{
    g_bindings = DefaultBindings();
}

bool LoadFromSessionIni(const std::wstring& iniFile)
{
    g_bindings = DefaultBindings();

    if (iniFile.empty() || GetFileAttributesW(iniFile.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        log::Debug("controls", "no session ini — using default bindings");
        LogBindingsSummary();
        return false;
    }

    LoadPlayer(iniFile, L"controls_p1", kP1Defaults, kP1PadDefaults, &g_bindings.p1);
    LoadPlayer(iniFile, L"controls_p2", kP2Defaults, kP2PadDefaults, &g_bindings.p2);
    const bool shortcutsValid = LoadShortcuts(iniFile, &g_bindings);
    const bool conflictsValid = ValidateResolvedBindings(g_bindings);

    log::Info("controls", "loaded bindings from %ls", iniFile.c_str());
    LogBindingsSummary();
    return shortcutsValid && conflictsValid;
}

const Bindings& Get()
{
    return g_bindings;
}

SDL_Scancode KeyForPlayer(const int playerIndex, const int slot)
{
    if (slot < 0 || slot >= kFightSlotCount || playerIndex < 0 || playerIndex > 1)
    {
        return SDL_SCANCODE_UNKNOWN;
    }

    const PlayerBindings& player = playerIndex == 1 ? g_bindings.p2 : g_bindings.p1;
    return player.keys[static_cast<std::size_t>(slot)];
}

const PadBinding& PadForPlayer(const int playerIndex, const int slot)
{
    static const PadBinding kEmpty = {};
    if (slot < 0 || slot >= kFightSlotCount || playerIndex < 0 || playerIndex > 1)
    {
        return kEmpty;
    }

    const PlayerBindings& player = playerIndex == 1 ? g_bindings.p2 : g_bindings.p1;
    return player.pad[static_cast<std::size_t>(slot)];
}

trialnet::input::Source SourceForPlayer(const int playerIndex)
{
    return playerIndex == 1 ? g_bindings.p2.source : g_bindings.p1.source;
}

SDL_Scancode ShortcutKey(const trialnet::shortcuts::Action action)
{
    const std::size_t index = trialnet::shortcuts::Index(action);
    return index < g_bindings.shortcuts.size()
        ? g_bindings.shortcuts[index]
        : SDL_SCANCODE_UNKNOWN;
}

bool ShortcutConflictsWithFightBinding(
    const trialnet::shortcuts::Action action,
    const int bindingPlayerIndex)
{
    if (bindingPlayerIndex < 0 || bindingPlayerIndex > 1)
    {
        return false;
    }
    const SDL_Scancode shortcut = ShortcutKey(action);
    if (shortcut == SDL_SCANCODE_UNKNOWN)
    {
        return false;
    }
    const PlayerBindings& player = bindingPlayerIndex == 1 ? g_bindings.p2 : g_bindings.p1;
    if (player.source != trialnet::input::Source::Keyboard)
    {
        return false;
    }
    for (const SDL_Scancode fightKey : player.keys)
    {
        if (fightKey == shortcut)
        {
            return true;
        }
    }
    return false;
}

bool ShortcutConflictsWithAnyFightBinding(const trialnet::shortcuts::Action action)
{
    return ShortcutConflictsWithFightBinding(action, 0)
        || ShortcutConflictsWithFightBinding(action, 1);
}

void LogBindingsSummary()
{
    const char* slotNames[kFightSlotCount] = {
        "up", "down", "left", "right", "a", "b", "c", "d"};

    for (int player = 0; player <= 1; ++player)
    {
        log::Info(
            "controls",
            "P%d fight source=%s",
            player + 1,
            trialnet::input::SourceDisplayName(SourceForPlayer(player)));
        for (int slot = 0; slot < kFightSlotCount; ++slot)
        {
            const SDL_Scancode key = KeyForPlayer(player, slot);
            const char* keyName = SDL_GetScancodeName(key);
            const PadBinding& pad = PadForPlayer(player, slot);
            if (pad.active)
            {
                if (pad.kind == PadSourceKind::GamepadAxis)
                {
                    log::Info(
                        "controls",
                        "P%d %s key=%s pad=gamepad-axis axis=%d sign=%d",
                        player + 1,
                        slotNames[slot],
                        keyName != nullptr ? keyName : "?",
                        static_cast<int>(pad.gamepadAxis),
                        pad.direction);
                }
                else if (pad.kind == PadSourceKind::GamepadButton)
                {
                    const char* padName = SDL_GetGamepadStringForButton(pad.button);
                    log::Info(
                        "controls",
                        "P%d %s key=%s pad=%s",
                        player + 1,
                        slotNames[slot],
                        keyName != nullptr ? keyName : "?",
                        padName != nullptr ? padName : "?");
                }
                else
                {
                    log::Info(
                        "controls",
                        "P%d %s key=%s pad=raw kind=%d index=%d direction=%d",
                        player + 1,
                        slotNames[slot],
                        keyName != nullptr ? keyName : "?",
                        static_cast<int>(pad.kind),
                        pad.index,
                        pad.direction);
                }
            }
            else
            {
                log::Debug(
                    "controls",
                    "P%d %s key=%s pad=none",
                    player + 1,
                    slotNames[slot],
                    keyName != nullptr ? keyName : "?");
            }
        }
    }
    for (const trialnet::shortcuts::Descriptor& descriptor : trialnet::shortcuts::kDescriptors)
    {
        const SDL_Scancode key = ShortcutKey(descriptor.action);
        log::Info(
            "controls",
            "shortcut %ls=%s",
            descriptor.iniKey,
            key != SDL_SCANCODE_UNKNOWN ? SDL_GetScancodeName(key) : "none");
    }
}

}
