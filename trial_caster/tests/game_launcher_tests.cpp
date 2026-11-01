#include "game_launcher.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3)
    {
        return 1;
    }

    wchar_t temporaryDirectory[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temporaryDirectory) == 0u)
    {
        return 2;
    }
    const std::filesystem::path marker = std::filesystem::path(temporaryDirectory)
        / (L"trial_injection_" + std::to_wstring(GetCurrentProcessId()) + L".txt");
    std::error_code ignored;
    std::filesystem::remove(marker, ignored);
    SetEnvironmentVariableW(L"TRIAL_LAUNCH_PROBE_MARKER", marker.c_str());

    trial_caster::GameLauncher launcher;
    std::atomic<bool> beforeResumeCalled{false};
    const trial_caster::LaunchResult result = launcher.Launch(
        argv[1],
        {std::filesystem::path(argv[2])},
        false,
        {},
        nullptr,
        [&beforeResumeCalled]() { beforeResumeCalled.store(true); });
    SetEnvironmentVariableW(L"TRIAL_LAUNCH_PROBE_MARKER", nullptr);
    if (!result.ok || result.processId == 0u || !beforeResumeCalled.load())
    {
        return 3;
    }

    const ULONGLONG deadline = GetTickCount64() + 5000u;
    while (!std::filesystem::is_regular_file(marker) && GetTickCount64() < deadline)
    {
        Sleep(10u);
    }
    if (!std::filesystem::is_regular_file(marker))
    {
        return 4;
    }
    std::ifstream input(marker, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)), {});
    std::filesystem::remove(marker, ignored);
    if (contents != "injected")
    {
        return 5;
    }

    std::atomic<bool> cancelled{true};
    const trial_caster::LaunchResult cancelledResult = launcher.Launch(
        argv[1],
        {std::filesystem::path(argv[2])},
        false,
        {},
        &cancelled);
    return !cancelledResult.ok && cancelledResult.processId == 0u ? 0 : 6;
}
