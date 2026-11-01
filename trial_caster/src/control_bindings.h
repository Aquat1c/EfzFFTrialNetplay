#pragma once

#include "input_binding_tokens.h"
#include "trial_shortcuts.h"

#include <array>
#include <cstdint>
#include <string>

struct SDL_Gamepad;
struct SDL_Joystick;

namespace trial_caster
{

constexpr int kFightSlotCount = 8;

constexpr const wchar_t* kSlotLabels[kFightSlotCount] = {
    L"Up",
    L"Down",
    L"Left",
    L"Right",
    L"A",
    L"B",
    L"C",
    L"D",
};

struct PlayerBindings
{
    trialnet::input::Source source = trialnet::input::Source::Keyboard;
    std::array<std::wstring, kFightSlotCount> keys{};
    std::array<std::wstring, kFightSlotCount> pad{};
};

struct ControlBindings
{
    PlayerBindings p1{};
    PlayerBindings p2{};
    trialnet::shortcuts::Bindings shortcuts = trialnet::shortcuts::DefaultBindings();
};

ControlBindings DefaultControlBindings();

void LoadControlsFromCasterIni(const std::wstring& iniFile, ControlBindings* out);
bool SaveControlsToCasterIni(const std::wstring& iniFile, const ControlBindings& bindings);
bool WriteControlsToSessionIni(const std::wstring& iniFile, const ControlBindings& bindings);
bool ValidateControlBindings(const ControlBindings& bindings, std::wstring* error = nullptr);

int ControllerDeviceIndexForPlayer(const ControlBindings& bindings, int playerIndex);

bool EvalPadBinding(const std::wstring& padName, SDL_Gamepad* pad, SDL_Joystick* joystick);
bool IsKeyboardSlotDown(const PlayerBindings& bindings, int slot);
bool IsSelectedSlotDown(
    const PlayerBindings& bindings,
    int slot,
    SDL_Gamepad* pad,
    SDL_Joystick* joystick);
std::uint8_t SampleWireMaskPreview(
    const PlayerBindings& bindings,
    SDL_Gamepad* pad,
    SDL_Joystick* joystick);

}
