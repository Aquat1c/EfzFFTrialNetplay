#include "version_info.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <vector>

namespace trial_caster
{
namespace
{

std::wstring Win32VersionError(const DWORD error)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    std::wstring result = length != 0 && buffer != nullptr
        ? std::wstring(buffer, length)
        : L"Windows error " + std::to_wstring(error);
    if (buffer != nullptr)
    {
        LocalFree(buffer);
    }
    while (!result.empty()
        && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' '))
    {
        result.pop_back();
    }
    return result;
}

VersionQuery UnavailableVersion(const std::filesystem::path& path, const DWORD error)
{
    VersionQuery result{};
    result.error = L"Version information unavailable for " + path.filename().wstring()
        + L": " + Win32VersionError(error);
    return result;
}

}

std::wstring FormatBinaryVersion(const BinaryVersion& version)
{
    std::wstring result = std::to_wstring(version.major) + L"." + std::to_wstring(version.minor);
    if (version.patch != 0 || version.build != 0)
    {
        result += L"." + std::to_wstring(version.patch);
    }
    if (version.build != 0)
    {
        result += L"." + std::to_wstring(version.build);
    }
    return result;
}

VersionQuery QueryBinaryVersion(const std::filesystem::path& path)
{
    DWORD ignoredHandle = 0;
    SetLastError(ERROR_SUCCESS);
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignoredHandle);
    if (size == 0)
    {
        DWORD error = GetLastError();
        if (error == ERROR_SUCCESS)
        {
            error = ERROR_RESOURCE_DATA_NOT_FOUND;
        }
        return UnavailableVersion(path, error);
    }

    std::vector<std::uint8_t> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
    {
        return UnavailableVersion(path, GetLastError());
    }

    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedSize = 0;
    if (!VerQueryValueW(
            data.data(),
            L"\\",
            reinterpret_cast<void**>(&fixed),
            &fixedSize)
        || fixed == nullptr
        || fixedSize < sizeof(VS_FIXEDFILEINFO)
        || fixed->dwSignature != VS_FFI_SIGNATURE)
    {
        return UnavailableVersion(path, ERROR_RESOURCE_DATA_NOT_FOUND);
    }

    VersionQuery result{};
    result.available = true;
    result.value.major = static_cast<std::uint16_t>(HIWORD(fixed->dwFileVersionMS));
    result.value.minor = static_cast<std::uint16_t>(LOWORD(fixed->dwFileVersionMS));
    result.value.patch = static_cast<std::uint16_t>(HIWORD(fixed->dwFileVersionLS));
    result.value.build = static_cast<std::uint16_t>(LOWORD(fixed->dwFileVersionLS));
    result.display = FormatBinaryVersion(result.value);
    return result;
}

std::filesystem::path CurrentExecutablePath()
{
    std::vector<wchar_t> path(512u, L'\0');
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0)
        {
            return {};
        }
        if (length < path.size() - 1u)
        {
            return std::filesystem::path(std::wstring(path.data(), length));
        }
        path.resize(path.size() * 2u, L'\0');
    }
}

InstalledComponentVersions QueryInstalledComponentVersions()
{
    InstalledComponentVersions result{};
    const std::filesystem::path launcherPath = CurrentExecutablePath();
    if (launcherPath.empty())
    {
        result.launcher.error = L"Launcher version information is unavailable: executable path lookup failed";
        result.netplayDll.error = L"DLL version information is unavailable: launcher folder lookup failed";
        return result;
    }

    result.launcher = QueryBinaryVersion(launcherPath);
    result.netplayDll = QueryBinaryVersion(launcherPath.parent_path() / L"netplay.dll");
    return result;
}

}
