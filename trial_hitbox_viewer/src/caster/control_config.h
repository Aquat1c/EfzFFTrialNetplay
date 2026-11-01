#pragma once

#include "input_binding_tokens.h"
#include "trial_shortcuts.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#include <array>
#include <string>

namespace thv::controls
{

constexpr int kFightSlotCount = 8;

enum class PadSourceKind
{
    None,
    GamepadButton,
    GamepadAxis,
    JoystickButton,
    JoystickAxis,
    JoystickHat,
};

struct PadBinding
{
    bool active = false;
    PadSourceKind kind = PadSourceKind::None;
    SDL_GamepadButton button = SDL_GAMEPAD_BUTTON_INVALID;
    SDL_GamepadAxis gamepadAxis = SDL_GAMEPAD_AXIS_INVALID;
    int index = -1;
    int direction = 0;    // axis sign (-1/+1) or SDL_HAT_* mask
};

struct PlayerBindings
{
    trialnet::input::Source source = trialnet::input::Source::Keyboard;
    std::array<SDL_Scancode, kFightSlotCount> keys{};
    std::array<PadBinding, kFightSlotCount> pad{};
};

struct Bindings
{
    PlayerBindings p1{};
    PlayerBindings p2{};
    std::array<SDL_Scancode, trialnet::shortcuts::kActionCount> shortcuts{};
};

Bindings DefaultBindings();
void InitializeDefaults();
bool LoadFromSessionIni(const std::wstring& iniFile);
const Bindings& Get();
SDL_Scancode KeyForPlayer(int playerIndex, int slot);
const PadBinding& PadForPlayer(int playerIndex, int slot);
trialnet::input::Source SourceForPlayer(int playerIndex);
SDL_Scancode ShortcutKey(trialnet::shortcuts::Action action);
bool ShortcutConflictsWithFightBinding(
    trialnet::shortcuts::Action action,
    int bindingPlayerIndex);
bool ShortcutConflictsWithAnyFightBinding(trialnet::shortcuts::Action action);

void LogBindingsSummary();

}
