#include "caster_app.h"
#include "caster_config.h"
#include "caster_ui.h"
#include "compatibility_runtime.h"
#include "session_log.h"
#include "util.h"

#include "crash_handler.h"

#include <windows.h>

namespace
{

void FlushLauncherLogs()
{
    // Do not take SessionLog's mutex here: the fault may have occurred while it was owned.
}

const char* LauncherSessionLogPath()
{
    return trial_caster::SessionLog::LogFilePathNarrow();
}

}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const auto launcherDir = trial_caster::util::ExeDirectory();
    if (launcherDir.empty())
    {
        MessageBoxW(
            nullptr,
            L"The launcher could not determine its installation folder.",
            L"EFZ FF Netplay",
            MB_ICONERROR);
        return 1;
    }
    const auto stateDir = trial_caster::util::ResolveWritableStateDirectory(launcherDir);
    const BOOL publishedStateRoot = SetEnvironmentVariableW(
        L"TRIAL_NETPLAY_STATE_DIR",
        stateDir.c_str());
    const DWORD publishStateError = publishedStateRoot ? ERROR_SUCCESS : GetLastError();

    // Load the persisted opt-in before initializing diagnostics.
    const auto iniPath = trial_caster::ResolveLauncherIniPath(launcherDir, stateDir);
    trial_caster::ConfigStore config(iniPath);
    config.ApplyDefaults(launcherDir);
    config.Load();

    if (!trial_caster::SessionLog::Initialize(
            stateDir,
            config.Get().diagnosticLogging))
    {
        MessageBoxW(
            nullptr,
            L"The launcher could not create its runtime folder. Check folder permissions or set TRIAL_NETPLAY_STATE_DIR to a writable Windows path.",
            L"EFZ FF Netplay",
            MB_ICONERROR);
        return 1;
    }
    config.Load();
    if (!publishedStateRoot)
    {
        trial_caster::SessionLog::Warn(
            L"paths",
            L"Could not publish the writable state folder to the game child: %s",
            trial_caster::util::Win32Error(publishStateError).c_str());
    }

    thv::crash::Options crashOpts{};
    crashOpts.component = "TrialLauncher";
    crashOpts.flushLogs = &FlushLauncherLogs;
    crashOpts.sessionLogPathFn = &LauncherSessionLogPath;
    thv::crash::Install(nullptr, crashOpts);

    const trial_caster::CompatibilityRuntimeInfo runtime =
        trial_caster::DetectCompatibilityRuntime();
    const std::wstring dpiMode = trial_caster::ConfigureProcessDpiAwareness();
    std::error_code currentDirectoryError;
    const std::filesystem::path startupDirectory =
        std::filesystem::current_path(currentDirectoryError);
    trial_caster::SessionLog::Step(
        L"init",
        L"TrialLauncher starting install=%s state=%s cwd=%s runtime=%s dpi=%s",
        launcherDir.wstring().c_str(),
        stateDir.wstring().c_str(),
        currentDirectoryError ? L"(unavailable)" : startupDirectory.wstring().c_str(),
        runtime.display.c_str(),
        dpiMode.c_str());
    if (runtime.IsWine())
    {
        trial_caster::SessionLog::Info(
            L"compat",
            L"Wine build=%s host=%s prefix=%s SteamAppId=%s",
            runtime.wineBuildId.empty() ? L"(unreported)" : runtime.wineBuildId.c_str(),
            runtime.hostSystem.empty() ? L"(unreported)" : runtime.hostSystem.c_str(),
            runtime.prefixPath.empty() ? L"(default)" : runtime.prefixPath.c_str(),
            runtime.steamAppId.empty() ? L"(none)" : runtime.steamAppId.c_str());
    }

    if (!SetDllDirectoryW(launcherDir.c_str()))
    {
        trial_caster::SessionLog::Warn(
            L"paths",
            L"SetDllDirectory failed: %s",
            trial_caster::util::Win32Error(GetLastError()).c_str());
    }
    if (!SetCurrentDirectoryW(launcherDir.c_str()))
    {
        trial_caster::SessionLog::Warn(
            L"paths",
            L"Could not select the game folder as current directory: %s",
            trial_caster::util::Win32Error(GetLastError()).c_str());
    }
    trial_caster::SessionLog::Step(L"config", L"Using %s", iniPath.wstring().c_str());

    trial_caster::CasterApp app(config);
    app.AppendLog(trial_caster::util::Format(L"EFZ FF Netplay — %s", launcherDir.wstring().c_str()));

    if (!trial_caster::ui::Initialize(
            static_cast<float>(config.Get().uiScalePercent) / 100.0f))
    {
        std::wstring message = L"The launcher window could not be initialized. Check that the packaged SDL3.dll is beside the launcher.";
        if (trial_caster::SessionLog::IsFileLoggingEnabled())
        {
            message += L" Details were written to "
                + trial_caster::SessionLog::LogFilePath().wstring()
                + L".";
        }
        message += trial_caster::CompatibilityLaunchAdvice(runtime);
        MessageBoxW(nullptr, message.c_str(), L"EFZ FF Netplay", MB_ICONERROR);
        return 1;
    }

    while (trial_caster::ui::RunFrame(app))
    {
    }

    trial_caster::ui::Shutdown();
    config.Save();
    trial_caster::SessionLog::Step(L"init", L"TrialLauncher exiting");
    trial_caster::SessionLog::Shutdown();
    thv::crash::Shutdown();
    return 0;
}
