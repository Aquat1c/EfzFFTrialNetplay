#include "control_config.h"
#include "control_bindings.h"
#include "controller_device_routing.h"
#include "offline_input_routing.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace thv::log
{
void Debug(const char*, const char*, ...) {}
void Info(const char*, const char*, ...) {}
void Warn(const char*, const char*, ...) {}
void Error(const char*, const char*, ...) {}
}

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

void Put(const std::wstring& file, const wchar_t* section, const wchar_t* key, const wchar_t* value)
{
    WritePrivateProfileStringW(section, key, value, file.c_str());
}

void TestControllerDeviceRouting()
{
    using Candidate = trialnet::input::ControllerDeviceCandidate;
    using trialnet::input::kNoControllerDevice;

    const std::vector<Candidate> devices = {
        {20u, false},
        {10u, true},
        {10u, false},
        {30u, false},
        {40u, true},
        {0u, true},
    };
    const std::vector<trialnet::input::ControllerDeviceId> ordered =
        trialnet::input::PreferredControllerDeviceOrder(devices);
    Expect(ordered.size() == 4u, "controller order did not remove duplicate/invalid IDs");
    if (ordered.size() == 4u)
    {
        Expect(ordered[0] == 10u && ordered[1] == 40u
            && ordered[2] == 20u && ordered[3] == 30u,
            "controller order is not mapped-first and stable");
    }

    const auto both = trialnet::input::AssignControllerDevices(devices, true, true);
    Expect(both.player[0] == 10u && both.player[1] == 40u,
        "two controller players did not receive first/second preferred devices");

    const auto p1Only = trialnet::input::AssignControllerDevices(devices, true, false);
    Expect(p1Only.player[0] == 10u && p1Only.player[1] == kNoControllerDevice,
        "P1-only controller did not receive the first preferred device");

    const auto p2Only = trialnet::input::AssignControllerDevices(devices, false, true);
    Expect(p2Only.player[0] == kNoControllerDevice && p2Only.player[1] == 10u,
        "P2-only controller did not receive the first preferred device");

    const std::vector<Candidate> oneDevice = {{77u, false}};
    const auto shortBoth = trialnet::input::AssignControllerDevices(oneDevice, true, true);
    Expect(shortBoth.player[0] == 77u && shortBoth.player[1] == kNoControllerDevice,
        "one device was incorrectly shared by both controller players");
}

void TestOfflineSourceRouting()
{
    using trialnet::input::Source;

    int keyboardSamples = 0;
    int controllerSamples = 0;
    const bool keyboardResult = trialnet::input::SampleConfiguredFightSource(
        Source::Keyboard,
        [&]() { ++keyboardSamples; return true; },
        [&]() { ++controllerSamples; return false; });
    Expect(keyboardResult, "selected keyboard source did not produce its sample");
    Expect(keyboardSamples == 1 && controllerSamples == 0,
        "keyboard source also sampled the controller");

    keyboardSamples = 0;
    controllerSamples = 0;
    const bool controllerResult = trialnet::input::SampleConfiguredFightSource(
        Source::Controller,
        [&]() { ++keyboardSamples; return false; },
        [&]() { ++controllerSamples; return true; });
    Expect(controllerResult, "selected controller source did not produce its sample");
    Expect(keyboardSamples == 0 && controllerSamples == 1,
        "controller source also sampled the keyboard");

    Expect(trialnet::input::ControllerPlayerMaySample(0, false, 1),
        "offline P1 controller was tied to the online local binding bank");
    Expect(trialnet::input::ControllerPlayerMaySample(1, false, 0),
        "offline P2 controller was tied to the online local binding bank");
    Expect(trialnet::input::ControllerPlayerMaySample(0, true, 0),
        "online local controller bank was rejected");
    Expect(!trialnet::input::ControllerPlayerMaySample(1, true, 0),
        "online remote controller bank was sampled locally");
    Expect(!trialnet::input::ControllerPlayerMaySample(-1, false, 0),
        "invalid offline player was accepted");
}

void TestOfflineDiCanonicalizationAndOverlay()
{
    using trialnet::input::Source;
    constexpr std::size_t kSlots = trialnet::input::kOfflineFightSlotCount;

    std::array<std::uint8_t, kSlots> keyboardDiks = {
        0xC8u, 0xD0u, 0xCBu, 0xCDu, 0x2Cu, 0u, 0x2Eu, 0x2Fu};
    std::array<bool, kSlots> padActive = {
        true, true, true, true, true, false, true, true};

    const auto keyboardPlan = trialnet::input::BuildOfflineDiBindingPlan(
        0, Source::Keyboard, keyboardDiks, padActive);
    Expect(keyboardPlan.keyCodes[5] == 0u
            && keyboardPlan.deviceTypes[5] == trialnet::input::kDiDisabledDevice,
        "NONE keyboard binding became a live DI slot");
    Expect(keyboardPlan.keyCodes[4] == 0x2Cu
            && keyboardPlan.deviceTypes[4] == trialnet::input::kDiKeyboardDevice,
        "active keyboard binding was disabled");

    const auto controllerPlan = trialnet::input::BuildOfflineDiBindingPlan(
        0, Source::Controller, keyboardDiks, padActive);
    Expect(controllerPlan.keyCodes[5] == 0u
            && controllerPlan.deviceTypes[5] == trialnet::input::kDiDisabledDevice,
        "NONE controller binding became a live DI slot");
    for (std::size_t slot = 0u; slot < kSlots; ++slot)
    {
        if (padActive[slot])
        {
            Expect(controllerPlan.keyCodes[slot] != 0u
                    && controllerPlan.deviceTypes[slot] == trialnet::input::kDiKeyboardDevice,
                "active controller binding lacked a safe DIK route");
            Expect(controllerPlan.keyCodes[slot] != 0x01u,
                "controller route reused DIK Escape");
        }
    }

    const auto p2ControllerPlan = trialnet::input::BuildOfflineDiBindingPlan(
        1, Source::Controller, keyboardDiks, padActive);
    for (std::size_t left = 0u; left < kSlots; ++left)
    {
        for (std::size_t right = 0u; right < kSlots; ++right)
        {
            if (controllerPlan.keyCodes[left] != 0u
                && p2ControllerPlan.keyCodes[right] != 0u)
            {
                Expect(controllerPlan.keyCodes[left] != p2ControllerPlan.keyCodes[right],
                    "P1/P2 private controller DI slots overlap");
            }
        }
        for (const std::uint8_t physicalKeyboardDik : keyboardDiks)
        {
            if (physicalKeyboardDik != 0u)
            {
                if (controllerPlan.keyCodes[left] != 0u)
                {
                    Expect(controllerPlan.keyCodes[left] != physicalKeyboardDik,
                        "P1 private controller DI slot collides with a physical keyboard binding");
                }
                if (p2ControllerPlan.keyCodes[left] != 0u)
                {
                    Expect(p2ControllerPlan.keyCodes[left] != physicalKeyboardDik,
                        "P2 private controller DI slot collides with a physical keyboard binding");
                }
            }
        }
    }

    // Joystick button 1 must not be treated as a DIK (it would release Escape).
    std::array<std::uint8_t, 256> di{};
    di.fill(0x55u);
    constexpr std::uint8_t kDikEscape = 0x01u;
    constexpr std::uint8_t kDikZ = 0x2Cu;
    di[kDikEscape] = trialnet::input::kDiKeyDown;
    di[kDikZ] = trialnet::input::kDiKeyDown;
    std::array<std::uint8_t, kSlots> legacyTypes{};
    std::array<std::uint8_t, kSlots> legacyCodes{};
    legacyTypes.fill(trialnet::input::kDiDisabledDevice);
    legacyCodes.fill(0u);
    legacyTypes[0] = 1u;
    legacyCodes[0] = kDikEscape;
    legacyTypes[1] = trialnet::input::kDiKeyboardDevice;
    legacyCodes[1] = kDikZ;
    legacyTypes[2] = trialnet::input::kDiKeyboardDevice;
    legacyCodes[2] = 0u;
    const std::uint8_t bufferZeroBefore = di[0];
    trialnet::input::ClearManagedDiSlots(
        di.data(), legacyTypes.data(), legacyCodes.data(), kSlots);
    Expect(di[kDikEscape] == trialnet::input::kDiKeyDown,
        "native joystick index corrupted DIK Escape during replacement");
    Expect(di[kDikZ] == 0u, "managed keyboard DIK was not cleared for replacement");
    Expect(di[0] == bufferZeroBefore, "disabled/zero DIK mutated buffer index zero");

    std::array<bool, kSlots> down{};
    down[0] = true;
    down[4] = true;
    down[5] = true;
    di.fill(0u);
    di[kDikEscape] = trialnet::input::kDiKeyDown;
    trialnet::input::MergeManagedDiSlots(
        di.data(),
        controllerPlan.deviceTypes.data(),
        controllerPlan.keyCodes.data(),
        down.data(),
        kSlots);
    Expect(di[kDikEscape] == trialnet::input::kDiKeyDown,
        "controller overlay overwrote an unrelated Escape state");
    Expect(di[controllerPlan.keyCodes[0]] == trialnet::input::kDiKeyDown
            && di[controllerPlan.keyCodes[4]] == trialnet::input::kDiKeyDown,
        "selected controller slots were not overlaid through safe DIKs");
    Expect(di[0] == 0u, "disabled controller slot wrote buffer index zero");

    di.fill(0u);
    constexpr std::uint8_t kUnrelatedDik = 0x1Cu; // Return
    di[kUnrelatedDik] = trialnet::input::kDiKeyDown;
    trialnet::input::MergeManagedDiMask(
        di.data(),
        controllerPlan.deviceTypes.data(),
        controllerPlan.keyCodes.data(),
        0xB1u,
        kSlots);
    Expect(di[kUnrelatedDik] == trialnet::input::kDiKeyDown,
        "wire-mask merge erased unrelated native DI state");
    for (std::size_t slot = 0u; slot < kSlots; ++slot)
    {
        const bool expected = padActive[slot] && (0xB1u & (1u << slot)) != 0u;
        const std::uint8_t dik = controllerPlan.keyCodes[slot];
        if (dik != 0u)
        {
            Expect((di[dik] & trialnet::input::kDiKeyDown) != 0u == expected,
                "wire-mask bit did not match the managed DI slot");
        }
    }

    const auto invalidPlan = trialnet::input::BuildOfflineDiBindingPlan(
        2, Source::Controller, keyboardDiks, padActive);
    for (std::size_t slot = 0u; slot < kSlots; ++slot)
    {
        Expect(invalidPlan.keyCodes[slot] == 0u
                && invalidPlan.deviceTypes[slot] == trialnet::input::kDiDisabledDevice,
            "invalid player produced a live offline DI route");
    }
}

void TestLauncherWriterToDllLoaderContract()
{
    wchar_t directory[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, directory);
    const std::wstring file = std::wstring(directory) + L"trial_control_contract_"
        + std::to_wstring(GetCurrentProcessId()) + L".ini";
    DeleteFileW(file.c_str());

    trial_caster::ControlBindings launcher = trial_caster::DefaultControlBindings();
    launcher.p1.source = trialnet::input::Source::Controller;
    launcher.p1.pad[0] = L"LEFT_STICK_UP";
    launcher.p1.pad[4] = L"WEST";
    launcher.p1.pad[5] = L"RIGHT_TRIGGER";
    launcher.p1.pad[6] = L"LEFTSHOULDER";
    launcher.p2.source = trialnet::input::Source::Keyboard;
    launcher.p2.keys[4] = L"F8";
    launcher.p2.keys[5] = L"NONE";
    launcher.p2.pad[4] = L"JOY_BUTTON_11";
    trial_caster::WriteControlsToSessionIni(file, launcher);

    Expect(thv::controls::LoadFromSessionIni(file),
        "DLL rejected the launcher's production session control schema");
    Expect(thv::controls::SourceForPlayer(0) == trialnet::input::Source::Controller,
        "launcher P1 source did not reach the DLL");
    Expect(thv::controls::SourceForPlayer(1) == trialnet::input::Source::Keyboard,
        "launcher P2 source did not reach the DLL");

    using Kind = thv::controls::PadSourceKind;
    const auto& p1Up = thv::controls::PadForPlayer(0, 0);
    const auto& p1A = thv::controls::PadForPlayer(0, 4);
    const auto& p1B = thv::controls::PadForPlayer(0, 5);
    const auto& inactiveP2A = thv::controls::PadForPlayer(1, 4);
    Expect(p1Up.kind == Kind::GamepadAxis && p1Up.gamepadAxis == SDL_GAMEPAD_AXIS_LEFTY
        && p1Up.direction == -1, "launcher mapped axis changed across session boundary");
    Expect(p1A.kind == Kind::GamepadButton && p1A.button == SDL_GAMEPAD_BUTTON_WEST,
        "launcher face button changed across session boundary");
    Expect(p1B.kind == Kind::GamepadAxis && p1B.gamepadAxis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
        "launcher trigger changed across session boundary");
    Expect(thv::controls::KeyForPlayer(1, 4) == SDL_SCANCODE_F8,
        "launcher P2 keyboard bank changed across session boundary");
    Expect(thv::controls::KeyForPlayer(1, 5) == SDL_SCANCODE_UNKNOWN,
        "explicitly cleared keyboard binding was restored to a default");
    Expect(inactiveP2A.kind == Kind::JoystickButton && inactiveP2A.index == 11,
        "inactive launcher controller bank was lost at the DLL boundary");

    DeleteFileW(file.c_str());
}

void TestRequiredShortcutSchema()
{
    wchar_t directory[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, directory);
    const std::wstring file = std::wstring(directory) + L"trial_shortcut_schema_"
        + std::to_wstring(GetCurrentProcessId()) + L".ini";
    DeleteFileW(file.c_str());

    for (const trialnet::shortcuts::Descriptor& descriptor : trialnet::shortcuts::kDescriptors)
    {
        Put(file, L"shortcuts", descriptor.iniKey, descriptor.defaultBinding);
    }

    Expect(thv::controls::LoadFromSessionIni(file),
        "complete required shortcut schema was rejected");

    WritePrivateProfileStringW(
        L"shortcuts",
        trialnet::shortcuts::Describe(trialnet::shortcuts::Action::StepFrame).iniKey,
        nullptr,
        file.c_str());
    Expect(!thv::controls::LoadFromSessionIni(file),
        "session with a missing required shortcut was accepted");

    Put(
        file,
        L"shortcuts",
        trialnet::shortcuts::Describe(trialnet::shortcuts::Action::StepFrame).iniKey,
        L"NONE");
    Expect(thv::controls::LoadFromSessionIni(file),
        "explicitly cleared shortcut was treated as a missing binding");
    Expect(
        thv::controls::ShortcutKey(trialnet::shortcuts::Action::StepFrame)
            == SDL_SCANCODE_UNKNOWN,
        "explicitly cleared shortcut became active");

    Put(
        file,
        L"shortcuts",
        trialnet::shortcuts::Describe(trialnet::shortcuts::Action::Pause).iniKey,
        L"Z");
    Expect(!thv::controls::LoadFromSessionIni(file),
        "shortcut/fight-key conflict bypassed the DLL session gate");

    DeleteFileW(file.c_str());
}

}

int main()
{
    TestControllerDeviceRouting();
    TestOfflineSourceRouting();
    TestOfflineDiCanonicalizationAndOverlay();
    TestLauncherWriterToDllLoaderContract();
    TestRequiredShortcutSchema();

    wchar_t directory[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, directory);
    const std::wstring file = std::wstring(directory) + L"trial_dll_controls_"
        + std::to_wstring(GetCurrentProcessId()) + L".ini";
    DeleteFileW(file.c_str());

    Put(file, L"controls_p1", L"source", L"controller");
    Put(file, L"controls_p1", L"up_pad", L"DPUP");
    Put(file, L"controls_p1", L"down_pad", L"DPAD_DOWN");
    Put(file, L"controls_p1", L"left_pad", L"dpleft");
    Put(file, L"controls_p1", L"right_pad", L"DPAD_RIGHT");
    Put(file, L"controls_p1", L"a_pad", L"X");
    Put(file, L"controls_p1", L"b_pad", L"circle");
    Put(file, L"controls_p1", L"c_pad", L"SOUTH");
    Put(file, L"controls_p1", L"d_pad", L"Y");

    Put(file, L"controls_p2", L"source", L"not-valid");
    Put(file, L"controls_p2", L"up_pad", L"LEFT_STICK_UP");
    Put(file, L"controls_p2", L"down_pad", L"RIGHT_TRIGGER");
    Put(file, L"controls_p2", L"left_pad", L"JOY_AXIS_2_NEG");
    Put(file, L"controls_p2", L"right_pad", L"JOY_HAT_0_RIGHT");
    Put(file, L"controls_p2", L"a_pad", L"JOY_BUTTON_6");
    Put(file, L"controls_p2", L"b_pad", L"bogus");
    for (const trialnet::shortcuts::Descriptor& descriptor : trialnet::shortcuts::kDescriptors)
    {
        Put(file, L"shortcuts", descriptor.iniKey, descriptor.defaultBinding);
    }
    Put(file, L"shortcuts", L"increase_input_delay", L"Keypad +");

    Expect(thv::controls::LoadFromSessionIni(file), "session control file did not load");
    Expect(thv::controls::SourceForPlayer(0) == trialnet::input::Source::Controller,
        "controller source not loaded");
    Expect(thv::controls::SourceForPlayer(1) == trialnet::input::Source::Keyboard,
        "invalid source did not fail safe to keyboard");
    Expect(
        thv::controls::ShortcutKey(trialnet::shortcuts::Action::IncreaseInputDelay)
            == SDL_SCANCODE_KP_PLUS,
        "shortcut alias did not reach the DLL");
    Expect(
        thv::controls::ShortcutKey(trialnet::shortcuts::Action::PreviousSaveSlot)
            == SDL_SCANCODE_LEFTBRACKET,
        "canonical punctuation shortcut did not reach the DLL");

    using Kind = thv::controls::PadSourceKind;
    const auto& up = thv::controls::PadForPlayer(0, 0);
    const auto& down = thv::controls::PadForPlayer(0, 1);
    const auto& left = thv::controls::PadForPlayer(0, 2);
    const auto& right = thv::controls::PadForPlayer(0, 3);
    Expect(up.active && up.button == SDL_GAMEPAD_BUTTON_DPAD_UP, "DPUP alias failed");
    Expect(down.active && down.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN, "DPAD_DOWN alias failed");
    Expect(left.active && left.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT, "DPLEFT alias failed");
    Expect(right.active && right.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "DPAD_RIGHT alias failed");
    Expect(thv::controls::PadForPlayer(0, 4).button == SDL_GAMEPAD_BUTTON_WEST, "X face button failed");
    Expect(thv::controls::PadForPlayer(0, 5).button == SDL_GAMEPAD_BUTTON_EAST, "Circle face button failed");
    Expect(thv::controls::PadForPlayer(0, 6).button == SDL_GAMEPAD_BUTTON_SOUTH, "South face button failed");
    Expect(thv::controls::PadForPlayer(0, 7).button == SDL_GAMEPAD_BUTTON_NORTH, "Y face button failed");

    const auto& stick = thv::controls::PadForPlayer(1, 0);
    const auto& trigger = thv::controls::PadForPlayer(1, 1);
    const auto& rawAxis = thv::controls::PadForPlayer(1, 2);
    const auto& rawHat = thv::controls::PadForPlayer(1, 3);
    const auto& rawButton = thv::controls::PadForPlayer(1, 4);
    Expect(stick.kind == Kind::GamepadAxis && stick.gamepadAxis == SDL_GAMEPAD_AXIS_LEFTY
        && stick.direction == -1, "mapped stick direction failed");
    Expect(trigger.kind == Kind::GamepadAxis && trigger.gamepadAxis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
        "mapped trigger failed");
    Expect(rawAxis.kind == Kind::JoystickAxis && rawAxis.index == 2 && rawAxis.direction == -1,
        "raw axis failed");
    Expect(rawHat.kind == Kind::JoystickHat && rawHat.index == 0 && rawHat.direction == SDL_HAT_RIGHT,
        "raw hat failed");
    Expect(rawButton.kind == Kind::JoystickButton && rawButton.index == 6, "raw button failed");
    Expect(!thv::controls::PadForPlayer(1, 5).active, "invalid pad token became active");

    DeleteFileW(file.c_str());
    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d DLL control config test(s) failed\n", g_failures);
        return 1;
    }
    std::puts("DLL control config tests passed");
    return 0;
}
