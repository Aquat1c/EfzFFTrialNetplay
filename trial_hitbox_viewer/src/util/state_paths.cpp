#include "state_paths.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

namespace thv::paths
{
namespace
{

struct ResolvedRoot
{
    std::filesystem::path path;
    const char* source = "unresolved";
};

std::filesystem::path ReadEnvironmentPath(const wchar_t* const name)
{
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0u);
    if (required <= 1u)
    {
        return {};
    }
    std::vector<wchar_t> buffer(required, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        name, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0u || length >= buffer.size())
    {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer.data(), length));
}

std::filesystem::path ReadSystemTempPath()
{
    std::vector<wchar_t> buffer(512u, L'\0');
    for (;;)
    {
        const DWORD length = GetTempPathW(
            static_cast<DWORD>(buffer.size()), buffer.data());
        if (length == 0u)
        {
            return {};
        }
        if (length < buffer.size())
        {
            return std::filesystem::path(std::wstring(buffer.data(), length));
        }
        if (length >= 32768u)
        {
            return {};
        }
        buffer.resize(static_cast<std::size_t>(length) + 1u, L'\0');
    }
}

bool WriteProbe(const std::filesystem::path& candidate)
{
    if (candidate.empty())
    {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(candidate, ec);
    if (ec)
    {
        return false;
    }

    wchar_t probeName[96] = {};
    std::swprintf(
        probeName,
        std::size(probeName),
        L".trial_netplay_write_probe_%lu_%lu.tmp",
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetCurrentThreadId()));
    const std::filesystem::path probe = candidate / probeName;
    const HANDLE file = CreateFileW(
        probe.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    CloseHandle(file);
    DeleteFileW(probe.c_str());
    return true;
}

ResolvedRoot ResolveRoot()
{
    struct Candidate
    {
        std::filesystem::path path;
        const char* source;
    };
    std::array<Candidate, 5> candidates{};
    std::size_t count = 0u;

    const std::filesystem::path explicitRoot =
        ReadEnvironmentPath(L"TRIAL_NETPLAY_STATE_DIR");
    if (!explicitRoot.empty())
    {
        candidates[count++] = {explicitRoot, "TRIAL_NETPLAY_STATE_DIR"};
    }

    candidates[count++] = {GameDirectory(), "game directory"};

    const std::filesystem::path sessionIni =
        ReadEnvironmentPath(L"TRIAL_CASTER_SESSION_INI");
    if (!sessionIni.empty() && !sessionIni.parent_path().empty())
    {
        candidates[count++] = {sessionIni.parent_path() / L"game_state", "launcher session"};
    }

    const std::filesystem::path localAppData = ReadEnvironmentPath(L"LOCALAPPDATA");
    if (!localAppData.empty())
    {
        candidates[count++] = {localAppData / L"EFZ FF Netplay", "LocalAppData"};
    }

    std::filesystem::path temp = ReadEnvironmentPath(L"TEMP");
    if (temp.empty())
    {
        temp = ReadEnvironmentPath(L"TMP");
    }
    if (temp.empty())
    {
        temp = ReadSystemTempPath();
    }
    if (!temp.empty())
    {
        candidates[count++] = {temp / L"EFZ FF Netplay", "Temp"};
    }

    for (std::size_t index = 0u; index < count; ++index)
    {
        if (WriteProbe(candidates[index].path))
        {
            return {candidates[index].path, candidates[index].source};
        }
    }
    return {std::filesystem::path(L"."), "current directory fallback"};
}

const ResolvedRoot& Root()
{
    static const ResolvedRoot root = ResolveRoot();
    return root;
}

}

std::filesystem::path GameDirectory()
{
    std::vector<wchar_t> buffer(512u, L'\0');
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0u)
        {
            return std::filesystem::current_path();
        }
        if (length < buffer.size() - 1u)
        {
            return std::filesystem::path(
                std::wstring(buffer.data(), length)).parent_path();
        }
        if (buffer.size() >= 32768u)
        {
            return std::filesystem::current_path();
        }
        buffer.resize((std::min<std::size_t>)(buffer.size() * 2u, 32768u), L'\0');
    }
}

std::string PathUtf8(const std::filesystem::path& path) noexcept
{
    try
    {
        const std::wstring& wide = path.native();
        if (wide.empty())
        {
            return {};
        }
        const int required = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            wide.c_str(),
            static_cast<int>(wide.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (required <= 0)
        {
            return "(path is not valid UTF-16)";
        }
        std::string result(static_cast<std::size_t>(required), '\0');
        if (WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                wide.c_str(),
                static_cast<int>(wide.size()),
                result.data(),
                required,
                nullptr,
                nullptr) != required)
        {
            return "(path UTF-8 conversion failed)";
        }
        return result;
    }
    catch (...)
    {
        return "(path UTF-8 conversion failed)";
    }
}

const std::filesystem::path& MutableStateRoot()
{
    return Root().path;
}

const char* MutableStateRootSource()
{
    return Root().source;
}

}
