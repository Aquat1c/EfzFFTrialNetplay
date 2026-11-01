#pragma once

#include "control_bindings.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace trial_caster
{

struct CasterConfig
{
    std::wstring playerName = L"Player";
    std::wstring location;
    bool useRollbackNetplay = true;
    std::wstring lastConnectIp = L"127.0.0.1";
    std::vector<std::wstring> recentConnectIps;
    std::uint16_t port = 7500;
    std::uint8_t inputDelay = 1;
    std::uint8_t rollbackFrames = 6;
    // Match speed is host-authoritative and immutable after LaunchGo.
    std::uint8_t targetFps = 60;
    bool allowPracticeKeys = false;
    std::uint8_t maxRetryCount = 99;
    std::uint8_t packetResendCount = 9;
    std::uint16_t maxTimeoutMs = 5000;
    bool highPriority = false;
    bool verifyExeHash = false;
    bool diagnosticLogging = false;
    std::uint16_t uiScalePercent = 100;
    // Must be populated even when no INI exists (clean install).
    ControlBindings controls = DefaultControlBindings();
};

class ConfigStore
{
public:
    explicit ConfigStore(std::filesystem::path iniPath);

    const CasterConfig& Get() const { return config_; }
    CasterConfig& Mutable() { return config_; }

    void ApplyDefaults(const std::filesystem::path& launcherDir);
    bool Load();
    bool Save();

    void SetLastConnectIp(const std::wstring& ip);
    void RememberSuccessfulConnect(const std::wstring& ip);

    std::filesystem::path IniPath() const { return iniPath_; }

private:
    static constexpr std::size_t kMaxRecentConnectIps = 8;

    void EnsureRecentListContains(const std::wstring& destination);
    static std::vector<std::wstring> ParseRecentConnectIps(
        const std::wstring& raw,
        std::uint16_t defaultPort);

    std::filesystem::path iniPath_;
    CasterConfig config_{};
};

std::filesystem::path ResolveLauncherIniPath(
    const std::filesystem::path& launcherDir,
    const std::filesystem::path& writableStateDir = {});

}
