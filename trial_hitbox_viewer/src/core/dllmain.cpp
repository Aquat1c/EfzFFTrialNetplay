#include "caster_input.h"
#include "caster_session.h"
#include "caster_telemetry.h"
#include "caster_ui.h"
#include "collision_display.h"
#include "control_swap.h"
#include "debug_log.h"
#include "dinput_focus_guard.h"
#include "dinput_relay.h"
#include "crash_handler.h"
#include "lifecycle_log.h"
#include "determinism.h"
#include "framestep.h"
#include "netplay_bootstrap.h"
#include "netplay_sync.h"
#include "render_gate.h"
#include "rng.h"
#include "rollback.h"
#include "savestate.h"
#include "savestate_profiler.h"
#include "sdl_input.h"
#include "spectator_player.h"
#include "state_paths.h"
#include "trial_addresses.h"
#include "trial_compatibility.h"
#include "watchdog.h"
#include "window_telemetry.h"
#include "win32_compat.h"

#include <MinHook.h>

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

extern "C" __declspec(dllexport) const std::uint32_t TrialNetplayCompatibilityId =
    trialnet::compat::kCompatibilityId;

extern "C" __declspec(dllexport) std::uint32_t __cdecl TrialNetplayGetCompatibilityId() noexcept
{
    return trialnet::compat::kCompatibilityId;
}

namespace
{

using DrawCollisionBoxesFn = int(__cdecl*)(int player, int camX, int camY);
DrawCollisionBoxesFn g_originalDrawCollisionBoxes = nullptr;

constexpr DWORD kPollIntervalMs = 16;

std::atomic<bool> g_shuttingDown{false};
std::atomic<bool> g_modReady{false};
std::atomic<bool> g_hooksInstalled{false};
HANDLE g_modThread = nullptr;
HANDLE g_singleInstanceMutex = nullptr;
bool g_timerPeriodActive = false;
bool g_dinputFocusGuardReady = false;
wchar_t g_attachModulePath[32768] = {};

bool IsLauncherManagedProcess()
{
    return GetEnvironmentVariableW(L"TRIAL_CASTER_SESSION_INI", nullptr, 0u) > 1u;
}

// A second copy of this DLL may load after injection; that image must stay inert.
enum class CasterImageDisposition
{
    Active,
    InertSecondary,
    IdentificationError,
};

CasterImageDisposition ClassifyCasterImage(const HMODULE module)
{
    wchar_t casterOnly[8] = {};
    const DWORD casterOnlyLength = GetEnvironmentVariableW(
        L"TRIAL_CASTER_ONLY", casterOnly, ARRAYSIZE(casterOnly));
    if (casterOnlyLength == 0u)
    {
        return CasterImageDisposition::Active;
    }
    if (casterOnlyLength >= ARRAYSIZE(casterOnly))
    {
        return CasterImageDisposition::IdentificationError;
    }
    if (lstrcmpW(casterOnly, L"1") != 0)
    {
        return CasterImageDisposition::Active;
    }

    g_attachModulePath[0] = L'\0';
    const DWORD length = GetModuleFileNameW(
        module, g_attachModulePath, static_cast<DWORD>(std::size(g_attachModulePath)));
    if (length == 0u || length >= std::size(g_attachModulePath))
    {
        return CasterImageDisposition::IdentificationError;
    }

    const wchar_t* baseName = g_attachModulePath;
    for (const wchar_t* cursor = g_attachModulePath; *cursor != L'\0'; ++cursor)
    {
        if (*cursor == L'\\' || *cursor == L'/')
        {
            baseName = cursor + 1;
        }
    }
    return lstrcmpiW(baseName, L"netplay.dll") == 0
        ? CasterImageDisposition::Active
        : CasterImageDisposition::InertSecondary;
}

// Module images have separate statics; a PID-scoped mutex picks one mod thread.
enum class SingleInstanceClaim
{
    Owned,
    AlreadyOwned,
    Error,
};

SingleInstanceClaim ClaimSingleInstance()
{
    char name[64] = {};
    std::snprintf(name, sizeof(name), "Local\\TrialNetplayMod_%lu", GetCurrentProcessId());
    g_singleInstanceMutex = CreateMutexA(nullptr, TRUE, name);
    if (g_singleInstanceMutex == nullptr)
    {
        OutputDebugStringA("netplay: single-instance mutex creation failed; refusing unsafe startup\n");
        return SingleInstanceClaim::Error;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = nullptr;
        OutputDebugStringA("netplay: second DLL image in-process — mod thread already owned, no-op\n");
        return SingleInstanceClaim::AlreadyOwned;
    }
    return SingleInstanceClaim::Owned;
}

void ReleaseSingleInstanceClaim()
{
    if (g_singleInstanceMutex == nullptr)
    {
        return;
    }
    ReleaseMutex(g_singleInstanceMutex);
    CloseHandle(g_singleInstanceMutex);
    g_singleInstanceMutex = nullptr;
}

bool PinActiveModule(const HMODULE expectedModule)
{
    // Pin this exact image before the IAT hook; a copy may load elsewhere.
    HMODULE pinnedModule = nullptr;
    constexpr DWORD kFlags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
        | GET_MODULE_HANDLE_EX_FLAG_PIN;
    const BOOL pinned = GetModuleHandleExW(
        kFlags,
        reinterpret_cast<LPCWSTR>(&g_modThread),
        &pinnedModule);
    return pinned != FALSE && pinnedModule == expectedModule;
}

int __cdecl HookedDrawCollisionBoxes(int player, int camX, int camY)
{
    const std::uint8_t layerMask = thv::GetLayerMask();
    if (layerMask == 0)
    {
        return 0;
    }

    thv::DrawPlayerCollisionBoxes(player, camX, camY, layerMask);
    return 0;
}

// Hooking during DirectDraw setup makes WinMain exit; wait for init to finish.
bool WaitForDDrawReady(const char* why)
{
    const bool wine = thv::win32::IsWineRuntime();
    const int kMaxWaitMs = wine ? 30000 : 8000;
    const int kInitTableWaitMs = wine ? 15000 : 4000;
    const DWORD kSettleMs = wine ? 1000u : 500u;
    constexpr int kStepMs = 20;
    for (int waited = 0; waited < kMaxWaitMs && !g_shuttingDown.load(); waited += kStepMs)
    {
        const auto* const surf =
            static_cast<void* const*>(trial::ResolveVa(trial::kVaPrimarySurface));
        if (surf != nullptr && *surf != nullptr)
        {
            const volatile std::int32_t* const initDone =
                static_cast<const volatile std::int32_t*>(trial::ResolveVa(static_cast<std::uintptr_t>(0x004A3E34u)));
            int extra = 0;
            bool initTableReady = false;
            while (extra < kInitTableWaitMs && !g_shuttingDown.load())
            {
                if (initDone != nullptr && *initDone == 65025)
                {
                    initTableReady = true;
                    break;
                }
                Sleep(kStepMs);
                extra += kStepMs;
            }
            if (initTableReady)
            {
                thv::log::Step(
                    "hooks",
                    "DDraw ready (%s): surface @%dms, init-table marker=65025 @+%dms",
                    why != nullptr ? why : "?",
                    waited,
                    extra);
                Sleep(kSettleMs);
                return true;
            }
            thv::log::Warn(
                "hooks",
                "DDraw surface appeared @%dms but init-table marker never reached 65025 within +%dms (%s runtime=%s)",
                waited,
                kInitTableWaitMs,
                why != nullptr ? why : "?",
                wine ? "Wine/Proton" : "Windows");
            return false;
        }
        Sleep(kStepMs);
    }
    thv::log::Warn(
        "hooks",
        "DDraw primary surface not seen before %dms timeout (%s runtime=%s)",
        kMaxWaitMs,
        why != nullptr ? why : "?",
        wine ? "Wine/Proton" : "Windows");
    return false;
}

bool InitializeRuntimeCompatibility()
{
    bool runtimeReady = true;
    const thv::win32::RuntimeInfo runtime = thv::win32::QueryRuntimeInfo();
    thv::win32::LoadedPeIdentity image{};
    const thv::win32::TrialHostStatus hostStatus = thv::win32::ValidateTrialHostImage(
        GetModuleHandleW(nullptr), &image);
    thv::log::Info(
        "platform",
        "runtime=%s wine_version='%s' wine_build='%s' host='%s %s'",
        runtime.wine ? "Wine/Proton" : "Windows",
        runtime.wineVersion,
        runtime.wineBuildId,
        runtime.hostSystem,
        runtime.hostRelease);
    thv::log::Info(
        "platform",
        "EFZ image validation=%s runtimeBase=0x%08lX preferred=0x%08X size=0x%08X timestamp=0x%08X entryRva=0x%08X characteristics=0x%04X reloc={rva:0x%08X bytes:%u}",
        thv::win32::TrialHostStatusName(hostStatus),
        static_cast<unsigned long>(image.runtimeImageBase),
        image.preferredImageBase,
        image.sizeOfImage,
        image.timeDateStamp,
        image.addressOfEntryPoint,
        image.characteristics,
        image.relocationRva,
        image.relocationBytes);

    const HMODULE ddraw = GetModuleHandleW(L"ddraw.dll");
    std::wstring ddrawPath;
    if (ddraw != nullptr)
    {
        std::vector<wchar_t> pathBuffer(512u, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(
                ddraw, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
            if (length == 0u)
            {
                break;
            }
            if (length < pathBuffer.size() - 1u)
            {
                ddrawPath.assign(pathBuffer.data(), length);
                break;
            }
            if (pathBuffer.size() >= 32768u)
            {
                break;
            }
            pathBuffer.resize(pathBuffer.size() * 2u, L'\0');
        }
    }
    std::string ddrawPathUtf8;
    if (!ddrawPath.empty())
    {
        const int required = WideCharToMultiByte(
            CP_UTF8, 0, ddrawPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (required > 1)
        {
            ddrawPathUtf8.resize(static_cast<std::size_t>(required), '\0');
            WideCharToMultiByte(
                CP_UTF8,
                0,
                ddrawPath.c_str(),
                -1,
                ddrawPathUtf8.data(),
                required,
                nullptr,
                nullptr);
            ddrawPathUtf8.resize(static_cast<std::size_t>(required - 1));
        }
    }
    const bool hasOverlayCallback = ddraw != nullptr
        && GetProcAddress(ddraw, "EFZDDRAW_RegisterOverlayCallback") != nullptr;
    const bool hasFrameLimitHandoff = ddraw != nullptr
        && GetProcAddress(ddraw, "EFZDDRAW_SetFrameLimit") != nullptr;
    const bool hasHudProvider = ddraw != nullptr
        && GetProcAddress(ddraw, "EFZDX9_RegisterNetplayHudProvider") != nullptr;
    const bool hasOverlayProvider = ddraw != nullptr
        && GetProcAddress(ddraw, "EFZDX9_RegisterOverlayProvider") != nullptr;
    const bool hasTrainingMenuBridge = ddraw != nullptr
        && GetProcAddress(ddraw, "EFZDX9_RegisterTrainingMenuBridge") != nullptr;
    const bool customDdraw = hasOverlayCallback || hasFrameLimitHandoff || hasHudProvider
        || hasOverlayProvider || hasTrainingMenuBridge;
    thv::log::Info(
        "platform",
        "ddraw module=%s custom_efz_wrapper=%d exports={overlay_callback:%d frame_limit:%d hud_provider:%d overlay_provider:%d training_bridge:%d}",
        ddrawPathUtf8.empty() ? "(not loaded/unresolved)" : ddrawPathUtf8.c_str(),
        customDdraw ? 1 : 0,
        hasOverlayCallback ? 1 : 0,
        hasFrameLimitHandoff ? 1 : 0,
        hasHudProvider ? 1 : 0,
        hasOverlayProvider ? 1 : 0,
        hasTrainingMenuBridge ? 1 : 0);
    if (runtime.wine && !customDdraw)
    {
        runtimeReady = false;
        thv::lifecycle::Fault(
            "platform",
            "Wine/Proton loaded builtin/non-EFZ ddraw; set WINEDLLOVERRIDES=ddraw=n,b");
        thv::log::Warn(
            "platform",
            "Wine/Proton ddraw override mismatch: packaged EFZ wrapper export missing; rendering/input wrapper behavior is not guaranteed");
    }

    TIMECAPS caps{};
    const MMRESULT capsResult = timeGetDevCaps(&caps, sizeof(caps));
    const MMRESULT periodResult = timeBeginPeriod(1u);
    g_timerPeriodActive = periodResult == TIMERR_NOERROR;
    thv::log::Info(
        "platform",
        "multimedia timer caps=%s min=%u max=%u request_1ms=%s",
        capsResult == TIMERR_NOERROR ? "ok" : "failed",
        capsResult == TIMERR_NOERROR ? caps.wPeriodMin : 0u,
        capsResult == TIMERR_NOERROR ? caps.wPeriodMax : 0u,
        g_timerPeriodActive ? "active" : "unavailable");

    const bool secureRandomReady = thv::win32::PrewarmSecureRandomProvider();
    thv::log::Info(
        "platform",
        "secure random provider prewarmed=%d",
        secureRandomReady ? 1 : 0);
    if (!secureRandomReady)
    {
        runtimeReady = false;
        thv::lifecycle::Fault(
            "platform",
            "No secure random provider is available (BCryptGenRandom/SystemFunction036)");
    }
    return runtimeReady;
}

void ShutdownRuntimeCompatibility()
{
    if (g_timerPeriodActive)
    {
        timeEndPeriod(1u);
        g_timerPeriodActive = false;
    }
}

bool InstallHooks()
{
    thv::log::Step("hooks", "InstallHooks begin");
    if (MH_Initialize() != MH_OK)
    {
        thv::lifecycle::Fault("hooks", "MinHook initialize failed");
        thv::log::Error("hooks", "MinHook initialize failed");
        return false;
    }

    if (!thv::det::Initialize() || !thv::det::rng::Initialize())
    {
        thv::lifecycle::Fault("hooks", "determinism or RNG initialize failed");
        thv::log::Error("hooks", "determinism or RNG initialize failed");
        thv::det::rng::Shutdown();
        thv::det::Shutdown();
        MH_Uninitialize();
        return false;
    }

    void* const collisionTarget = trial::ResolveVa(trial::kVaDrawCollisionBoxes);
    if (collisionTarget == nullptr)
    {
        thv::lifecycle::Fault("hooks", "collision draw target unresolved");
        thv::log::Error("hooks", "collision draw target unresolved");
        thv::det::rng::Shutdown();
        thv::det::Shutdown();
        MH_Uninitialize();
        return false;
    }

    if (MH_CreateHook(
            collisionTarget,
            &HookedDrawCollisionBoxes,
            reinterpret_cast<void**>(&g_originalDrawCollisionBoxes))
        != MH_OK)
    {
        thv::lifecycle::Fault("hooks", "collision draw hook create failed");
        thv::log::Error("hooks", "collision draw hook create failed");
        thv::det::rng::Shutdown();
        thv::det::Shutdown();
        MH_Uninitialize();
        return false;
    }

    if (MH_QueueEnableHook(collisionTarget) != MH_OK)
    {
        thv::lifecycle::Fault("hooks", "collision draw hook queue-enable failed");
        thv::log::Error("hooks", "collision draw hook queue-enable failed");
        MH_DisableHook(collisionTarget);
        MH_RemoveHook(collisionTarget);
        thv::det::rng::Shutdown();
        thv::det::Shutdown();
        MH_Uninitialize();
        return false;
    }

    if (!thv::InitializeDinputRelay())
    {
        thv::lifecycle::Fault("hooks", "DirectInput relay initialize failed");
        thv::log::Error("hooks", "DirectInput relay initialize failed");
        thv::ShutdownDinputRelay();
        MH_DisableHook(collisionTarget);
        MH_RemoveHook(collisionTarget);
        thv::det::rng::Shutdown();
        thv::det::Shutdown();
        MH_Uninitialize();
        return false;
    }

    if (!thv::InitializeFramestep() || !thv::InitializeControlSwap() || !thv::InitializeSavestate())
    {
        thv::lifecycle::Fault("hooks", "framestep, control swap, or savestate initialize failed");
        thv::log::Error("hooks", "framestep, control swap, or savestate initialize failed");
        MH_DisableHook(collisionTarget);
        MH_RemoveHook(collisionTarget);
        thv::det::rng::Shutdown();
        thv::det::Shutdown();
        MH_Uninitialize();
        return false;
    }

    // Without these hooks replay drains sim RNG and repeats audio; refuse online init.
    if (!thv::InitializeRenderGate())
    {
        thv::lifecycle::Fault("hooks", "rollback render/presentation contract unavailable");
        thv::log::Error("hooks", "rollback render/presentation contract unavailable");
        thv::ShutdownRenderGate();
        thv::ShutdownSavestate();
        thv::ShutdownControlSwap();
        thv::ShutdownFramestep();
        thv::ShutdownDinputRelay();
        MH_DisableHook(collisionTarget);
        MH_RemoveHook(collisionTarget);
        thv::det::rng::Shutdown();
        thv::det::Shutdown();
        MH_Uninitialize();
        return false;
    }

    const MH_STATUS applyStatus = MH_ApplyQueued();
    if (applyStatus != MH_OK)
    {
        thv::lifecycle::Fault(
            "hooks", "batched MinHook enable failed status=%d", static_cast<int>(applyStatus));
        thv::log::Error(
            "hooks", "batched MinHook enable failed status=%d", static_cast<int>(applyStatus));
        thv::ShutdownRenderGate();
        thv::ShutdownSavestate();
        thv::ShutdownControlSwap();
        thv::ShutdownFramestep();
        thv::ShutdownDinputRelay();
        MH_DisableHook(collisionTarget);
        MH_RemoveHook(collisionTarget);
        thv::det::rng::Shutdown();
        thv::det::Shutdown();
        MH_Uninitialize();
        return false;
    }
    thv::log::Info("hooks", "batched MinHook enable committed in one thread-freeze pass");

    g_hooksInstalled.store(true);
    thv::telemetry::DumpFullSnapshot("hooks installed");
    return true;
}

void RemoveHooks()
{
    if (!g_hooksInstalled.exchange(false))
    {
        return;
    }

    thv::watchdog::Shutdown();
    thv::telemetry::Shutdown();
    thv::rollback::Shutdown();
    thv::ShutdownRenderGate();
    thv::ShutdownSavestate();
    thv::ShutdownControlSwap();
    thv::ShutdownFramestep();
    thv::ShutdownDinputRelay();
    thv::caster::Shutdown();
    thv::det::rng::Shutdown();
    thv::det::Shutdown();

    void* const collisionTarget = trial::ResolveVa(trial::kVaDrawCollisionBoxes);
    if (collisionTarget != nullptr)
    {
        MH_DisableHook(collisionTarget);
        MH_RemoveHook(collisionTarget);
    }
    MH_Uninitialize();
    g_modReady.store(false);
}

bool InitializeMod()
{
    thv::lifecycle::Init("mod", "InitializeMod begin");
    thv::log::Step("init", "InitializeMod begin");
    if (!thv::telemetry::Initialize())
    {
        OutputDebugStringA("netplay: telemetry/log init failed\n");
        return false;
    }
    thv::log::Step("init", "telemetry online dir=%s", thv::log::SessionDirectory().c_str());
    if (thv::DinputAcquireRescues() > 0)
    {
        thv::log::Info(
            "hooks",
            "DI focus guard rescued %u fatal Acquire failure(s) during game init (background launch)",
            thv::DinputAcquireRescues());
    }

    thv::watchdog::Initialize();

    if (!thv::ui::Initialize())
    {
        thv::lifecycle::Fault("init", "caster UI init failed");
        thv::log::Error("init", "caster UI init failed");
        thv::telemetry::Shutdown();
        return false;
    }

    thv::log::Step("init", "caster UI ready");

    if (!thv::input::Initialize())
    {
        thv::lifecycle::Fault("init", "keyboard input init failed");
        thv::log::Error("init", "keyboard input init failed");
        thv::ui::Shutdown();
        thv::telemetry::Shutdown();
        return false;
    }

    thv::log::Step("init", "SDL input ready");

    if (!thv::caster::Initialize())
    {
        thv::lifecycle::Fault("init", "caster input bridge init failed");
        thv::log::Error("init", "caster input bridge init failed");
        thv::input::Shutdown();
        thv::ui::Shutdown();
        thv::telemetry::Shutdown();
        return false;
    }
    thv::log::Step("init", "caster input bridge ready");

    if (!thv::rollback::Initialize())
    {
        thv::lifecycle::Fault("init", "rollback module init failed");
        thv::log::Error("init", "rollback module init failed");
        thv::caster::Shutdown();
        thv::input::Shutdown();
        thv::ui::Shutdown();
        thv::telemetry::Shutdown();
        return false;
    }

    if (!thv::Initialize())
    {
        thv::lifecycle::Fault("init", "collision display init failed");
        thv::log::Error("init", "collision display init failed");
        thv::caster::Shutdown();
        thv::input::Shutdown();
        thv::ui::Shutdown();
        thv::telemetry::Shutdown();
        return false;
    }

    thv::log::Step("init", "rollback module ready");

    if (!InstallHooks())
    {
        thv::rollback::Shutdown();
        thv::Shutdown();
        thv::caster::Shutdown();
        thv::input::Shutdown();
        thv::ui::Shutdown();
        return false;
    }
    thv::log::Step("init", "hooks installed");

    thv::session::LauncherSession launcherSession{};
    std::filesystem::path sessionIniPath;
    if (thv::session::TryLoadSession(&launcherSession, &sessionIniPath))
    {
        const bool online = launcherSession.mode == thv::session::LauncherMode::Host
              || launcherSession.mode == thv::session::LauncherMode::Client
              || launcherSession.mode == thv::session::LauncherMode::Spectator;
        if ((online || IsLauncherManagedProcess())
            && launcherSession.compatibilityId != trialnet::compat::kCompatibilityId)
        {
            thv::lifecycle::Exit(
                "session",
                "launcher/netplay.dll compatibility mismatch",
                thv::paths::PathUtf8(sessionIniPath).c_str());
            thv::log::Error(
                "session",
                "online session compatibility mismatch launcher=0x%08X dll=0x%08X",
                launcherSession.compatibilityId,
                trialnet::compat::kCompatibilityId);
            thv::log::FlushFile();
            ExitProcess(ERROR_REVISION_MISMATCH);
            return false;
        }
        thv::session::ApplyToCasterBridge(launcherSession);
    }
    else
    {
        const auto resolved = thv::session::ResolveSessionIniPath();
        if (IsLauncherManagedProcess())
        {
            thv::lifecycle::Exit(
                "session",
                "launcher session descriptor is missing, unreadable, incomplete, or invalid",
                thv::paths::PathUtf8(resolved).c_str());
            thv::log::Error(
                "session",
                "managed session handoff failed validation path=%s; refusing offline fallback",
                resolved.empty() ? "(unresolved)" : thv::paths::PathUtf8(resolved).c_str());
            thv::log::FlushFile();
            ExitProcess(ERROR_INVALID_DATA);
            return false;
        }
        thv::log::Step(
            "init",
            "no session config (%s) — offline defaults",
            resolved.empty() ? "(unresolved)" : thv::paths::PathUtf8(resolved).c_str());
    }

    g_modReady.store(true);
    thv::lifecycle::Init("mod", "netplay.dll ready session=%s", thv::log::SessionLogPath().c_str());
    thv::log::Step("init", "netplay.dll ready");
    return true;
}

DWORD WINAPI ModThread(LPVOID)
{
    // Must precede the DDraw wait: input init runs right after DDraw init.
    thv::ReassertDinputFocusGuard();

    thv::log::ConfigureFileLogging(thv::session::DiagnosticFileLoggingRequested());
    thv::log::Initialize();

    thv::crash::Options crashOpts{};
    crashOpts.component = "netplay";
    crashOpts.flushLogs = &thv::log::FlushFile;
    crashOpts.sessionLogPathFn = &thv::log::SessionLogPathForCrash;
    thv::crash::Install(nullptr, crashOpts);
    const bool socketRuntimeReady = thv::netplay::InitializeSocketRuntime();
    thv::log::Info(
        "platform",
        "Winsock worker initialization ready=%d error=%d",
        socketRuntimeReady ? 1 : 0,
        thv::netplay::SocketRuntimeError());
    const bool runtimeCompatibilityReady = InitializeRuntimeCompatibility();

    if ((!g_dinputFocusGuardReady || !socketRuntimeReady || !runtimeCompatibilityReady)
        && IsLauncherManagedProcess())
    {
        const char* const failure = !g_dinputFocusGuardReady
            ? "DirectInput focus guard could not be installed"
            : !socketRuntimeReady
                ? "Winsock could not be initialized outside the loader lock"
                : "Wine/Proton runtime compatibility contract failed";
        thv::lifecycle::Exit("init", failure, "launcher-managed game terminated fail-closed");
        thv::log::Error("init", "%s; refusing to run an unhooked launcher session", failure);
        thv::log::FlushFile();
        ExitProcess(ERROR_DLL_INIT_FAILED);
        return ERROR_DLL_INIT_FAILED;
    }
    if (!g_dinputFocusGuardReady)
    {
        thv::log::Warn(
            "hooks",
            "DirectInput focus guard unavailable in legacy/manual mode; launch focus behavior is not guaranteed");
    }
    if (!socketRuntimeReady)
    {
        thv::log::Warn(
            "platform",
            "Winsock unavailable in legacy/manual mode; online play will remain disabled");
    }
    if (!runtimeCompatibilityReady)
    {
        thv::log::Warn(
            "platform",
            "legacy/manual mode continuing after runtime compatibility contract failure");
    }

    bool ddrawReady = false;
    if (!g_shuttingDown.load())
    {
        ddrawReady = WaitForDDrawReady("mod init gate");
    }
    if (!ddrawReady && IsLauncherManagedProcess())
    {
        thv::lifecycle::Exit(
            "init",
            "DirectDraw initialization marker timed out",
            "launcher-managed game terminated before hook installation");
        thv::log::Error(
            "init",
            "DirectDraw readiness was not proven; refusing to install hooks in launcher-managed process");
        thv::log::FlushFile();
        ExitProcess(ERROR_DLL_INIT_FAILED);
        return ERROR_DLL_INIT_FAILED;
    }
    if (!ddrawReady)
    {
        thv::log::Warn(
            "init",
            "legacy/manual mode proceeding without proven DirectDraw readiness; hook race risk remains");
    }
    if (!g_shuttingDown.load())
    {
        if (!InitializeMod())
        {
            thv::lifecycle::Exit("init", "mod init failed", "game continues without hooks");
            if (IsLauncherManagedProcess())
            {
                thv::log::Error(
                    "init",
                    "launcher-managed initialization failed; terminating instead of running offline/unhooked");
                thv::log::FlushFile();
                ExitProcess(ERROR_DLL_INIT_FAILED);
                return ERROR_DLL_INIT_FAILED;
            }
        }
    }

    while (!g_shuttingDown.load())
    {
        if (g_modReady.load())
        {
            thv::input::Poll();
              if (!thv::caster::IsNetplayActive() && !thv::caster::IsSpectatorActive())
            {
                thv::PollHotkeys();
                thv::PollFramestepHotkeys();
                thv::PollControlSwapHotkeys();
                thv::PollSavestateHotkeys();
            }
            thv::telemetry::PollHotkeys();
            thv::MaintainDinputRelay();
            thv::caster::OnInputTick();
            const HWND gameWindow = thv::ResolveGameWindow();
            thv::window_telemetry::Poll(gameWindow);
            if (!thv::window_telemetry::IsOnlineExitPending())
            {
                thv::netplay::OnModTick(gameWindow);
                thv::spectator::player::OnModTick(gameWindow);
                thv::rollback::OnModTick();
            }
            thv::savestate_profiler::SampleLiveRegion();
        }
        Sleep(kPollIntervalMs);
    }

    thv::lifecycle::Phase("mod", "running", "shutdown", "mod thread exiting");
    thv::netplay::Shutdown();
    thv::window_telemetry::Shutdown();
    thv::rollback::Shutdown();
    thv::caster::Shutdown();
    thv::input::Shutdown();
    thv::ui::Shutdown();
    ShutdownRuntimeCompatibility();
    thv::log::Shutdown();
    return 0;
}

}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        // No DisableThreadLibraryCalls: this /MT DLL uses thread_local RNG state.
        const CasterImageDisposition imageDisposition = ClassifyCasterImage(module);
        switch (imageDisposition)
        {
        case CasterImageDisposition::InertSecondary:
            OutputDebugStringA("netplay: caster-only process ignored non-netplay DLL image\n");
            break;
        case CasterImageDisposition::IdentificationError:
            OutputDebugStringA(
                "netplay: could not identify DLL image in caster-only process; rejecting load\n");
            return FALSE;
        case CasterImageDisposition::Active:
            break;
        }
        if (imageDisposition == CasterImageDisposition::InertSecondary)
        {
            break;
        }
        const SingleInstanceClaim instanceClaim = ClaimSingleInstance();
        if (instanceClaim == SingleInstanceClaim::AlreadyOwned)
        {
            break;
        }
        if (instanceClaim == SingleInstanceClaim::Error)
        {
            return FALSE;
        }
        {
            // Reject an incompatible EXE before the IAT write.
            const thv::win32::TrialHostStatus hostStatus =
                thv::win32::ValidateTrialHostImage(GetModuleHandleW(nullptr));
            if (hostStatus != thv::win32::TrialHostStatus::Ok)
            {
                char message[192] = {};
                std::snprintf(
                    message,
                    sizeof(message),
                    "netplay: unsupported EFZ executable (%s); DLL remains inert\n",
                    thv::win32::TrialHostStatusName(hostStatus));
                OutputDebugStringA(message);
                ReleaseSingleInstanceClaim();
                if (IsLauncherManagedProcess())
                {
                    return FALSE;
                }
                break;
            }
        }
        if (!PinActiveModule(module))
        {
            OutputDebugStringA(
                "netplay: failed to pin active DLL image; rejecting startup before hooks/threads\n");
            ReleaseSingleInstanceClaim();
            return FALSE;
        }
        // Before WinMain runs: the game's input init treats a denied Acquire as fatal.
        g_dinputFocusGuardReady = thv::InstallDinputFocusGuard();
        if (!g_dinputFocusGuardReady)
        {
            OutputDebugStringA("netplay: dinput focus guard install FAILED (launch flake unguarded)\n");
        }
        g_modThread = CreateThread(nullptr, 0, ModThread, nullptr, 0, nullptr);
        if (g_modThread == nullptr)
        {
            OutputDebugStringA(
                "netplay: failed to create mod thread after pinning; terminating process\n");
            if (g_dinputFocusGuardReady)
            {
                thv::ShutdownDinputFocusGuard();
                g_dinputFocusGuardReady = false;
            }
            TerminateProcess(GetCurrentProcess(), ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
        OutputDebugStringA("netplay: mod thread created; active image pinned\n");
        break;
    }

    case DLL_PROCESS_DETACH:
        // Loader lock held: never wait for threads or touch SDL/Winsock/DbgHelp/hooks here.
        (void)reserved;
        break;

    default:
        break;
    }

    return TRUE;
}
