#include "caster_config.h"
#include "control_bindings.h"
#include "input_binding_tokens.h"

#include <windows.h>

#include <array>
#include <cstdio>
#include <string>
#include <utility>

namespace
{

int g_failures = 0;

void Expect(const bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

std::wstring TempIni(const wchar_t* suffix)
{
    wchar_t directory[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, directory);
    return std::wstring(directory) + L"trial_controls_" + std::to_wstring(GetCurrentProcessId())
        + suffix + L".ini";
}

void TestTokenAliases()
{
    using trialnet::input::ParseStandardButton;
    using Button = trialnet::input::StandardButton;
    const std::array<std::pair<const wchar_t*, Button>, 20> cases = {{
        {L"A", Button::South}, {L"cross", Button::South}, {L"SOUTH", Button::South},
        {L"B", Button::East}, {L"circle", Button::East}, {L"EAST", Button::East},
        {L"X", Button::West}, {L"square", Button::West}, {L"WEST", Button::West},
        {L"Y", Button::North}, {L"triangle", Button::North}, {L"NORTH", Button::North},
        {L"dpup", Button::DpadUp}, {L"DPAD_UP", Button::DpadUp},
        {L"dpdown", Button::DpadDown}, {L"dpleft", Button::DpadLeft},
        {L"dpright", Button::DpadRight}, {L"rightshoulder", Button::RightShoulder},
        {L"L1", Button::LeftShoulder}, {L"OPTIONS", Button::Start},
    }};

    for (const auto& [token, expected] : cases)
    {
        Expect(ParseStandardButton(token) == expected, "gamepad alias did not map to semantic button");
    }
    Expect(ParseStandardButton(L"not_a_button") == Button::Invalid, "invalid token accepted");
    Expect(trialnet::input::CanonicalizePadToken(L"x") == L"WEST", "face token not canonicalized");
    Expect(trialnet::input::CanonicalizePadToken(L"dpup") == L"DPAD_UP", "D-pad token not canonicalized");
    Expect(
        trialnet::shortcuts::NormalizeBindingToken(L"-") == L"MINUS",
        "minus shortcut punctuation was corrupted");
    Expect(
        trialnet::shortcuts::NormalizeBindingToken(L"Keypad +") == L"KP_PLUS",
        "keypad-plus shortcut alias was not canonicalized");
}

void TestCleanInstallDefaults()
{
    trial_caster::CasterConfig config;
    Expect(config.controls.p1.source == trialnet::input::Source::Keyboard, "P1 clean source is not keyboard");
    Expect(config.controls.p2.source == trialnet::input::Source::Keyboard, "P2 clean source is not keyboard");
    for (int slot = 0; slot < trial_caster::kFightSlotCount; ++slot)
    {
        Expect(!config.controls.p1.keys[static_cast<std::size_t>(slot)].empty(), "P1 clean key missing");
        Expect(!config.controls.p2.keys[static_cast<std::size_t>(slot)].empty(), "P2 clean key missing");
        Expect(!config.controls.p1.pad[static_cast<std::size_t>(slot)].empty(), "P1 controller default missing");
    }
    for (const trialnet::shortcuts::Descriptor& descriptor : trialnet::shortcuts::kDescriptors)
    {
        Expect(
            config.controls.shortcuts[descriptor.action] == descriptor.defaultBinding,
            "clean shortcut default missing");
    }
    Expect(!config.allowPracticeKeys, "practice keys must default off");
}

void TestControllerDeviceAssignment()
{
    trial_caster::ControlBindings bindings = trial_caster::DefaultControlBindings();
    Expect(trial_caster::ControllerDeviceIndexForPlayer(bindings, 0) == -1,
        "keyboard P1 unexpectedly owns a controller");
    Expect(trial_caster::ControllerDeviceIndexForPlayer(bindings, 1) == -1,
        "keyboard P2 unexpectedly owns a controller");

    bindings.p2.source = trialnet::input::Source::Controller;
    Expect(trial_caster::ControllerDeviceIndexForPlayer(bindings, 1) == 0,
        "single P2 controller was not assigned device 0");

    bindings.p1.source = trialnet::input::Source::Controller;
    Expect(trial_caster::ControllerDeviceIndexForPlayer(bindings, 0) == 0,
        "P1 did not own controller device 0");
    Expect(trial_caster::ControllerDeviceIndexForPlayer(bindings, 1) == 1,
        "P2 did not own controller device 1");
    Expect(trial_caster::ControllerDeviceIndexForPlayer(bindings, -1) == -1,
        "invalid player index was assigned a controller");
}

void TestConflictValidation()
{
    trial_caster::ControlBindings bindings = trial_caster::DefaultControlBindings();
    std::wstring error;
    Expect(trial_caster::ValidateControlBindings(bindings, &error),
        "clean defaults failed conflict validation");

    bindings.shortcuts[trialnet::shortcuts::Action::Pause] = bindings.p1.keys[4];
    Expect(!trial_caster::ValidateControlBindings(bindings, &error),
        "shortcut/fight-key conflict was accepted");

    bindings = trial_caster::DefaultControlBindings();
    bindings.p2.keys[0] = bindings.p1.keys[0];
    Expect(!trial_caster::ValidateControlBindings(bindings, &error),
        "cross-player keyboard conflict was accepted");

    bindings = trial_caster::DefaultControlBindings();
    bindings.p1.pad[5] = bindings.p1.pad[4];
    Expect(!trial_caster::ValidateControlBindings(bindings, &error),
        "same-controller duplicate was accepted");

    bindings = trial_caster::DefaultControlBindings();
    bindings.p1.keys[0] = L"NONE";
    bindings.p2.keys[0] = L"NONE";
    bindings.p1.pad[0] = L"NONE";
    bindings.p1.pad[1] = L"NONE";
    Expect(trial_caster::ValidateControlBindings(bindings, &error),
        "explicit cleared bindings were treated as conflicts");
}

void TestIniRoundTrip()
{
    const std::wstring casterIni = TempIni(L"_caster");
    const std::wstring sessionIni = TempIni(L"_session");
    DeleteFileW(casterIni.c_str());
    DeleteFileW(sessionIni.c_str());

    trial_caster::ControlBindings saved = trial_caster::DefaultControlBindings();
    saved.p1.source = trialnet::input::Source::Controller;
    saved.p2.source = trialnet::input::Source::Keyboard;
    saved.p1.pad[4] = L"WEST";
    saved.p2.pad[4] = L"JOY_BUTTON_7";
    saved.p2.keys[4] = L"F8";
    saved.shortcuts[trialnet::shortcuts::Action::IncreaseInputDelay] = L"keypad +";
    saved.shortcuts[trialnet::shortcuts::Action::Pause] = L"F10";
    Expect(trial_caster::SaveControlsToCasterIni(casterIni, saved), "caster controls save failed");

    trial_caster::ControlBindings loaded{};
    trial_caster::LoadControlsFromCasterIni(casterIni, &loaded);
    Expect(loaded.p1.source == trialnet::input::Source::Controller, "P1 source did not round-trip");
    Expect(loaded.p2.source == trialnet::input::Source::Keyboard, "P2 source did not round-trip");
    Expect(loaded.p1.pad[4] == L"WEST", "active controller binding did not round-trip");
    Expect(loaded.p2.pad[4] == L"JOY_BUTTON_7", "inactive controller bank was lost");
    Expect(loaded.p2.keys[4] == L"F8", "active keyboard binding did not round-trip");
    Expect(
        loaded.shortcuts[trialnet::shortcuts::Action::IncreaseInputDelay] == L"KP_PLUS",
        "shortcut token was not normalized");
    Expect(
        loaded.shortcuts[trialnet::shortcuts::Action::Pause] == L"F10",
        "shortcut binding did not round-trip");

    Expect(
        trial_caster::WriteControlsToSessionIni(sessionIni, saved),
        "session controls save failed");
    wchar_t value[64] = {};
    GetPrivateProfileStringW(L"controls_p1", L"source", L"", value, 64, sessionIni.c_str());
    Expect(std::wstring(value) == L"controller", "session P1 source missing");
    GetPrivateProfileStringW(L"controls_p2", L"source", L"", value, 64, sessionIni.c_str());
    Expect(std::wstring(value) == L"keyboard", "session P2 source missing");
    GetPrivateProfileStringW(L"controls_p1", L"a_pad", L"", value, 64, sessionIni.c_str());
    Expect(std::wstring(value) == L"WEST", "session controller binding missing");
    GetPrivateProfileStringW(
        L"shortcuts", L"increase_input_delay", L"", value, 64, sessionIni.c_str());
    Expect(std::wstring(value) == L"KP_PLUS", "session shortcut missing or unnormalized");
    GetPrivateProfileStringW(L"shortcuts", L"pause", L"", value, 64, sessionIni.c_str());
    Expect(std::wstring(value) == L"F10", "session practice shortcut missing");

    WritePrivateProfileStringW(L"controls", L"p1_up", L"", casterIni.c_str());
    WritePrivateProfileStringW(L"controls", L"p1_down", L"NONE", casterIni.c_str());
    WritePrivateProfileStringW(L"controls", L"p1_a_pad", L"", casterIni.c_str());
    WritePrivateProfileStringW(L"controls", L"p1_b_pad", L"NONE", casterIni.c_str());
    trial_caster::LoadControlsFromCasterIni(casterIni, &loaded);
    Expect(loaded.p1.keys[0] == L"UP", "empty legacy key did not migrate to default");
    Expect(loaded.p1.keys[1] == L"NONE", "intentional keyboard clear was not preserved");
    Expect(loaded.p1.pad[4] == L"SOUTH", "empty legacy controller binding did not migrate");
    Expect(loaded.p1.pad[5] == L"NONE", "intentional controller clear was not preserved");

    WritePrivateProfileStringW(L"controls", L"p1_source", L"invalid", casterIni.c_str());
    trial_caster::LoadControlsFromCasterIni(casterIni, &loaded);
    Expect(loaded.p1.source == trialnet::input::Source::Keyboard, "invalid source did not fail safe");

    DeleteFileW(casterIni.c_str());
    DeleteFileW(sessionIni.c_str());
}

}

int main()
{
    TestTokenAliases();
    TestCleanInstallDefaults();
    TestControllerDeviceAssignment();
    TestConflictValidation();
    TestIniRoundTrip();
    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d control binding test(s) failed\n", g_failures);
        return 1;
    }
    std::puts("control binding tests passed");
    return 0;
}
