// Pass the session ini explicitly: GameLauncher's env cleanup would discard it.

#include "game_launcher.h"
#include "util.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

std::filesystem::path SessionIniFromEnvironment()
{
    constexpr const wchar_t* kName = L"TRIAL_CASTER_SESSION_INI";
    const DWORD required = GetEnvironmentVariableW(kName, nullptr, 0u);
    if (required == 0u)
    {
        return {};
    }

    std::vector<wchar_t> value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        kName,
        value.data(),
        static_cast<DWORD>(value.size()));
    if (written == 0u || written >= value.size())
    {
        return {};
    }
    return std::filesystem::path(value.data());
}

}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        std::fwprintf(stderr, L"Usage: launch_smoke <EFZ.EXE> [netplay.dll]\n");
        return 1;
    }

    const std::filesystem::path efzExe = argv[1];
    trial_caster::GameLauncher launcher;

    std::vector<std::filesystem::path> dlls;
    if (argc >= 3)
    {
        dlls.push_back(argv[2]);
    }
    else
    {
        const auto fallback = efzExe.parent_path() / L"netplay.dll";
        if (trial_caster::util::FileExists(fallback))
        {
            dlls.push_back(fallback);
        }
    }

    const std::filesystem::path sessionIni = SessionIniFromEnvironment();
    const auto result = launcher.Launch(efzExe, dlls, true, sessionIni);
    std::fwprintf(stderr, L"%s\n", result.message.c_str());
    if (!result.ok)
    {
        return 2;
    }

    Sleep(3000);
    return 0;
}
