#include "compatibility_runtime.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <vector>

namespace trial_caster
{
namespace
{

std::wstring EnvironmentValue(const wchar_t* const name)
{
    SetLastError(ERROR_SUCCESS);
    DWORD length = GetEnvironmentVariableW(name, nullptr, 0u);
    if (length == 0u)
    {
        return {};
    }

    std::vector<wchar_t> value(static_cast<std::size_t>(length), L'\0');
    length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    if (length == 0u || length >= value.size())
    {
        return {};
    }
    return std::wstring(value.data(), length);
}

std::wstring NarrowText(const char* const text)
{
    if (text == nullptr || text[0] == '\0')
    {
        return {};
    }

    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (count <= 1)
    {
        codePage = CP_ACP;
        flags = 0u;
        count = MultiByteToWideChar(codePage, flags, text, -1, nullptr, 0);
    }
    if (count <= 1)
    {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(codePage, flags, text, -1, result.data(), count);
    result.resize(static_cast<std::size_t>(count - 1));
    return result;
}

bool HasProtonEnvironment()
{
    return !EnvironmentValue(L"STEAM_COMPAT_DATA_PATH").empty()
        || !EnvironmentValue(L"STEAM_COMPAT_CLIENT_INSTALL_PATH").empty()
        || !EnvironmentValue(L"PROTON_VERSION").empty();
}

}

CompatibilityRuntimeInfo DetectCompatibilityRuntime()
{
    CompatibilityRuntimeInfo result{};

    HMODULE const ntdll = GetModuleHandleW(L"ntdll.dll");
    using WineGetStringFn = const char* (__cdecl*)();
    using WineGetHostVersionFn = void (__cdecl*)(const char**, const char**);
    const auto getWineVersion = ntdll != nullptr
        ? reinterpret_cast<WineGetStringFn>(GetProcAddress(ntdll, "wine_get_version"))
        : nullptr;
    if (getWineVersion == nullptr)
    {
        return result;
    }

    result.kind = HasProtonEnvironment()
        ? CompatibilityRuntimeKind::Proton
        : CompatibilityRuntimeKind::Wine;
    result.wineVersion = NarrowText(getWineVersion());

    const auto getWineBuildId = reinterpret_cast<WineGetStringFn>(
        GetProcAddress(ntdll, "wine_get_build_id"));
    if (getWineBuildId != nullptr)
    {
        result.wineBuildId = NarrowText(getWineBuildId());
    }

    const auto getWineHostVersion = reinterpret_cast<WineGetHostVersionFn>(
        GetProcAddress(ntdll, "wine_get_host_version"));
    if (getWineHostVersion != nullptr)
    {
        const char* system = nullptr;
        const char* release = nullptr;
        getWineHostVersion(&system, &release);
        result.hostSystem = NarrowText(system);
        const std::wstring hostRelease = NarrowText(release);
        if (!hostRelease.empty())
        {
            if (!result.hostSystem.empty())
            {
                result.hostSystem += L" ";
            }
            result.hostSystem += hostRelease;
        }
    }

    result.prefixPath = EnvironmentValue(L"WINEPREFIX");
    if (result.prefixPath.empty())
    {
        const std::wstring compatData = EnvironmentValue(L"STEAM_COMPAT_DATA_PATH");
        if (!compatData.empty())
        {
            result.prefixPath = compatData;
            const wchar_t tail = result.prefixPath.back();
            if (tail != L'/' && tail != L'\\')
            {
                result.prefixPath += L'/';
            }
            result.prefixPath += L"pfx";
        }
    }
    result.steamAppId = EnvironmentValue(L"SteamAppId");
    if (result.steamAppId.empty())
    {
        result.steamAppId = EnvironmentValue(L"STEAM_COMPAT_APP_ID");
    }

    result.display = result.kind == CompatibilityRuntimeKind::Proton ? L"Proton" : L"Wine";
    if (!result.wineVersion.empty())
    {
        result.display += L" (Wine " + result.wineVersion + L")";
    }
    return result;
}

std::wstring CompatibilityLaunchAdvice(const CompatibilityRuntimeInfo& runtime)
{
    if (!runtime.IsWine())
    {
        return {};
    }

    std::wstring advice = L" Running through " + runtime.display
        + L": keep the launcher, EFZ.EXE, netplay.dll, and SDL3.dll in the same folder "
          L"and start the launcher inside the same Wine/Proton prefix as the game.";
    if (runtime.kind == CompatibilityRuntimeKind::Proton)
    {
        advice += L" In Steam, set this launcher as the game executable so the child inherits the game's Proton prefix.";
    }
    return advice;
}

std::wstring ConfigureProcessDpiAwareness()
{
    HMODULE const user32 = GetModuleHandleW(L"user32.dll");
    using SetContextFn = BOOL(WINAPI*)(HANDLE);
    const auto setContext = user32 != nullptr
        ? reinterpret_cast<SetContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
        : nullptr;
    if (setContext != nullptr)
    {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is declared as -4 on supported SDKs.
        SetLastError(ERROR_SUCCESS);
        if (setContext(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))))
        {
            return L"Per-monitor v2";
        }
        if (GetLastError() == ERROR_ACCESS_DENIED)
        {
            return L"Already configured";
        }
    }

    using SetAwareFn = BOOL(WINAPI*)();
    const auto setAware = user32 != nullptr
        ? reinterpret_cast<SetAwareFn>(GetProcAddress(user32, "SetProcessDPIAware"))
        : nullptr;
    if (setAware != nullptr && setAware())
    {
        return L"System aware (legacy fallback)";
    }
    return L"Managed by SDL/runtime";
}

}
