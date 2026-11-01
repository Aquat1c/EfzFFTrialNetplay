#include "caster_session.h"

#include "caster_input.h"
#include "control_config.h"
#include "control_swap.h"
#include "debug_log.h"
#include "framestep.h"
#include "game_pacing.h"
#include "lifecycle_log.h"
#include "menu_nav.h"
#include "netplay_bootstrap.h"
#include "spectator_player.h"
#include "state_paths.h"
#include "trial_protocol.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <vector>

namespace thv::session
{
namespace
{

LauncherSession g_cachedSession{};
bool g_hasCachedSession = false;

constexpr wchar_t kSessionIniEnvVar[] = L"TRIAL_CASTER_SESSION_INI";

}

namespace
{

std::filesystem::path GameExeDirectory()
{
    std::vector<wchar_t> buffer(512u, L'\0');
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0u)
        {
            return {};
        }
        if (length < buffer.size() - 1u)
        {
            return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        }
        if (buffer.size() >= 32768u)
        {
            return {};
        }
        buffer.resize(std::min<std::size_t>(buffer.size() * 2u, 32768u), L'\0');
    }
}

std::wstring ReadIni(const wchar_t* section, const wchar_t* key, const wchar_t* def, const std::wstring& file)
{
    wchar_t buf[512] = {};
    GetPrivateProfileStringW(section, key, def, buf, static_cast<DWORD>(std::size(buf)), file.c_str());
    return buf;
}

bool ReadRequiredIni(
    const wchar_t* const section,
    const wchar_t* const key,
    const std::wstring& file,
    std::wstring* const out)
{
    if (out == nullptr)
    {
        return false;
    }
    constexpr wchar_t kMissing[] = L"{TRIAL_NETPLAY_MISSING_VALUE}";
    wchar_t buffer[1024] = {};
    const DWORD length = GetPrivateProfileStringW(
        section,
        key,
        kMissing,
        buffer,
        static_cast<DWORD>(std::size(buffer)),
        file.c_str());
    if (length >= std::size(buffer) - 1u || std::wcscmp(buffer, kMissing) == 0)
    {
        return false;
    }
    out->assign(buffer, length);
    return true;
}

bool ParseUnsigned(
    const std::wstring& text,
    const std::uint32_t maximum,
    std::uint32_t* const out)
{
    if (out == nullptr || text.empty())
    {
        return false;
    }
    std::uint64_t value = 0u;
    for (const wchar_t ch : text)
    {
        if (ch < L'0' || ch > L'9')
        {
            return false;
        }
        const std::uint32_t digit = static_cast<unsigned>(ch - L'0');
        if (value > (static_cast<std::uint64_t>(maximum) - digit) / 10u)
        {
            return false;
        }
        value = value * 10u + digit;
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
}

LauncherMode ParseMode(const std::wstring& text)
{
    if (text == L"host")
    {
        return LauncherMode::Host;
    }
    if (text == L"client")
    {
        return LauncherMode::Client;
    }
    if (text == L"spectator")
    {
        return LauncherMode::Spectator;
    }
    return LauncherMode::Offline;
}

BootstrapMode ParseBootstrap(const std::wstring& text)
{
    if (text == L"debug_vs_human" || text == L"vs_human")
    {
        return BootstrapMode::DebugVsHuman;
    }
    return BootstrapMode::Default;
}

bool LoadSessionFromIni(const std::filesystem::path& path, LauncherSession* out)
{
    if (out == nullptr || path.empty())
    {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u)
    {
        return false;
    }

    const std::wstring file = path.wstring();
    std::wstring modeText;
    std::wstring readyText;
    std::wstring isHostText;
    std::wstring localIsP1Text;
    std::wstring peerIpText;
    std::wstring portText;
    std::wstring peerPortText;
    std::wstring delayText;
    std::wstring rollbackFramesText;
    std::wstring targetFpsText;
    std::wstring allowPracticeKeysText;
    std::wstring compatibilityText;
    std::wstring sessionIdText;
    if (!ReadRequiredIni(L"session", L"mode", file, &modeText)
        || !ReadRequiredIni(L"session", L"ready", file, &readyText)
        || !ReadRequiredIni(L"session", L"is_host", file, &isHostText)
        || !ReadRequiredIni(L"session", L"local_is_p1", file, &localIsP1Text)
        || !ReadRequiredIni(L"session", L"peer_ip", file, &peerIpText)
        || !ReadRequiredIni(L"session", L"port", file, &portText)
        || !ReadRequiredIni(L"session", L"peer_port", file, &peerPortText)
        || !ReadRequiredIni(L"session", L"input_delay", file, &delayText)
        || !ReadRequiredIni(L"session", L"rollback_frames", file, &rollbackFramesText)
        || !ReadRequiredIni(L"session", L"target_fps", file, &targetFpsText)
        || !ReadRequiredIni(L"session", L"allow_practice_keys", file, &allowPracticeKeysText)
        || !ReadRequiredIni(L"session", L"compatibility_id", file, &compatibilityText)
        || !ReadRequiredIni(L"session", L"session_id", file, &sessionIdText))
    {
        log::Error("session", "launcher descriptor is missing required fields");
        return false;
    }

    const bool validMode = modeText == L"offline" || modeText == L"host"
        || modeText == L"client" || modeText == L"spectator";
    std::uint32_t port = 0u;
    std::uint32_t peerPort = 0u;
    std::uint32_t inputDelay = 0u;
    std::uint32_t rollbackFrames = 0u;
    std::uint32_t targetFps = 0u;
    std::uint32_t compatibilityId = 0u;
    if (!validMode || readyText != L"1"
        || (isHostText != L"0" && isHostText != L"1")
        || (localIsP1Text != L"0" && localIsP1Text != L"1")
        || (allowPracticeKeysText != L"0" && allowPracticeKeysText != L"1")
        || sessionIdText.empty()
        || !ParseUnsigned(portText, 65535u, &port) || port == 0u
        || !ParseUnsigned(peerPortText, 65535u, &peerPort)
        || !ParseUnsigned(delayText, proto::kMaxInputDelay, &inputDelay)
        || !ParseUnsigned(rollbackFramesText, proto::kMaxRollbackFrames, &rollbackFrames)
        || !ParseUnsigned(targetFpsText, proto::kMaximumTargetFps, &targetFps)
        || !proto::IsSupportedTargetFps(static_cast<std::uint8_t>(targetFps))
        || !ParseUnsigned(compatibilityText, 0xFFFFFFFFu, &compatibilityId)
        || ((modeText == L"client" || modeText == L"spectator") && peerIpText.empty())
        || ((modeText == L"host") != (isHostText == L"1")))
    {
        log::Error("session", "launcher descriptor contains invalid required values");
        return false;
    }

    LauncherSession session{};
    session.mode = ParseMode(modeText);
    session.bootstrap = ParseBootstrap(ReadIni(L"session", L"bootstrap", L"default", file));
    session.peerIp = peerIpText;
    session.playerName = ReadIni(L"session", L"player_name", L"", file);
    session.localIsP1 = localIsP1Text == L"1";
    session.isHost = isHostText == L"1";
    session.ready = true;
    session.port = static_cast<std::uint16_t>(port);
    session.peerPort = static_cast<std::uint16_t>(peerPort);
    session.inputDelay = static_cast<std::uint8_t>(inputDelay);
    session.rollbackFrames = static_cast<std::uint8_t>(rollbackFrames);
    session.targetFps = static_cast<std::uint8_t>(targetFps);
    session.pingMs = static_cast<std::uint32_t>(GetPrivateProfileIntW(L"session", L"ping_ms", 0, file.c_str()));
    session.usedAutopunch = ReadIni(L"session", L"autopunch", L"0", file) != L"0";
    session.allowPracticeKeys = allowPracticeKeysText == L"1";
    session.compatibilityId = compatibilityId;
    session.peerName = ReadIni(L"session", L"peer_name", L"", file);
    session.sessionId = sessionIdText;
    session.launchIndex =
        static_cast<unsigned long>(GetPrivateProfileIntW(L"session", L"launch_index", 0, file.c_str()));
    session.casterPid =
        static_cast<unsigned long>(GetPrivateProfileIntW(L"session", L"caster_pid", 0, file.c_str()));
    session.requireRollback = ReadIni(L"session", L"rollback", L"0", file) != L"0";

    *out = session;
    return true;
}

}

std::filesystem::path ResolveSessionIniPath()
{
    const DWORD required = GetEnvironmentVariableW(kSessionIniEnvVar, nullptr, 0u);
    if (required > 1u)
    {
        std::vector<wchar_t> buffer(required, L'\0');
        const DWORD length = GetEnvironmentVariableW(
            kSessionIniEnvVar,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length > 0u && length < buffer.size())
        {
            return std::filesystem::path(std::wstring(buffer.data(), length));
        }
    }

    const auto dir = GameExeDirectory();
    if (dir.empty())
    {
        return {};
    }
    return dir / L"trial_caster_session.ini";
}

bool DiagnosticFileLoggingRequested()
{
    return log::FileLoggingRequested(ResolveSessionIniPath());
}

bool TryLoadSession(LauncherSession* out, std::filesystem::path* outIniPath)
{
    if (out == nullptr)
    {
        return false;
    }

    const auto path = ResolveSessionIniPath();
    if (path.empty())
    {
        return false;
    }

    if (!LoadSessionFromIni(path, out))
    {
        return false;
    }

    if (!controls::LoadFromSessionIni(path.wstring()))
    {
        log::Error("session", "launcher descriptor contains invalid shortcut bindings");
        return false;
    }

    if (outIniPath != nullptr)
    {
        *outIniPath = path;
    }

    log::Step(
        "session",
        "loaded ini=%s session_id=%ls launch_%lu caster_pid=%lu mode=%d bootstrap=%d compat=0x%08X autopunch=%d practice_keys=%d target_fps=%u",
        paths::PathUtf8(path).c_str(),
        out->sessionId.c_str(),
        out->launchIndex,
        out->casterPid,
        static_cast<int>(out->mode),
        static_cast<int>(out->bootstrap),
        out->compatibilityId,
        out->usedAutopunch ? 1 : 0,
        out->allowPracticeKeys ? 1 : 0,
        static_cast<unsigned>(out->targetFps));
    return true;
}

void ApplyToCasterBridge(const LauncherSession& session)
{
    g_cachedSession = session;
    g_hasCachedSession = true;

    const int localSide = session.localIsP1 ? 0 : 1;

    const bool pacingRejected = session.mode != LauncherMode::Spectator
        && !pacing::ConfigureSessionTargetFps(session.targetFps);
    const bool manualOffline = session.mode == LauncherMode::Offline
        && GetEnvironmentVariableW(kSessionIniEnvVar, nullptr, 0u) <= 1u;
    if (pacingRejected && manualOffline)
    {
        log::Warn("session", "manual offline load keeping stock pacing");
    }
    else if (pacingRejected)
    {
        log::Error(
            "session",
            "launcher session rejected: target_fps=%u cannot be paced exactly",
            static_cast<unsigned>(session.targetFps));
        lifecycle::Exit(
            "session",
            "requested frame rate is unavailable",
            "closing instead of running at a different game speed");
        log::FlushFile();
        const HWND window = ResolveGameWindow();
        if (window == nullptr || !PostMessageW(window, WM_CLOSE, 0u, 0))
        {
            ExitProcess(ERROR_NOT_SUPPORTED);
        }
        return;
    }

    switch (session.mode)
    {
    case LauncherMode::Host:
    case LauncherMode::Client:
        menu::SetOnlinePracticeKeysAllowed(session.allowPracticeKeys);
        lifecycle::Phase(
            "session",
            "idle",
            session.isHost ? "netplay_host" : "netplay_client",
            "ApplyToCasterBridge");
        netplay::BeginSession(session);
        log::Info(
            "session",
            "netplay %s session_id=%ls local=P%d peer=%ls port=%u delay=%u target_fps=%u",
            session.isHost ? "host" : "client",
            session.sessionId.c_str(),
            localSide + 1,
            session.peerIp.c_str(),
            static_cast<unsigned>(session.port),
            static_cast<unsigned>(session.inputDelay),
            static_cast<unsigned>(session.targetFps));
        break;
    case LauncherMode::Spectator:
        menu::SetOnlinePracticeKeysAllowed(false);
        lifecycle::Phase("session", "idle", "spectator", "ApplyToCasterBridge");
        // Apply the source RNG/FPU init before the VS Human transition.
        if (!spectator::player::Initialize(session))
        {
            log::Error("session", "spectator transport initialization failed");
            caster::SetTransportMode(caster::TransportMode::Offline);
            lifecycle::Exit(
                "spectator",
                "spectator transport initialization failed",
                "closing game instead of continuing offline");
            log::FlushFile();
            const HWND window = ResolveGameWindow();
            if (window == nullptr || !PostMessageW(window, WM_CLOSE, 0u, 0))
            {
                ExitProcess(ERROR_CONNECTION_ABORTED);
            }
        }
        else
        {
            log::Info(
                "session",
                "spectator session_id=%ls host=%ls port=%u",
                session.sessionId.c_str(),
                session.peerIp.c_str(),
                static_cast<unsigned>(session.peerPort != 0u ? session.peerPort : session.port));
        }
        break;
    default:
        menu::SetOnlinePracticeKeysAllowed(false);
        SetHumanSide(localSide);
        caster::SetTransportMode(caster::TransportMode::Offline);
        if (session.bootstrap == BootstrapMode::DebugVsHuman)
        {
            netplay::BeginDebugVsHuman(localSide);
            log::Info(
                "session",
                "debug VS Human bootstrap session_id=%ls local=P%d",
                session.sessionId.c_str(),
                localSide + 1);
        }
        else
        {
            log::Info(
                "session",
                "offline session_id=%ls local=P%d target_fps=%u",
                session.sessionId.c_str(),
                localSide + 1,
                static_cast<unsigned>(session.targetFps));
        }
        break;
    }
}

const LauncherSession* GetCachedSession()
{
    return g_hasCachedSession ? &g_cachedSession : nullptr;
}

}
