#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace trial_caster::util
{

std::wstring Format(const wchar_t* fmt, ...);
std::wstring Win32Error(DWORD code);
std::wstring WsaError(int code);

std::filesystem::path ExeDirectory();
std::filesystem::path ResolveWritableStateDirectory(const std::filesystem::path& launcherDir);
std::filesystem::path ResolvePath(const std::filesystem::path& base, const std::wstring& userPath);
std::filesystem::path ResolveAssetPath(
    const std::filesystem::path& gameDir,
    const std::filesystem::path& casterDir,
    const std::wstring& userPath,
    const wchar_t* fallbackName);
bool FileExists(const std::filesystem::path& path);
bool CopyIfExists(const std::filesystem::path& src, const std::filesystem::path& dst);

std::uint32_t ComputeEfzHash(const std::filesystem::path& exePath);

// Checks PE layout, not a file hash, so layout-preserving patches stay launchable.
bool ValidateEfzExecutableLayout(
    const std::filesystem::path& exePath,
    std::wstring* outError);

bool ReadNetplayDllCompatibility(
    const std::filesystem::path& dllPath,
    std::uint32_t* outCompatibilityId,
    std::wstring* outError);

// A generic Wine/native DirectDraw shim must never be forced ahead of Wine's builtin.
bool ValidateEfzDirectDrawWrapper(
    const std::filesystem::path& dllPath,
    std::wstring* outError);

std::wstring TrimConnectIp(std::wstring ip);
std::wstring NormalizeConnectIpv4(const std::wstring& ip);
bool IsValidConnectIpv4(const std::wstring& ip);
std::wstring NormalizeConnectHost(const std::wstring& host);
// Call from a worker, not while drawing the UI.
std::wstring ResolveConnectHostIpv4(
    const std::wstring& host,
    const std::atomic<bool>* cancel = nullptr);

struct ConnectDestination
{
    std::wstring host;
    std::uint16_t port = 0;
    bool explicitPort = false;
};

bool ParseConnectDestination(
    const std::wstring& text,
    std::uint16_t defaultPort,
    ConnectDestination* out);
std::wstring FormatConnectDestination(const ConnectDestination& destination);
std::wstring NormalizeConnectDestination(const std::wstring& text, std::uint16_t defaultPort);

std::vector<wchar_t> BuildChildEnvironmentBlock(const wchar_t* key, const std::wstring& value);
std::vector<wchar_t> BuildChildEnvironmentBlock(
    const std::vector<std::pair<std::wstring, std::wstring>>& replacements);

std::wstring EnsureNativeDirectDrawOverride(const std::wstring& existingOverrides);

}
