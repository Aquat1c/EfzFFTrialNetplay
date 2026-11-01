#include "session_file.h"

#include "control_bindings.h"
#include "protocol.h"
#include "session_log.h"
#include "trial_compatibility.h"
#include "util.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <vector>

namespace trial_caster
{
namespace
{

constexpr wchar_t kSessionIniEnvVar[] = L"TRIAL_CASTER_SESSION_INI";

const wchar_t* ModeName(SessionMode mode)
{
    switch (mode)
    {
    case SessionMode::Host:
        return L"host";
    case SessionMode::Client:
        return L"client";
    case SessionMode::Spectator:
        return L"spectator";
    default:
        return L"offline";
    }
}

const wchar_t* BootstrapName(BootstrapMode mode)
{
    switch (mode)
    {
    case BootstrapMode::DebugVsHuman:
        return L"debug_vs_human";
    default:
        return L"default";
    }
}

bool WriteIniValue(const std::wstring& file, const wchar_t* key, const std::wstring& value)
{
    return WritePrivateProfileStringW(
        L"session", key, value.c_str(), file.c_str()) != FALSE;
}

bool ReadIniEquals(
    const std::wstring& file,
    const wchar_t* const section,
    const wchar_t* const key,
    const std::wstring& expected)
{
    constexpr wchar_t kMissing[] = L"{TRIAL_NETPLAY_MISSING_VALUE}";
    std::vector<wchar_t> value((std::max<std::size_t>)(expected.size() + 2u, 128u), L'\0');
    const DWORD length = GetPrivateProfileStringW(
        section,
        key,
        kMissing,
        value.data(),
        static_cast<DWORD>(value.size()),
        file.c_str());
    if (length >= value.size() - 1u)
    {
        return false;
    }
    return std::wstring(value.data(), length) == expected;
}

bool VerifyControlSection(
    const std::wstring& file,
    const wchar_t* const section,
    const PlayerBindings& bindings)
{
    constexpr const wchar_t* kKeys[kFightSlotCount] = {
        L"up", L"down", L"left", L"right", L"a", L"b", L"c", L"d",
    };
    if (!ReadIniEquals(
            file,
            section,
            L"source",
            trialnet::input::SourceIniName(bindings.source)))
    {
        return false;
    }
    for (int index = 0; index < kFightSlotCount; ++index)
    {
        const std::size_t slot = static_cast<std::size_t>(index);
        if (!ReadIniEquals(file, section, kKeys[index], bindings.keys[slot])
            || !ReadIniEquals(
                file,
                section,
                (std::wstring(kKeys[index]) + L"_pad").c_str(),
                bindings.pad[slot]))
        {
            return false;
        }
    }
    return true;
}

bool VerifyShortcutSection(
    const std::wstring& file,
    const trialnet::shortcuts::Bindings& bindings)
{
    for (const trialnet::shortcuts::Descriptor& descriptor : trialnet::shortcuts::kDescriptors)
    {
        if (!ReadIniEquals(
                file,
                L"shortcuts",
                descriptor.iniKey,
                trialnet::shortcuts::NormalizeBindingToken(bindings[descriptor.action])))
        {
            return false;
        }
    }
    return true;
}

SYSTEMTIME LocalTimeNow()
{
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    return st;
}

}

SessionWriteResult WriteSessionFile(
    const std::filesystem::path& casterSessionDir,
    const SessionDescriptor& session)
{
    SessionWriteResult result{};
    result.launchIndex = session.launchIndex;
    result.sessionId = session.sessionId;

    if (casterSessionDir.empty() || session.launchIndex == 0 || session.sessionId.empty())
    {
        SessionLog::Error(L"session", L"WriteSessionFile invalid args");
        return result;
    }
    if (!proto::IsSupportedTargetFps(session.targetFps))
    {
        SessionLog::Error(
            L"session",
            L"WriteSessionFile rejected unsupported target_fps=%u",
            static_cast<unsigned>(session.targetFps));
        return result;
    }

    result.launchDir = casterSessionDir / util::Format(L"launch_%lu", session.launchIndex);
    result.iniPath = result.launchDir / L"session.ini";

    std::error_code ec;
    std::filesystem::create_directories(result.launchDir, ec);
    if (ec)
    {
        SessionLog::Error(L"session", L"Failed to create %s", result.launchDir.wstring().c_str());
        return result;
    }

    const std::wstring file = result.iniPath.wstring();
    std::filesystem::remove(result.iniPath, ec);
    ec.clear();
    const SYSTEMTIME st = LocalTimeNow();
    const std::wstring writtenAt = util::Format(
        L"%04u-%02u-%02uT%02u:%02u:%02u",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond);

    bool writeOk = true;
    const auto write = [&](const wchar_t* const key, const std::wstring& value) {
        writeOk = WriteIniValue(file, key, value) && writeOk;
    };
    write(L"session_id", session.sessionId);
    write(L"launch_index", std::to_wstring(session.launchIndex));
    write(L"caster_pid", std::to_wstring(session.casterPid));
    write(L"written_at", writtenAt);
    write(L"mode", ModeName(session.mode));
    write(L"bootstrap", BootstrapName(session.bootstrap));
    write(L"peer_ip", session.peerIp);
    write(L"player_name", session.playerName);
    write(L"peer_name", session.peerName);
    write(L"local_is_p1", session.localIsP1 ? L"1" : L"0");
    write(L"is_host", session.isHost ? L"1" : L"0");
    write(L"port", std::to_wstring(session.port));
    write(L"peer_port", std::to_wstring(session.peerPort));
    write(L"input_delay", std::to_wstring(session.inputDelay));
    write(L"rollback_frames", std::to_wstring(session.rollbackFrames));
    write(L"target_fps", std::to_wstring(session.targetFps));
    write(L"allow_practice_keys", session.allowPracticeKeys ? L"1" : L"0");
    write(L"diagnostic_logging", session.diagnosticLogging ? L"1" : L"0");
    write(L"ping_ms", std::to_wstring(session.pingMs));
    write(L"autopunch", session.usedAutopunch ? L"1" : L"0");
    write(L"compatibility_id", std::to_wstring(trialnet::compat::kCompatibilityId));
    write(
        L"compatibility_version",
        std::to_wstring(trialnet::compat::kCompatibilityVersion));
    write(L"rollback", session.requireRollback ? L"1" : L"0");
    writeOk = WriteControlsToSessionIni(file, session.controls) && writeOk;
    // Publish readiness only after every field/control binding has been accepted.
    writeOk = WriteIniValue(file, L"ready", L"1") && writeOk;
    // All-null call flushes the profile cache and returns zero; the readback decides.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, file.c_str());

    const bool verified = writeOk
        && ReadIniEquals(file, L"session", L"session_id", session.sessionId)
        && ReadIniEquals(file, L"session", L"launch_index", std::to_wstring(session.launchIndex))
        && ReadIniEquals(file, L"session", L"mode", ModeName(session.mode))
        && ReadIniEquals(file, L"session", L"bootstrap", BootstrapName(session.bootstrap))
        && ReadIniEquals(file, L"session", L"peer_ip", session.peerIp)
        && ReadIniEquals(file, L"session", L"local_is_p1", session.localIsP1 ? L"1" : L"0")
        && ReadIniEquals(file, L"session", L"is_host", session.isHost ? L"1" : L"0")
        && ReadIniEquals(file, L"session", L"port", std::to_wstring(session.port))
        && ReadIniEquals(file, L"session", L"peer_port", std::to_wstring(session.peerPort))
        && ReadIniEquals(file, L"session", L"input_delay", std::to_wstring(session.inputDelay))
        && ReadIniEquals(file, L"session", L"rollback_frames", std::to_wstring(session.rollbackFrames))
        && ReadIniEquals(file, L"session", L"target_fps", std::to_wstring(session.targetFps))
        && ReadIniEquals(
            file,
            L"session",
            L"allow_practice_keys",
            session.allowPracticeKeys ? L"1" : L"0")
        && ReadIniEquals(
            file,
            L"session",
            L"diagnostic_logging",
            session.diagnosticLogging ? L"1" : L"0")
        && ReadIniEquals(
            file,
            L"session",
            L"compatibility_id",
            std::to_wstring(trialnet::compat::kCompatibilityId))
        && ReadIniEquals(
            file,
            L"session",
            L"compatibility_version",
            std::to_wstring(trialnet::compat::kCompatibilityVersion))
        && ReadIniEquals(file, L"session", L"ready", L"1")
        && VerifyControlSection(file, L"controls_p1", session.controls.p1)
        && VerifyControlSection(file, L"controls_p2", session.controls.p2)
        && VerifyShortcutSection(file, session.controls.shortcuts);
    if (!verified)
    {
        SessionLog::Error(
            L"session",
            L"Session handoff write/readback failed; refusing to launch %s",
            result.iniPath.wstring().c_str());
        std::filesystem::remove(result.iniPath, ec);
        return result;
    }

    if (SessionLog::IsFileLoggingEnabled())
    {
        const auto manifestPath = SessionLog::LogDirectory() / L"launches.tsv";
        const bool manifestExists = std::filesystem::exists(manifestPath);
        std::wofstream manifest(manifestPath, std::ios::app);
        if (manifest)
        {
            if (!manifestExists)
            {
                manifest << L"status\tlaunch_index\tsession_id\tmode\tefz_pid\tini_path\ttimestamp\n";
            }
            manifest << L"planned\t"
                     << session.launchIndex << L'\t'
                     << session.sessionId << L'\t'
                     << ModeName(session.mode) << L'\t'
                     << L"0\t"
                     << result.iniPath.wstring() << L'\t'
                     << writtenAt << L'\n';
        }
    }

    SessionLog::Step(
        L"session",
        L"Wrote launch_%lu session_id=%s mode=%s bootstrap=%s ini=%s",
        session.launchIndex,
        session.sessionId.c_str(),
        ModeName(session.mode),
        BootstrapName(session.bootstrap),
        result.iniPath.wstring().c_str());
    SessionLog::Info(L"session", L"Child env %s=%s", kSessionIniEnvVar, result.iniPath.wstring().c_str());

    result.ok = true;
    return result;
}

void RecordLaunchOutcome(
    const std::filesystem::path& casterSessionDir,
    const SessionWriteResult& write,
    const SessionMode mode,
    const unsigned long efzPid,
    const bool ok)
{
    if (casterSessionDir.empty()
        || !write.ok
        || !SessionLog::IsFileLoggingEnabled())
    {
        return;
    }

    const SYSTEMTIME st = LocalTimeNow();
    const std::wstring stamp = util::Format(
        L"%04u-%02u-%02uT%02u:%02u:%02u",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond);

    std::wofstream manifest(SessionLog::LogDirectory() / L"launches.tsv", std::ios::app);
    if (!manifest)
    {
        return;
    }

    manifest << (ok ? L"launched\t" : L"failed\t")
             << write.launchIndex << L'\t'
             << write.sessionId << L'\t'
             << ModeName(mode) << L'\t'
             << efzPid << L'\t'
             << write.iniPath.wstring() << L'\t'
             << stamp << L'\n';

    SessionLog::Step(
        L"session",
        L"Recorded launch_%lu outcome=%s efzPid=%lu",
        write.launchIndex,
        ok ? L"ok" : L"fail",
        efzPid);
}

void DeleteSessionFile(const std::filesystem::path& gameDir)
{
    const auto path = gameDir / L"trial_caster_session.ini";
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}
