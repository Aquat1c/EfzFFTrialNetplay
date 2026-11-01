#pragma once

#include "caster_config.h"
#include "game_launcher.h"
#include "net_session.h"
#include "session_file.h"

#include <atomic>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace trial_caster
{

enum class AppPhase
{
    Idle,
    Hosting,
    Connecting,
    ActionRequired,
    Ready,
    Launching,
    Error,
};

enum class ConnectionPrompt
{
    None,
    HostAlreadyPlaying,
    HostStarting,
    HostNotPlaying,
};

struct UiSnapshot
{
    AppPhase phase = AppPhase::Idle;
    std::wstring status;
    std::wstring peerIp;
    bool localIsP1 = true;
    bool isHost = false;
    std::uint32_t exeHash = 0;
    std::uint32_t latencyMs = 0;
    std::uint8_t readyInputDelay = 1;
    std::uint8_t readyTargetFps = 60;
    ConnectionMethod connectionMethod = ConnectionMethod::Direct;
    bool hostLaunchGoReceived = false;
    ConnectionPrompt connectionPrompt = ConnectionPrompt::None;
    std::wstring promptDestination;
    std::wstring launcherVersion = L"Unavailable";
    std::wstring netplayDllVersion = L"Unavailable";
    std::wstring compatibilityRuntime = L"Windows";
    std::deque<std::wstring> log;
};

class CasterApp
{
public:
    explicit CasterApp(ConfigStore& config);
    ~CasterApp();

    UiSnapshot Snapshot() const;
    void AppendLog(const std::wstring& line);

    void StartHost();
    void StartConnect(const std::wstring& targetIp);
    void LaunchSpectator(const std::wstring& targetIp);
    void WatchAlreadyStartedMatch();
    void WaitForSpectatorMatch();
    void JoinSpectatorHostAsPlayer();
    void DismissConnectionPrompt();
    void LaunchOffline();
    void LaunchReady();
    void CancelNet();

    ConfigStore& Config();

private:
    void SetPhase(AppPhase phase, const std::wstring& status);
    void NetThreadHost();
    void NetThreadConnect(
        std::wstring targetIp,
        std::uint16_t targetPort,
        std::wstring destination);
    void NetThreadSpectatorProbe(
        std::wstring targetIp,
        std::uint16_t targetPort,
        std::wstring destination,
        bool waitForMatch);
    void NetThreadWaitLaunchGo();
    void LaunchSpectatorNow(
        const std::wstring& targetIp,
        std::uint16_t targetPort,
        const std::wstring& destination);
    void JoinWorkers();
    void StartLaunchAsync(
        SessionMode mode,
        BootstrapMode bootstrap,
        std::optional<NetSessionInfo> netInfo,
        bool sendLaunchGo);
    LaunchResult PrepareAndLaunch(
        SessionMode mode,
        BootstrapMode bootstrap,
        const std::optional<NetSessionInfo>& netInfo);

    ConfigStore& config_;
    GameLauncher launcher_;
    NetSession net_;

    mutable std::mutex mutex_;
    UiSnapshot ui_{};
    std::thread netWorker_;
    std::thread waitWorker_;
    std::thread launchWorker_;
    std::atomic<bool> cancelNet_{false};
    std::atomic<bool> hostLaunchGoReceived_{false};
    std::optional<NetSessionInfo> readyNet_;
    unsigned long launchCounter_ = 0;
};

}
