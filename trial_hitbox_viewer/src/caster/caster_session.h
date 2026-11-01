#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace thv::session
{

enum class LauncherMode
{
    Offline,
    Host,
    Client,
    Spectator,
};

enum class BootstrapMode
{
    Default = 0,
    DebugVsHuman = 1,
};

struct LauncherSession
{
    LauncherMode mode = LauncherMode::Offline;
    BootstrapMode bootstrap = BootstrapMode::Default;
    bool localIsP1 = true;
    bool isHost = false;
    bool ready = false;
    std::uint16_t port = 7500;
    // Destination port on the peer; 0 = same as `port`.
    std::uint16_t peerPort = 0;
    std::uint8_t inputDelay = 1;
    std::uint8_t rollbackFrames = 6;
    // Immutable and host-authoritative; only 60 or 64.
    std::uint8_t targetFps = 60;
    std::uint32_t pingMs = 0;
    bool usedAutopunch = false;
    // Online F4/F5 need both players' opt-in; immutable for the session.
    bool allowPracticeKeys = false;
    std::uint32_t compatibilityId = 0;
    std::wstring peerIp;
    std::wstring peerName;
    std::wstring playerName;
    std::wstring sessionId;
    unsigned long launchIndex = 0;
    unsigned long casterPid = 0;
    bool requireRollback = false;
};

std::filesystem::path ResolveSessionIniPath();

bool DiagnosticFileLoggingRequested();

bool TryLoadSession(LauncherSession* out, std::filesystem::path* outIniPath = nullptr);

void ApplyToCasterBridge(const LauncherSession& session);

const LauncherSession* GetCachedSession();

}
