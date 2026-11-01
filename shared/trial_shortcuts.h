#pragma once

#include "input_binding_tokens.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace trialnet::shortcuts
{

// Keep stable: the launcher persists these tokens and the DLL resolves the same table.
enum class Group
{
    Network,
    TrialPractice,
    ModPractice,
};

enum class Action : std::size_t
{
    IncreaseInputDelay = 0,
    DecreaseInputDelay,
    TrainingMode,
    RestoreResources,
    ToggleHitboxes,
    ToggleHurtboxes,
    ToggleCollision,
    Pause,
    StepFrame,
    SwitchPlayer,
    PreviousSaveSlot,
    NextSaveSlot,
    SaveState,
    LoadState,
    DebugSave,
    DebugLoad,
    Count,
};

constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::Count);

struct Descriptor
{
    Action action;
    Group group;
    const wchar_t* iniKey;
    const wchar_t* label;
    const wchar_t* defaultBinding;
};

inline constexpr std::array<Descriptor, kActionCount> kDescriptors{{
    {Action::IncreaseInputDelay, Group::Network, L"increase_input_delay", L"Increase input delay", L"KP_PLUS"},
    {Action::DecreaseInputDelay, Group::Network, L"decrease_input_delay", L"Decrease input delay", L"KP_MINUS"},
    {Action::TrainingMode, Group::TrialPractice, L"training_mode", L"Training mode", L"F4"},
    {Action::RestoreResources, Group::TrialPractice, L"restore_resources", L"Restore health and meter", L"F5"},
    {Action::ToggleHitboxes, Group::ModPractice, L"toggle_hitboxes", L"Show attack boxes", L"PAGEUP"},
    {Action::ToggleHurtboxes, Group::ModPractice, L"toggle_hurtboxes", L"Show hurt boxes", L"PAGEDOWN"},
    {Action::ToggleCollision, Group::ModPractice, L"toggle_collision", L"Show collision boxes", L"HOME"},
    {Action::Pause, Group::ModPractice, L"pause", L"Pause practice", L"SPACE"},
    {Action::StepFrame, Group::ModPractice, L"step_frame", L"Advance one frame", L"P"},
    {Action::SwitchPlayer, Group::ModPractice, L"switch_player", L"Switch controlled player", L"KP_0"},
    {Action::PreviousSaveSlot, Group::ModPractice, L"previous_save_slot", L"Previous save slot", L"LEFTBRACKET"},
    {Action::NextSaveSlot, Group::ModPractice, L"next_save_slot", L"Next save slot", L"RIGHTBRACKET"},
    {Action::SaveState, Group::ModPractice, L"save_state", L"Save practice state", L"G"},
    {Action::LoadState, Group::ModPractice, L"load_state", L"Load practice state", L"B"},
    {Action::DebugSave, Group::ModPractice, L"debug_save", L"Quick save", L"F6"},
    {Action::DebugLoad, Group::ModPractice, L"debug_load", L"Quick load", L"F7"},
}};

constexpr std::size_t Index(const Action action)
{
    return static_cast<std::size_t>(action);
}

constexpr const Descriptor& Describe(const Action action)
{
    return kDescriptors[Index(action)];
}

inline std::wstring NormalizeBindingToken(const std::wstring_view raw)
{
    // SDL reports punctuation scancodes as glyphs; stored names stay layout-independent.
    if (raw == L"-") return L"MINUS";
    if (raw == L"=") return L"EQUALS";
    if (raw == L",") return L"COMMA";
    if (raw == L".") return L"PERIOD";
    if (raw == L"/") return L"SLASH";
    if (raw == L";") return L"SEMICOLON";
    if (raw == L"[") return L"LEFTBRACKET";
    if (raw == L"]") return L"RIGHTBRACKET";
    if (raw == L"\\") return L"BACKSLASH";
    if (raw == L"'") return L"APOSTROPHE";
    if (raw == L"`") return L"GRAVE";

    std::wstring token = trialnet::input::NormalizeAsciiToken(raw);
    if (token == L"KEYPAD_+" || token == L"KEYPAD_PLUS" || token == L"KP_+")
    {
        return L"KP_PLUS";
    }
    if (token == L"KEYPAD__" || token == L"KEYPAD_MINUS" || token == L"KP__")
    {
        return L"KP_MINUS";
    }
    if (token == L"PAGE_UP")
    {
        return L"PAGEUP";
    }
    if (token == L"PAGE_DOWN")
    {
        return L"PAGEDOWN";
    }
    if (token == L"LEFT_BRACKET") return L"LEFTBRACKET";
    if (token == L"RIGHT_BRACKET") return L"RIGHTBRACKET";
    return token;
}

struct Bindings
{
    std::array<std::wstring, kActionCount> keys{};

    std::wstring& operator[](const Action action)
    {
        return keys[Index(action)];
    }

    const std::wstring& operator[](const Action action) const
    {
        return keys[Index(action)];
    }
};

inline Bindings DefaultBindings()
{
    Bindings bindings{};
    for (const Descriptor& descriptor : kDescriptors)
    {
        bindings[descriptor.action] = descriptor.defaultBinding;
    }
    return bindings;
}

constexpr bool DescriptorTableIsOrdered()
{
    for (std::size_t index = 0; index < kDescriptors.size(); ++index)
    {
        if (Index(kDescriptors[index].action) != index)
        {
            return false;
        }
    }
    return true;
}

static_assert(DescriptorTableIsOrdered(), "shortcut descriptors must match Action order");

}
