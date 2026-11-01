#include "caster_config.h"

#include "control_bindings.h"
#include "protocol.h"
#include "session_log.h"
#include "util.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace trial_caster
{

namespace
{

std::wstring ReadIniString(const std::wstring& section, const std::wstring& key, const std::wstring& def, const std::wstring& file)
{
    wchar_t buffer[1024] = {};
    GetPrivateProfileStringW(section.c_str(), key.c_str(), def.c_str(), buffer, static_cast<DWORD>(std::size(buffer)), file.c_str());
    return buffer;
}

int ReadIniInt(const std::wstring& section, const std::wstring& key, int def, const std::wstring& file)
{
    return static_cast<int>(GetPrivateProfileIntW(section.c_str(), key.c_str(), def, file.c_str()));
}

bool ReadIniBoolStrict(
    const std::wstring& section,
    const std::wstring& key,
    const bool def,
    const std::wstring& file)
{
    constexpr wchar_t kMissing[] = L"{TRIAL_NETPLAY_MISSING_BOOL}";
    const std::wstring value = ReadIniString(section, key, kMissing, file);
    if (value == kMissing)
    {
        return def;
    }
    if (value == L"0")
    {
        return false;
    }
    if (value == L"1")
    {
        return true;
    }
    SessionLog::Warn(
        L"config",
        L"Ignoring invalid boolean [%s] %s=%s",
        section.c_str(),
        key.c_str(),
        value.c_str());
    return def;
}

bool WriteIniString(const std::wstring& section, const std::wstring& key, const std::wstring& value, const std::wstring& file)
{
    return WritePrivateProfileStringW(
        section.c_str(), key.c_str(), value.c_str(), file.c_str()) != FALSE;
}

bool WriteIniInt(const std::wstring& section, const std::wstring& key, int value, const std::wstring& file)
{
    return WriteIniString(section, key, std::to_wstring(value), file);
}

std::wstring JoinRecentConnectIps(const std::vector<std::wstring>& ips)
{
    std::wstring joined;
    for (const std::wstring& ip : ips)
    {
        if (ip.empty())
        {
            continue;
        }
        if (!joined.empty())
        {
            joined += L';';
        }
        joined += ip;
    }
    return joined;
}

}

std::filesystem::path ResolveLauncherIniPath(
    const std::filesystem::path& launcherDir,
    const std::filesystem::path& writableStateDir)
{
    const std::filesystem::path stateDir = writableStateDir.empty()
        ? launcherDir
        : writableStateDir;
    const auto launcherIni = stateDir / L"trial_launcher.ini";
    if (std::filesystem::exists(launcherIni))
    {
        return launcherIni;
    }

    const std::filesystem::path candidates[] = {
        stateDir / L"trial_caster.ini",
        launcherDir / L"trial_launcher.ini",
        launcherDir / L"trial_caster.ini",
    };
    for (const std::filesystem::path& source : candidates)
    {
        if (source == launcherIni || !std::filesystem::exists(source))
        {
            continue;
        }
        std::error_code ec;
        std::filesystem::create_directories(stateDir, ec);
        ec.clear();
        std::filesystem::copy_file(
            source,
            launcherIni,
            std::filesystem::copy_options::overwrite_existing,
            ec);
        if (!ec)
        {
            SessionLog::Step(
                L"config",
                L"Migrated launcher settings %s -> %s",
                source.wstring().c_str(),
                launcherIni.wstring().c_str());
            break;
        }
    }
    return launcherIni;
}

ConfigStore::ConfigStore(std::filesystem::path iniPath)
    : iniPath_(std::move(iniPath))
{
}

void ConfigStore::ApplyDefaults(const std::filesystem::path& launcherDir)
{
    SessionLog::Step(L"config", L"Install root=%s (EFZ.EXE + netplay.dll + SDL3.dll)",
        launcherDir.wstring().c_str());
}

std::vector<std::wstring> ConfigStore::ParseRecentConnectIps(
    const std::wstring& raw,
    const std::uint16_t defaultPort)
{
    std::vector<std::wstring> out;
    std::wstringstream stream(raw);
    std::wstring token;
    while (std::getline(stream, token, L';'))
    {
        const std::wstring destination = util::NormalizeConnectDestination(token, defaultPort);
        if (destination.empty())
        {
            continue;
        }
        if (std::find(out.begin(), out.end(), destination) == out.end())
        {
            out.push_back(destination);
        }
        if (out.size() >= kMaxRecentConnectIps)
        {
            break;
        }
    }
    return out;
}

bool ConfigStore::Load()
{
    if (!std::filesystem::exists(iniPath_))
    {
        SessionLog::Step(L"config", L"Load skipped (no ini at %s)", iniPath_.wstring().c_str());
        return false;
    }

    const std::wstring file = iniPath_.wstring();
    auto& c = config_;
    c.playerName = ReadIniString(L"profile", L"name", c.playerName, file);
    c.location = ReadIniString(L"profile", L"location", c.location, file);
    c.useRollbackNetplay = true;
    const int configuredPort = ReadIniInt(L"net", L"port", c.port, file);
    if (configuredPort >= 1 && configuredPort <= 65535)
    {
        c.port = static_cast<std::uint16_t>(configuredPort);
    }
    else
    {
        SessionLog::Warn(
            L"config",
            L"Ignoring invalid saved connection port %d; using %u",
            configuredPort,
            static_cast<unsigned>(c.port));
    }
    c.inputDelay = static_cast<std::uint8_t>(
        std::clamp(ReadIniInt(L"net", L"input_delay", c.inputDelay, file), 0, 20));
    c.rollbackFrames = static_cast<std::uint8_t>(
        std::clamp(ReadIniInt(L"net", L"rollback_frames", c.rollbackFrames, file), 0, 20));
    const int configuredTargetFps = ReadIniInt(
        L"net",
        L"target_fps",
        proto::kDefaultTargetFps,
        file);
    if (configuredTargetFps == proto::kDefaultTargetFps
        || configuredTargetFps == proto::kMaximumTargetFps)
    {
        c.targetFps = static_cast<std::uint8_t>(configuredTargetFps);
    }
    else
    {
        c.targetFps = proto::kDefaultTargetFps;
        SessionLog::Warn(
            L"config",
            L"Ignoring unsupported target_fps=%d; using %u",
            configuredTargetFps,
            static_cast<unsigned>(c.targetFps));
    }
    c.allowPracticeKeys = ReadIniBoolStrict(
        L"net", L"allow_practice_keys", false, file);
    c.maxRetryCount = static_cast<std::uint8_t>(std::clamp(
        ReadIniInt(L"net", L"max_retry", c.maxRetryCount, file),
        1,
        proto::kMaxRetryCount));
    c.packetResendCount = static_cast<std::uint8_t>(std::clamp(
        ReadIniInt(L"net", L"packet_resend", c.packetResendCount, file),
        1,
        proto::kMaxOverlappedPacket));
    c.maxTimeoutMs = static_cast<std::uint16_t>(std::clamp(
        ReadIniInt(L"net", L"timeout_ms", c.maxTimeoutMs, file),
        250,
        60000));
    c.highPriority = ReadIniBoolStrict(
        L"launch", L"high_priority", c.highPriority, file);
    c.verifyExeHash = ReadIniBoolStrict(
        L"launch", L"verify_hash", c.verifyExeHash, file);
    c.diagnosticLogging = ReadIniBoolStrict(
        L"diagnostics", L"file_logging", false, file);
    c.uiScalePercent = static_cast<std::uint16_t>(
        std::clamp(ReadIniInt(L"ui", L"scale_percent", c.uiScalePercent, file), 75, 200));

    const std::wstring lastIpRaw = ReadIniString(L"net", L"last_connect_ip", c.lastConnectIp, file);
    const std::wstring normalizedLastIp = util::NormalizeConnectDestination(lastIpRaw, c.port);
    c.lastConnectIp = normalizedLastIp.empty() ? L"127.0.0.1" : normalizedLastIp;
    c.recentConnectIps = ParseRecentConnectIps(
        ReadIniString(L"net", L"recent_connect_ips", L"", file),
        c.port);

    LoadControlsFromCasterIni(file, &c.controls);

    SessionLog::Step(
        L"config",
        L"Loaded %s port=%u delay=%u target_fps=%u mode=rollback practice_keys=%s diagnostics=%s last_ip=%s",
        iniPath_.wstring().c_str(),
        static_cast<unsigned>(c.port),
        static_cast<unsigned>(c.inputDelay),
        static_cast<unsigned>(c.targetFps),
        c.allowPracticeKeys ? L"opt-in" : L"off",
        c.diagnosticLogging ? L"on" : L"off",
        c.lastConnectIp.c_str());
    return true;
}

bool ConfigStore::Save()
{
    std::error_code ec;
    if (!iniPath_.parent_path().empty())
    {
        std::filesystem::create_directories(iniPath_.parent_path(), ec);
    }
    if (ec)
    {
        SessionLog::Error(
            L"config",
            L"Couldn't create settings directory %s (error %d)",
            iniPath_.parent_path().wstring().c_str(),
            ec.value());
        return false;
    }
    const std::wstring file = iniPath_.wstring();
    auto& c = config_;
    if (c.port == 0u)
    {
        c.port = 7500u;
    }
    c.inputDelay = (std::min<std::uint8_t>)(c.inputDelay, 20u);
    c.rollbackFrames = (std::min<std::uint8_t>)(c.rollbackFrames, 20u);
    if (!proto::IsSupportedTargetFps(c.targetFps))
    {
        SessionLog::Error(
            L"config",
            L"Refusing to save unsupported target_fps=%u (supported: 60 or 64)",
            static_cast<unsigned>(c.targetFps));
        return false;
    }
    c.maxRetryCount = std::clamp<std::uint8_t>(
        c.maxRetryCount, 1u, proto::kMaxRetryCount);
    c.packetResendCount = std::clamp<std::uint8_t>(
        c.packetResendCount, 1u, proto::kMaxOverlappedPacket);
    c.maxTimeoutMs = std::clamp<std::uint16_t>(c.maxTimeoutMs, 250u, 60000u);
    c.uiScalePercent = std::clamp<std::uint16_t>(c.uiScalePercent, 75u, 200u);
    for (const trialnet::shortcuts::Descriptor& descriptor : trialnet::shortcuts::kDescriptors)
    {
        std::wstring& binding = c.controls.shortcuts[descriptor.action];
        binding = trialnet::shortcuts::NormalizeBindingToken(binding);
        if (binding.empty())
        {
            binding = descriptor.defaultBinding;
        }
    }
    std::wstring bindingError;
    if (!ValidateControlBindings(c.controls, &bindingError))
    {
        SessionLog::Error(
            L"config",
            L"Refusing to save ambiguous controls: %s",
            bindingError.c_str());
        return false;
    }
    bool ok = true;
    const auto writeString = [&](const wchar_t* section, const wchar_t* key, const std::wstring& value) {
        ok = WriteIniString(section, key, value, file) && ok;
    };
    const auto writeInt = [&](const wchar_t* section, const wchar_t* key, const int value) {
        ok = WriteIniInt(section, key, value, file) && ok;
    };
    writeString(L"profile", L"name", c.playerName);
    writeString(L"profile", L"location", c.location);
    writeString(L"profile", L"network", L"rollback");
    writeInt(L"net", L"use_rollback", 1);
    writeInt(L"net", L"port", c.port);
    writeInt(L"net", L"input_delay", c.inputDelay);
    writeInt(L"net", L"rollback_frames", c.rollbackFrames);
    writeInt(L"net", L"target_fps", c.targetFps);
    writeInt(L"net", L"allow_practice_keys", c.allowPracticeKeys ? 1 : 0);
    writeInt(L"net", L"max_retry", c.maxRetryCount);
    writeInt(L"net", L"packet_resend", c.packetResendCount);
    writeInt(L"net", L"timeout_ms", c.maxTimeoutMs);
    writeString(L"net", L"last_connect_ip", c.lastConnectIp);
    writeString(L"net", L"recent_connect_ips", JoinRecentConnectIps(c.recentConnectIps));
    writeInt(L"launch", L"high_priority", c.highPriority ? 1 : 0);
    writeInt(L"launch", L"verify_hash", c.verifyExeHash ? 1 : 0);
    writeInt(L"diagnostics", L"file_logging", c.diagnosticLogging ? 1 : 0);
    writeInt(L"ui", L"scale_percent", c.uiScalePercent);
    ok = SaveControlsToCasterIni(file, c.controls) && ok;
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, file.c_str());
    if (!ok)
    {
        SessionLog::Error(L"config", L"Failed to save one or more settings to %s", file.c_str());
        return false;
    }
    SessionLog::Step(L"config", L"Saved %s", iniPath_.wstring().c_str());
    return true;
}

void ConfigStore::EnsureRecentListContains(const std::wstring& destinationText)
{
    util::ConnectDestination destination{};
    if (!util::ParseConnectDestination(destinationText, config_.port, &destination))
    {
        return;
    }
    const std::wstring normalized = util::FormatConnectDestination(destination);
    if (normalized.empty())
    {
        return;
    }

    auto& list = config_.recentConnectIps;
    list.erase(
        std::remove_if(
            list.begin(),
            list.end(),
            [&destination, this](const std::wstring& entry) {
                util::ConnectDestination existing{};
                return util::ParseConnectDestination(entry, config_.port, &existing)
                    && existing.host == destination.host && existing.port == destination.port;
            }),
        list.end());
    list.insert(list.begin(), normalized);
    if (list.size() > kMaxRecentConnectIps)
    {
        list.resize(kMaxRecentConnectIps);
    }
}

void ConfigStore::SetLastConnectIp(const std::wstring& destination)
{
    const std::wstring normalized = util::NormalizeConnectDestination(destination, config_.port);
    if (normalized.empty())
    {
        return;
    }
    config_.lastConnectIp = normalized;
    Save();
}

void ConfigStore::RememberSuccessfulConnect(const std::wstring& destination)
{
    const std::wstring normalized = util::NormalizeConnectDestination(destination, config_.port);
    if (normalized.empty())
    {
        return;
    }
    config_.lastConnectIp = normalized;
    EnsureRecentListContains(normalized);
    Save();
    SessionLog::Step(L"config", L"Remembered outbound destination %s", normalized.c_str());
}

}
