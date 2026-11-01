#pragma once

#include "caster_config.h"
#include "control_bindings.h"

#include <filesystem>
#include <string>

namespace trial_caster
{

enum class SessionMode
{
    Offline = 0,
    Host = 1,
    Client = 2,
    Spectator = 3,
};

enum class BootstrapMode
{
    Default = 0,
    DebugVsHuman = 1,
};

struct SessionDescriptor
{
    SessionMode mode = SessionMode::Offline;
    BootstrapMode bootstrap = BootstrapMode::Default;
    bool localIsP1 = true;
    bool isHost = false;
    std::uint16_t port = 7500;
    std::uint16_t peerPort = 0;
    std::uint8_t inputDelay = 1;
    std::uint8_t rollbackFrames = 6;
    std::uint8_t targetFps = 60;
    bool allowPracticeKeys = false;
    bool diagnosticLogging = false;
    std::uint32_t pingMs = 0;
    bool usedAutopunch = false;
    std::wstring peerIp;
    std::wstring peerName;
    std::wstring playerName;
    std::wstring sessionId;
    unsigned long launchIndex = 0;
    unsigned long casterPid = 0;
    bool requireRollback = false;
    ControlBindings controls = DefaultControlBindings();
};

struct SessionWriteResult
{
    bool ok = false;
    std::filesystem::path iniPath;
    std::filesystem::path launchDir;
    std::wstring sessionId;
    unsigned long launchIndex = 0;
};

SessionWriteResult WriteSessionFile(
    const std::filesystem::path& casterSessionDir,
    const SessionDescriptor& session);

void RecordLaunchOutcome(
    const std::filesystem::path& casterSessionDir,
    const SessionWriteResult& write,
    SessionMode mode,
    unsigned long efzPid,
    bool ok);

void DeleteSessionFile(const std::filesystem::path& gameDir);

}
