#include "caster_app.h"

#include "compatibility_runtime.h"
#include "session_file.h"
#include "session_log.h"
#include "trial_compatibility.h"
#include "util.h"
#include "version_info.h"

#include <mmsystem.h>

#include <algorithm>
#include <optional>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace trial_caster
{
namespace
{

constexpr DWORD kLaunchSyncLeadMs = proto::kLaunchSyncLeadMs;

bool ValidateLocalNetplayDll(std::wstring* const outError)
{
    const auto dllPath = util::ExeDirectory() / L"netplay.dll";
    std::uint32_t dllCompatibility = 0u;
    std::wstring inspectError;
    if (!util::ReadNetplayDllCompatibility(dllPath, &dllCompatibility, &inspectError))
    {
        SessionLog::Error(L"compat", L"Game module inspection failed: %s", inspectError.c_str());
        if (outError != nullptr)
        {
            *outError = L"Online play files are missing or damaged. Update the launcher and game files together.";
        }
        return false;
    }
    if (dllCompatibility != trialnet::compat::kCompatibilityId)
    {
        SessionLog::Error(
            L"compat",
            L"Launcher/game module mismatch launcher=0x%08X module=0x%08X",
            trialnet::compat::kCompatibilityId,
            dllCompatibility);
        if (outError != nullptr)
        {
            *outError = L"Launcher and game files are from different releases. Update them together.";
        }
        return false;
    }
    return true;
}

void WaitUntilLaunchAt(
    const std::uint32_t launchAtMs,
    const std::atomic<bool>* const cancel = nullptr)
{
    if (launchAtMs == 0u)
    {
        return;
    }

    for (;;)
    {
        if (cancel != nullptr && cancel->load())
        {
            break;
        }
        const std::uint32_t now = timeGetTime();
        if (static_cast<std::int32_t>(now - launchAtMs) >= 0)
        {
            break;
        }
        const DWORD remaining = launchAtMs - now;
        Sleep((std::min<DWORD>)(remaining, 10u));
    }
}

const wchar_t* PhaseLabel(const AppPhase phase)
{
    switch (phase)
    {
    case AppPhase::Idle:
        return L"Idle";
    case AppPhase::Hosting:
        return L"Hosting";
    case AppPhase::Connecting:
        return L"Connecting";
    case AppPhase::ActionRequired:
        return L"Action required";
    case AppPhase::Ready:
        return L"Ready";
    case AppPhase::Launching:
        return L"Launching";
    case AppPhase::Error:
        return L"Error";
    default:
        return L"?";
    }
}

const wchar_t* NetResultText(NetResult result)
{
    switch (result)
    {
    case NetResult::Ok:
        return L"Connected";
    case NetResult::Cancelled:
        return L"Cancelled";
    case NetResult::Timeout:
        return L"Connection timed out";
    case NetResult::SocketError:
        return L"Couldn't open a network connection";
    case NetResult::InvalidDestination:
        return L"Enter a valid host address";
    case NetResult::VersionMismatch:
        return L"The other player is using a different release. Update both installations.";
    case NetResult::AlreadyInPlay:
        return L"Host already in play";
    case NetResult::Denied:
        return L"Connection denied";
    case NetResult::RollbackRequired:
        return L"The other player's online play files are outdated. Update both installations.";
    case NetResult::NetModeMismatch:
        return L"The other player is using an older online mode. Update both installations.";
    case NetResult::HostWaiting:
        return L"The host is waiting for a player";
    case NetResult::HostStarting:
        return L"The host has two players and is preparing the match";
    case NetResult::InvalidConfiguration:
        return L"The match settings are invalid or incompatible. Update both installations.";
    default:
        return L"Unknown error";
    }
}

}

CasterApp::CasterApp(ConfigStore& config)
    : config_(config)
    , net_(config.Get())
{
    net_.SetLogger([this](const std::wstring& line) { AppendLog(line); });
    ui_.compatibilityRuntime = DetectCompatibilityRuntime().display;
    const InstalledComponentVersions versions = QueryInstalledComponentVersions();
    ui_.launcherVersion = versions.launcher.display;
    ui_.netplayDllVersion = versions.netplayDll.display;
    if (versions.launcher.available)
    {
        SessionLog::Step(L"version", L"Launcher version %s", ui_.launcherVersion.c_str());
    }
    else
    {
        SessionLog::Warn(L"version", L"%s", versions.launcher.error.c_str());
    }
    if (versions.netplayDll.available)
    {
        SessionLog::Step(L"version", L"Netplay DLL version %s", ui_.netplayDllVersion.c_str());
    }
    else
    {
        SessionLog::Warn(L"version", L"%s", versions.netplayDll.error.c_str());
    }
    SetPhase(AppPhase::Idle, L"Ready");
}

CasterApp::~CasterApp()
{
    CancelNet();
}

ConfigStore& CasterApp::Config()
{
    return config_;
}

UiSnapshot CasterApp::Snapshot() const
{
    std::lock_guard lock(mutex_);
    return ui_;
}

void CasterApp::AppendLog(const std::wstring& line)
{
    SessionLog::WriteLine(line);
    std::lock_guard lock(mutex_);
    ui_.log.push_back(line);
    while (ui_.log.size() > 200)
    {
        ui_.log.pop_front();
    }
}

void CasterApp::SetPhase(const AppPhase phase, const std::wstring& status)
{
    SessionLog::Step(L"app", L"phase -> %s | %s", PhaseLabel(phase), status.c_str());
    std::lock_guard lock(mutex_);
    ui_.phase = phase;
    ui_.status = status;
}

void CasterApp::JoinWorkers()
{
    if (netWorker_.joinable())
    {
        netWorker_.join();
    }
    if (waitWorker_.joinable())
    {
        waitWorker_.join();
    }
    if (launchWorker_.joinable())
    {
        launchWorker_.join();
    }
}

void CasterApp::CancelNet()
{
    SessionLog::Step(L"net", L"CancelNet");
    cancelNet_.store(true);
    net_.Close();
    JoinWorkers();
    cancelNet_.store(false);
    hostLaunchGoReceived_.store(false);
    readyNet_.reset();
    {
        std::lock_guard lock(mutex_);
        ui_.hostLaunchGoReceived = false;
        ui_.connectionPrompt = ConnectionPrompt::None;
        ui_.promptDestination.clear();
    }
    SetPhase(AppPhase::Idle, L"Cancelled");
}

void CasterApp::StartHost()
{
    SessionLog::Step(L"net", L"StartHost requested");
    std::wstring bindingError;
    if (!ValidateControlBindings(config_.Get().controls, &bindingError))
    {
        const std::wstring message = L"Fix the Controls page before hosting: " + bindingError;
        AppendLog(message);
        SetPhase(AppPhase::Error, message);
        return;
    }
    std::wstring compatibilityError;
    if (!ValidateLocalNetplayDll(&compatibilityError))
    {
        AppendLog(compatibilityError);
        SetPhase(AppPhase::Error, compatibilityError);
        return;
    }
    CancelNet();
    hostLaunchGoReceived_.store(false);
    config_.Save();
    SetPhase(AppPhase::Hosting, L"Waiting for opponent...");
    netWorker_ = std::thread([this]() { NetThreadHost(); });
}

void CasterApp::StartConnect(const std::wstring& targetIp)
{
    util::ConnectDestination target{};
    if (!util::ParseConnectDestination(targetIp, config_.Get().port, &target))
    {
        SetPhase(AppPhase::Error, L"Enter a valid address, with an optional :port");
        return;
    }
    const std::wstring destination = util::FormatConnectDestination(target);

    std::wstring bindingError;
    if (!ValidateControlBindings(config_.Get().controls, &bindingError))
    {
        const std::wstring message = L"Fix the Controls page before joining: " + bindingError;
        AppendLog(message);
        SetPhase(AppPhase::Error, message);
        return;
    }

    std::wstring compatibilityError;
    if (!ValidateLocalNetplayDll(&compatibilityError))
    {
        AppendLog(compatibilityError);
        SetPhase(AppPhase::Error, compatibilityError);
        return;
    }

    SessionLog::Step(
        L"net",
        L"StartConnect requested peer=%s:%u destination=%s",
        target.host.c_str(),
        static_cast<unsigned>(target.port),
        destination.c_str());
    CancelNet();
    hostLaunchGoReceived_.store(false);
    config_.SetLastConnectIp(destination);
    SetPhase(AppPhase::Connecting, L"Connecting...");
    netWorker_ = std::thread([this, target, destination]() {
        NetThreadConnect(target.host, target.port, destination);
    });
}

void CasterApp::LaunchSpectator(const std::wstring& targetIp)
{
    util::ConnectDestination target{};
    if (!util::ParseConnectDestination(targetIp, config_.Get().port, &target))
    {
        SetPhase(AppPhase::Error, L"Enter a valid address, with an optional :port");
        return;
    }
    const std::wstring destination = util::FormatConnectDestination(target);
    std::wstring compatibilityError;
    if (!ValidateLocalNetplayDll(&compatibilityError))
    {
        AppendLog(compatibilityError);
        SetPhase(AppPhase::Error, compatibilityError);
        return;
    }

    CancelNet();
    config_.SetLastConnectIp(destination);
    config_.Save();
    SessionLog::Step(
        L"spectator",
        L"presence check requested peer=%s:%u destination=%s",
        target.host.c_str(),
        static_cast<unsigned>(target.port),
        destination.c_str());
    SetPhase(AppPhase::Connecting, L"Looking for an active match...");
    netWorker_ = std::thread([this, target, destination]() {
        NetThreadSpectatorProbe(target.host, target.port, destination, false);
    });
}

void CasterApp::LaunchSpectatorNow(
    const std::wstring& ip,
    const std::uint16_t targetPort,
    const std::wstring& destination)
{
    const std::wstring resolvedIp = util::ResolveConnectHostIpv4(ip, &cancelNet_);
    if (resolvedIp.empty())
    {
        SetPhase(AppPhase::Error, L"The match address could not be resolved");
        SessionLog::Warn(
            L"spectator",
            L"launcher spectator address resolution failed host=%s destination=%s",
            ip.c_str(),
            destination.c_str());
        return;
    }
    config_.SetLastConnectIp(destination);
    config_.RememberSuccessfulConnect(destination);
    config_.Save();
    NetSessionInfo info{};
    info.isHost = false;
    info.localIsP1 = true;
    info.peerIp = resolvedIp;
    info.peerPort = targetPort;
    info.connectionMethod = ConnectionMethod::Direct;
    SessionLog::Step(
        L"spectator",
        L"launcher spectator request peer=%s:%u direct-first assisted-fallback",
        resolvedIp.c_str(),
        static_cast<unsigned>(info.peerPort));
    AppendLog(L"Opening the match viewer. Connection assistance will be used automatically if needed.");
    StartLaunchAsync(
        SessionMode::Spectator,
        BootstrapMode::Default,
        info,
        false);
}

void CasterApp::WatchAlreadyStartedMatch()
{
    std::wstring destination;
    {
        std::lock_guard lock(mutex_);
        if (ui_.connectionPrompt != ConnectionPrompt::HostAlreadyPlaying)
        {
            return;
        }
        destination = ui_.promptDestination;
    }
    util::ConnectDestination target{};
    if (!util::ParseConnectDestination(destination, config_.Get().port, &target))
    {
        SetPhase(AppPhase::Error, L"The saved match address is no longer valid");
        return;
    }
    SessionLog::Step(L"spectator", L"accepted already-started match offer=%s", destination.c_str());
    CancelNet();
    LaunchSpectatorNow(target.host, target.port, destination);
}

void CasterApp::WaitForSpectatorMatch()
{
    std::wstring destination;
    {
        std::lock_guard lock(mutex_);
        if (ui_.connectionPrompt != ConnectionPrompt::HostNotPlaying
            && ui_.connectionPrompt != ConnectionPrompt::HostStarting)
        {
            return;
        }
        destination = ui_.promptDestination;
    }
    util::ConnectDestination target{};
    if (!util::ParseConnectDestination(destination, config_.Get().port, &target))
    {
        SetPhase(AppPhase::Error, L"The saved match address is no longer valid");
        return;
    }
    SessionLog::Step(L"spectator", L"waiting for next match destination=%s", destination.c_str());
    CancelNet();
    SetPhase(AppPhase::Connecting, L"Waiting for the host to start a match...");
    netWorker_ = std::thread([this, target, destination]() {
        NetThreadSpectatorProbe(target.host, target.port, destination, true);
    });
}

void CasterApp::JoinSpectatorHostAsPlayer()
{
    std::wstring destination;
    {
        std::lock_guard lock(mutex_);
        if (ui_.connectionPrompt != ConnectionPrompt::HostNotPlaying)
        {
            return;
        }
        destination = ui_.promptDestination;
    }
    SessionLog::Step(L"net", L"spectator chose to join waiting host=%s", destination.c_str());
    StartConnect(destination);
}

void CasterApp::DismissConnectionPrompt()
{
    SessionLog::Step(L"net", L"dismissed connection choice");
    CancelNet();
    SetPhase(AppPhase::Idle, L"Ready");
}

void CasterApp::NetThreadHost()
{
    NetSessionInfo info{};
    const NetResult result = net_.HostHandshake(&info);
    if (cancelNet_.load())
    {
        return;
    }

    if (result != NetResult::Ok)
    {
        SetPhase(AppPhase::Error, NetResultText(result));
        return;
    }

    readyNet_ = info;
    {
        std::lock_guard lock(mutex_);
        ui_.phase = AppPhase::Ready;
        ui_.status = L"Opponent connected — launch when ready";
        ui_.peerIp = info.peerIp;
        ui_.localIsP1 = info.localIsP1;
        ui_.isHost = true;
        ui_.latencyMs = info.averageLatencyMs;
        ui_.readyInputDelay = info.inputDelay;
        ui_.readyTargetFps = info.targetFps;
        ui_.connectionMethod = info.connectionMethod;
    }

    // Keep receiving so a later connector gets the occupied answer, not a timeout.
    for (;;)
    {
        if (cancelNet_.load())
        {
            return;
        }
        AppPhase phase = AppPhase::Idle;
        {
            std::lock_guard lock(mutex_);
            phase = ui_.phase;
        }
        if (phase != AppPhase::Ready && phase != AppPhase::Launching)
        {
            return;
        }
        const NetResult serviceResult = net_.ServiceOccupiedHost(100u);
        if (serviceResult == NetResult::SocketError)
        {
            if (!cancelNet_.load())
            {
                SessionLog::Warn(L"net", L"occupied host listener stopped after socket error");
            }
            return;
        }
        if (serviceResult == NetResult::Denied)
        {
            return;
        }
    }
}

void CasterApp::NetThreadConnect(
    std::wstring targetIp,
    const std::uint16_t targetPort,
    std::wstring destination)
{
    NetSessionInfo info{};
    const NetResult result = net_.ConnectHandshake(targetIp, &info, targetPort);
    if (cancelNet_.load())
    {
        return;
    }

    if (result == NetResult::AlreadyInPlay)
    {
        SessionLog::Step(
            L"net",
            L"normal connect reached active match; offering read-only spectator peer=%s",
            targetIp.c_str());
        std::lock_guard lock(mutex_);
        ui_.phase = AppPhase::ActionRequired;
        ui_.status = L"This match has already started";
        ui_.connectionPrompt = ConnectionPrompt::HostAlreadyPlaying;
        ui_.promptDestination = destination;
        return;
    }

    if (result == NetResult::HostStarting)
    {
        SessionLog::Step(
            L"net",
            L"normal connect reached occupied launcher; offering wait-to-watch peer=%s",
            targetIp.c_str());
        std::lock_guard lock(mutex_);
        ui_.phase = AppPhase::ActionRequired;
        ui_.status = L"Two players are connected, but the match has not started";
        ui_.connectionPrompt = ConnectionPrompt::HostStarting;
        ui_.promptDestination = destination;
        return;
    }

    if (result != NetResult::Ok)
    {
        SetPhase(AppPhase::Error, NetResultText(result));
        return;
    }

    readyNet_ = info;
    config_.RememberSuccessfulConnect(destination);
    {
        std::lock_guard lock(mutex_);
        ui_.phase = AppPhase::Ready;
        ui_.status = L"Connected — waiting for the host to start";
        ui_.peerIp = destination;
        ui_.localIsP1 = info.localIsP1;
        ui_.isHost = false;
        ui_.latencyMs = info.averageLatencyMs;
        ui_.readyInputDelay = info.inputDelay;
        ui_.readyTargetFps = info.targetFps;
        ui_.connectionMethod = info.connectionMethod;
        ui_.hostLaunchGoReceived = false;
    }

    waitWorker_ = std::thread([this]() { NetThreadWaitLaunchGo(); });
}

void CasterApp::NetThreadSpectatorProbe(
    std::wstring targetIp,
    const std::uint16_t targetPort,
    std::wstring destination,
    const bool waitForMatch)
{
    bool announcedWait = false;
    bool announcedStarting = false;
    bool continueWaiting = waitForMatch;
    for (;;)
    {
        const NetResult result = net_.ProbeSpectatorHost(targetIp, targetPort);
        if (cancelNet_.load())
        {
            return;
        }

        if (result == NetResult::AlreadyInPlay)
        {
            SessionLog::Step(
                L"spectator",
                L"active match discovered peer=%s wait_mode=%d",
                targetIp.c_str(),
                waitForMatch ? 1 : 0);
            LaunchSpectatorNow(targetIp, targetPort, destination);
            return;
        }

        if (result == NetResult::HostWaiting)
        {
            if (!continueWaiting)
            {
                SessionLog::Step(
                    L"spectator",
                    L"host has no active match; offering wait-or-join peer=%s",
                    targetIp.c_str());
                std::lock_guard lock(mutex_);
                ui_.phase = AppPhase::ActionRequired;
                ui_.status = L"The host has not started a match";
                ui_.connectionPrompt = ConnectionPrompt::HostNotPlaying;
                ui_.promptDestination = destination;
                return;
            }
            if (!announcedWait)
            {
                announcedWait = true;
                AppendLog(L"The host is waiting for a player. The match viewer will open automatically when play starts.");
            }
        }
        else if (result == NetResult::HostStarting)
        {
            continueWaiting = true;
            if (!announcedStarting)
            {
                announcedStarting = true;
                AppendLog(L"Both players are connected. Waiting for the game to start before opening the match viewer.");
                SetPhase(AppPhase::Connecting, L"Players connected — waiting for the match to start...");
            }
        }
        else if (!continueWaiting || result != NetResult::Timeout)
        {
            SetPhase(AppPhase::Error, NetResultText(result));
            return;
        }

        for (int tenth = 0; tenth < 10; ++tenth)
        {
            if (cancelNet_.load())
            {
                return;
            }
            Sleep(100);
        }
    }
}

void CasterApp::NetThreadWaitLaunchGo()
{
    std::uint8_t delay = readyNet_.has_value() ? readyNet_->inputDelay : config_.Get().inputDelay;
    std::uint8_t targetFps = readyNet_.has_value()
        ? readyNet_->targetFps
        : config_.Get().targetFps;
    std::uint32_t launchAtMs = 0;
    const NetResult result = net_.WaitForLaunchGo(&delay, &targetFps, &launchAtMs, 300000);
    if (cancelNet_.load())
    {
        return;
    }

    if (result != NetResult::Ok)
    {
        SetPhase(AppPhase::Error, NetResultText(result));
        return;
    }

    if (readyNet_.has_value())
    {
        readyNet_->inputDelay = delay;
        readyNet_->targetFps = targetFps;
    }

    hostLaunchGoReceived_.store(true);
    {
        std::lock_guard lock(mutex_);
        ui_.hostLaunchGoReceived = true;
        ui_.readyInputDelay = delay;
        ui_.readyTargetFps = targetFps;
        ui_.status = L"Match starting...";
    }
    AppendLog(L"Host launch-go received — auto-launching EFZ at scheduled time");

    const std::uint32_t leadMs = launchAtMs != 0u ? launchAtMs : kLaunchSyncLeadMs;
    WaitUntilLaunchAt(timeGetTime() + leadMs, &cancelNet_);

    if (cancelNet_.load() || !readyNet_.has_value())
    {
        return;
    }

    const SessionMode mode = readyNet_->isHost ? SessionMode::Host : SessionMode::Client;
    StartLaunchAsync(mode, BootstrapMode::Default, readyNet_, false);
}

LaunchResult CasterApp::PrepareAndLaunch(
    const SessionMode mode,
    const BootstrapMode bootstrap,
    const std::optional<NetSessionInfo>& netInfo)
{
    SessionLog::Step(
        L"launch",
        L"PrepareAndLaunch mode=%d bootstrap=%d netInfo=%d",
        static_cast<int>(mode),
        static_cast<int>(bootstrap),
        netInfo.has_value() ? 1 : 0);
    LaunchResult result{};
    const auto& cfg = config_.Get();
    if (mode != SessionMode::Spectator)
    {
        std::wstring bindingError;
        if (!ValidateControlBindings(cfg.controls, &bindingError))
        {
            result.message = L"Controls contain ambiguous bindings: " + bindingError;
            SessionLog::Error(L"launch", L"%s", result.message.c_str());
            return result;
        }
    }
    const auto casterDir = util::ExeDirectory();
    const auto efzExe = casterDir / L"EFZ.EXE";

    if (!util::FileExists(efzExe))
    {
        result.message = L"The game executable is missing from the launcher folder.";
        return result;
    }
    std::wstring executableError;
    if (!util::ValidateEfzExecutableLayout(efzExe, &executableError))
    {
        result.message = executableError;
        SessionLog::Error(L"launch", L"%s", result.message.c_str());
        return result;
    }

    const auto gameDir = efzExe.parent_path();
    const auto modDll = casterDir / L"netplay.dll";
    const auto sdlDll = casterDir / L"SDL3.dll";
    const auto directDrawDll = casterDir / L"ddraw.dll";
    if (!util::FileExists(modDll))
    {
        result.message = L"Online play files are missing. Reinstall or update the package.";
        return result;
    }
    std::wstring compatibilityError;
    if (!ValidateLocalNetplayDll(&compatibilityError))
    {
        result.message = compatibilityError;
        SessionLog::Error(L"launch", L"%s", result.message.c_str());
        return result;
    }
    if (!util::FileExists(sdlDll))
    {
        result.message = L"Controller support files are missing. Reinstall or update the package.";
        return result;
    }
    if (!util::FileExists(directDrawDll))
    {
        result.message = L"Display compatibility file ddraw.dll is missing. Reinstall the complete package beside EFZ.EXE.";
        return result;
    }
    std::wstring directDrawError;
    if (!util::ValidateEfzDirectDrawWrapper(directDrawDll, &directDrawError))
    {
        result.message = directDrawError;
        SessionLog::Error(L"launch", L"%s", result.message.c_str());
        return result;
    }

    if (cfg.verifyExeHash)
    {
        const std::uint32_t hash = util::ComputeEfzHash(efzExe);
        {
            std::lock_guard lock(mutex_);
            ui_.exeHash = hash;
        }
        AppendLog(util::Format(L"EFZ hash: 0x%08X", hash));
    }

    AppendLog(L"Installation check passed (EFZ.EXE + netplay.dll + SDL3.dll).");

    const unsigned long launchIndex = ++launchCounter_;
    const unsigned long casterPid = GetCurrentProcessId();

    SessionDescriptor session{};
    session.mode = mode;
    session.bootstrap = bootstrap;
    session.requireRollback = mode == SessionMode::Host || mode == SessionMode::Client;
    session.controls = cfg.controls;
    session.port = cfg.port;
    session.playerName = cfg.playerName;
    session.inputDelay = cfg.inputDelay;
    session.rollbackFrames = cfg.rollbackFrames;
    session.targetFps = cfg.targetFps;
    session.allowPracticeKeys = mode == SessionMode::Offline
        ? cfg.allowPracticeKeys
        : false;
    session.diagnosticLogging = cfg.diagnosticLogging;
    session.launchIndex = launchIndex;
    session.casterPid = casterPid;
    session.sessionId = util::Format(L"%lu_%llu_%lu", casterPid, GetTickCount64(), launchIndex);
    if (netInfo.has_value())
    {
        session.localIsP1 = netInfo->localIsP1;
        session.isHost = netInfo->isHost;
        session.peerIp = netInfo->peerIp;
        session.peerPort = netInfo->peerPort;
        session.peerName = netInfo->peerName;
        session.pingMs = netInfo->averageLatencyMs;
        session.inputDelay = netInfo->inputDelay;
        if (mode == SessionMode::Host || mode == SessionMode::Client)
        {
            session.targetFps = netInfo->targetFps;
        }
        session.usedAutopunch = netInfo->connectionMethod == ConnectionMethod::AutoPunch;
        if (mode == SessionMode::Host || mode == SessionMode::Client)
        {
            session.allowPracticeKeys = netInfo->allowPracticeKeys;
        }
    }
    else
    {
        session.localIsP1 = true;
        session.isHost = false;
    }

    const auto casterSessionDir = SessionLog::SessionDirectory();
    const SessionWriteResult sessionWrite = WriteSessionFile(casterSessionDir, session);
    if (!sessionWrite.ok)
    {
        result.message = L"Couldn't prepare the game session. Check folder permissions and try again.";
        SessionLog::Error(L"launch", L"%s", result.message.c_str());
        return result;
    }

    std::vector<std::filesystem::path> dlls;
    const auto injectedSdl = gameDir / L"SDL3.dll";
    const auto injectedDll = gameDir / L"netplay.dll";
    if (util::FileExists(injectedSdl))
    {
        dlls.push_back(injectedSdl);
    }
    if (util::FileExists(injectedDll))
    {
        dlls.push_back(injectedDll);
    }

    std::function<void()> beforeResume;
    if (mode == SessionMode::Host || mode == SessionMode::Client)
    {
        beforeResume = [this]()
        {
            SessionLog::Step(
                L"network",
                L"Releasing launcher UDP endpoint for the resumed game process");
            net_.Close();
        };
    }

    result = launcher_.Launch(
        efzExe,
        dlls,
        cfg.highPriority,
        sessionWrite.iniPath,
        &cancelNet_,
        beforeResume);
    result.sessionId = sessionWrite.sessionId;
    result.sessionIniPath = sessionWrite.iniPath;
    RecordLaunchOutcome(casterSessionDir, sessionWrite, mode, result.processId, result.ok);
    if (result.ok)
    {
        result.message = util::Format(
            L"Launched EFZ (PID %lu) session %s launch_%lu",
            result.processId,
            sessionWrite.sessionId.c_str(),
            sessionWrite.launchIndex);
    }
    return result;
}

void CasterApp::StartLaunchAsync(
    const SessionMode mode,
    const BootstrapMode bootstrap,
    std::optional<NetSessionInfo> netInfo,
    const bool sendLaunchGo)
{
    {
        std::lock_guard lock(mutex_);
        if (ui_.phase == AppPhase::Launching)
        {
            AppendLog(L"Launch already in progress");
            return;
        }
    }

    if (launchWorker_.joinable())
    {
        launchWorker_.join();
    }

    SetPhase(AppPhase::Launching, L"Launching...");
    launchWorker_ = std::thread([this, mode, bootstrap, netInfo = std::move(netInfo), sendLaunchGo]() mutable
    {
        if (sendLaunchGo && netInfo.has_value())
        {
            // Anchor the deadline before the UDP burst, or the host launches ~200 ms late.
            const std::uint32_t hostLaunchDeadline = timeGetTime() + kLaunchSyncLeadMs;
            if (!net_.SendLaunchGo(
                    netInfo->inputDelay,
                    netInfo->targetFps,
                    kLaunchSyncLeadMs))
            {
                SetPhase(
                    AppPhase::Error,
                    L"Couldn't commit the match settings to the other player");
                return;
            }
            AppendLog(util::Format(L"Launch scheduled in %u ms (both sides)", kLaunchSyncLeadMs));
            WaitUntilLaunchAt(hostLaunchDeadline, &cancelNet_);
        }

        const LaunchResult result = PrepareAndLaunch(mode, bootstrap, netInfo);
        if (cancelNet_.load())
        {
            return;
        }

        if (result.ok)
        {
            AppendLog(result.message);
            if (mode == SessionMode::Offline)
            {
                SetPhase(AppPhase::Idle, L"Game launched (offline)");
            }
            else
            {
                SetPhase(AppPhase::Idle, L"Game launched");
                readyNet_.reset();
                hostLaunchGoReceived_.store(false);
                {
                    std::lock_guard lock(mutex_);
                    ui_.hostLaunchGoReceived = false;
                }
                net_.Close();
            }
        }
        else
        {
            AppendLog(result.message);
            SetPhase(AppPhase::Error, result.message);
        }
    });
}

void CasterApp::LaunchOffline()
{
    StartLaunchAsync(SessionMode::Offline, BootstrapMode::Default, std::nullopt, false);
}

void CasterApp::LaunchReady()
{
    if (!readyNet_.has_value())
    {
        SetPhase(AppPhase::Error, L"Connect to another player before starting");
        return;
    }

    if (!readyNet_->isHost)
    {
        AppendLog(L"Client launches automatically when the host sends launch-go");
        return;
    }

    // Refresh readyNet_: Ready-screen delay edits must reach LaunchGo and session.ini.
    readyNet_->inputDelay = static_cast<std::uint8_t>(
        std::min<int>(config_.Get().inputDelay, 20));
    readyNet_->targetFps = config_.Get().targetFps;

    const SessionMode mode = SessionMode::Host;
    StartLaunchAsync(mode, BootstrapMode::Default, readyNet_, true);
}

}
