#include "caster_config.h"
#include "compatibility_runtime.h"
#include "net_session.h"
#include "protocol.h"
#include "session_file.h"
#include "session_log.h"
#include "trial_compatibility.h"
#include "util.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cwchar>
#include <thread>
#include <vector>

namespace
{

constexpr std::uint16_t kTestPort = 37991u;
constexpr std::uint16_t kBusyTestPort = 37992u;
constexpr std::uint16_t kHostOptOutTestPort = 37994u;
constexpr std::uint16_t kClientOptOutTestPort = 37995u;

std::vector<std::wstring> ParseEnvironmentBlock(const std::vector<wchar_t>& block)
{
    std::vector<std::wstring> entries;
    if (block.empty())
    {
        return entries;
    }
    for (const wchar_t* cursor = block.data(); *cursor != L'\0';)
    {
        entries.emplace_back(cursor);
        cursor += entries.back().size() + 1u;
    }
    return entries;
}

bool VerifyEnvironmentAndWineOverrides()
{
    SetEnvironmentVariableW(L"TRIAL_ENV_INHERIT_TEST", L"preserved");
    SetEnvironmentVariableW(L"TRIAL_ENV_REPLACE_TEST", L"old");
    SetEnvironmentVariableW(L"TRIAL_CASTER_SESSION_INI", L"C:\\stale\\session.ini");
    const std::vector<wchar_t> block = trial_caster::util::BuildChildEnvironmentBlock({
        {L"TRIAL_ENV_REPLACE_TEST", L"new"},
        {L"TRIAL_CASTER_SESSION_INI", L"C:\\session path\\session.ini"},
    });
    SetEnvironmentVariableW(L"TRIAL_ENV_INHERIT_TEST", nullptr);
    SetEnvironmentVariableW(L"TRIAL_ENV_REPLACE_TEST", nullptr);
    SetEnvironmentVariableW(L"TRIAL_CASTER_SESSION_INI", nullptr);
    if (block.size() < 2u || block[block.size() - 1u] != L'\0'
        || block[block.size() - 2u] != L'\0')
    {
        return false;
    }

    const std::vector<std::wstring> entries = ParseEnvironmentBlock(block);
    bool inherited = false;
    bool replaced = false;
    bool stale = false;
    bool session = false;
    bool casterOnly = false;
    for (std::size_t index = 0u; index < entries.size(); ++index)
    {
        inherited = inherited || entries[index] == L"TRIAL_ENV_INHERIT_TEST=preserved";
        replaced = replaced || entries[index] == L"TRIAL_ENV_REPLACE_TEST=new";
        stale = stale || entries[index] == L"TRIAL_ENV_REPLACE_TEST=old";
        session = session
            || entries[index] == L"TRIAL_CASTER_SESSION_INI=C:\\session path\\session.ini";
        casterOnly = casterOnly || entries[index] == L"TRIAL_CASTER_ONLY=1";
        if (index > 0u && _wcsicmp(entries[index - 1u].c_str(), entries[index].c_str()) > 0)
        {
            return false;
        }
    }
    if (!inherited || !replaced || stale || !session || !casterOnly)
    {
        return false;
    }

    SetEnvironmentVariableW(L"TRIAL_CASTER_SESSION_INI", L"C:\\stale\\session.ini");
    const std::vector<std::wstring> legacyEntries = ParseEnvironmentBlock(
        trial_caster::util::BuildChildEnvironmentBlock(L"TRIAL_CASTER_ONLY", L"1"));
    SetEnvironmentVariableW(L"TRIAL_CASTER_SESSION_INI", nullptr);
    if (std::any_of(legacyEntries.begin(), legacyEntries.end(), [](const std::wstring& entry) {
            return _wcsnicmp(
                entry.c_str(),
                L"TRIAL_CASTER_SESSION_INI=",
                std::wcslen(L"TRIAL_CASTER_SESSION_INI=")) == 0;
        }))
    {
        return false;
    }

    return trial_caster::util::EnsureNativeDirectDrawOverride(
               L"d3d9=b;ddraw=b;xaudio2_7=n,b")
            == L"d3d9=b;xaudio2_7=n,b;ddraw=n,b"
        && trial_caster::util::EnsureNativeDirectDrawOverride(
               L"ddraw.dll,dinput8=b;version=n")
            == L"dinput8=b;version=n;ddraw=n,b"
        && trial_caster::util::EnsureNativeDirectDrawOverride(
               L"*ddraw=b;d3dcompiler_47=n")
            == L"d3dcompiler_47=n;ddraw=n,b"
        && trial_caster::util::EnsureNativeDirectDrawOverride(L"") == L"ddraw=n,b";
}

bool VerifyWritableStateRoot()
{
    wchar_t temporaryDirectory[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temporaryDirectory) == 0u)
    {
        return false;
    }
    const std::filesystem::path state = std::filesystem::path(temporaryDirectory)
        / (L"trial_state_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(state, ignored);
    SetEnvironmentVariableW(L"TRIAL_NETPLAY_STATE_DIR", state.c_str());
    const std::filesystem::path resolved =
        trial_caster::util::ResolveWritableStateDirectory(L"Z:\\missing\\readonly");
    SetEnvironmentVariableW(L"TRIAL_NETPLAY_STATE_DIR", nullptr);
    const bool valid = resolved == state && std::filesystem::is_directory(state)
        && !std::filesystem::exists(
            state / (L".trial_write_probe_" + std::to_wstring(GetCurrentProcessId())));
    std::filesystem::remove_all(state, ignored);
    if (!valid)
    {
        return false;
    }

    const std::filesystem::path relative =
        L"trial_relative_state_" + std::to_wstring(GetCurrentProcessId());
    const std::filesystem::path expected =
        std::filesystem::absolute(relative).lexically_normal();
    std::filesystem::remove_all(expected, ignored);
    SetEnvironmentVariableW(L"TRIAL_NETPLAY_STATE_DIR", relative.c_str());
    const std::filesystem::path relativeResolved =
        trial_caster::util::ResolveWritableStateDirectory(L"Z:\\missing\\readonly");
    SetEnvironmentVariableW(L"TRIAL_NETPLAY_STATE_DIR", nullptr);
    const bool relativeValid = relativeResolved == expected
        && relativeResolved.is_absolute()
        && std::filesystem::is_directory(expected);
    std::filesystem::remove_all(expected, ignored);
    return relativeValid;
}

bool VerifySessionHandoffPersistence()
{
    wchar_t temporaryDirectory[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temporaryDirectory) == 0u)
    {
        return false;
    }
    const std::filesystem::path root = std::filesystem::path(temporaryDirectory)
        / (L"trial_session_handoff_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    trial_caster::SessionDescriptor descriptor{};
    descriptor.mode = trial_caster::SessionMode::Client;
    descriptor.sessionId = L"wine_handoff_test";
    descriptor.launchIndex = 1u;
    descriptor.casterPid = GetCurrentProcessId();
    descriptor.peerIp = L"127.0.0.1";
    descriptor.peerPort = 38123u;
    descriptor.inputDelay = 3u;
    descriptor.rollbackFrames = 9u;
    descriptor.targetFps = 64u;
    descriptor.allowPracticeKeys = true;
    descriptor.diagnosticLogging = true;
    descriptor.requireRollback = true;
    descriptor.controls = trial_caster::DefaultControlBindings();
    descriptor.controls.shortcuts[trialnet::shortcuts::Action::Pause] = L"F10";
    const trial_caster::SessionWriteResult written =
        trial_caster::WriteSessionFile(root, descriptor);

    wchar_t ready[8] = {};
    wchar_t source[32] = {};
    wchar_t practiceKeys[8] = {};
    wchar_t targetFps[8] = {};
    wchar_t diagnosticLogging[8] = {};
    wchar_t pauseShortcut[32] = {};
    const bool valid = written.ok && std::filesystem::is_regular_file(written.iniPath)
        && GetPrivateProfileStringW(
            L"session", L"ready", L"", ready, static_cast<DWORD>(std::size(ready)), written.iniPath.c_str()) == 1u
        && std::wstring(ready) == L"1"
        && GetPrivateProfileStringW(
            L"session",
            L"allow_practice_keys",
            L"",
            practiceKeys,
            static_cast<DWORD>(std::size(practiceKeys)),
            written.iniPath.c_str()) == 1u
        && std::wstring(practiceKeys) == L"1"
        && GetPrivateProfileStringW(
            L"session",
            L"target_fps",
            L"",
            targetFps,
            static_cast<DWORD>(std::size(targetFps)),
            written.iniPath.c_str()) == 2u
        && std::wstring(targetFps) == L"64"
        && GetPrivateProfileStringW(
            L"session",
            L"diagnostic_logging",
            L"",
            diagnosticLogging,
            static_cast<DWORD>(std::size(diagnosticLogging)),
            written.iniPath.c_str()) == 1u
        && std::wstring(diagnosticLogging) == L"1"
        && GetPrivateProfileStringW(
            L"controls_p1", L"source", L"", source, static_cast<DWORD>(std::size(source)), written.iniPath.c_str()) > 0u
        && std::wstring(source) == L"keyboard"
        && GetPrivateProfileStringW(
            L"shortcuts",
            L"pause",
            L"",
            pauseShortcut,
            static_cast<DWORD>(std::size(pauseShortcut)),
            written.iniPath.c_str()) > 0u
        && std::wstring(pauseShortcut) == L"F10";
    descriptor.sessionId = L"invalid_fps_handoff";
    descriptor.launchIndex = 2u;
    descriptor.targetFps = 63u;
    const bool invalidRejected = !trial_caster::WriteSessionFile(root, descriptor).ok;
    std::filesystem::remove_all(root, ignored);
    return valid && invalidRejected;
}

bool VerifyDiagnosticLoggingOptIn()
{
    if (trial_caster::CasterConfig{}.diagnosticLogging)
    {
        return false;
    }
    wchar_t temporaryDirectory[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temporaryDirectory) == 0u)
    {
        return false;
    }
    const std::filesystem::path root = std::filesystem::path(temporaryDirectory)
        / (L"trial_launcher_logging_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    const bool silentInitialized = trial_caster::SessionLog::Initialize(root / L"silent", false);
    const std::filesystem::path silentSession = trial_caster::SessionLog::SessionDirectory();
    trial_caster::SessionLog::Step(L"test", L"must remain in memory only");
    trial_caster::SessionDescriptor silentDescriptor{};
    silentDescriptor.sessionId = L"silent_handoff";
    silentDescriptor.launchIndex = 1u;
    silentDescriptor.casterPid = GetCurrentProcessId();
    const trial_caster::SessionWriteResult silentWrite =
        trial_caster::WriteSessionFile(silentSession, silentDescriptor);
    const bool silent = silentInitialized
        && !trial_caster::SessionLog::IsFileLoggingEnabled()
        && std::filesystem::is_directory(silentSession)
        && silentWrite.ok
        && std::filesystem::is_regular_file(silentWrite.iniPath)
        && trial_caster::SessionLog::LogDirectory().empty()
        && trial_caster::SessionLog::LogFilePath().empty()
        && !std::filesystem::exists(root / L"silent" / L"logs");
    trial_caster::SessionLog::Shutdown();

    const bool enabledInitialized = trial_caster::SessionLog::Initialize(root / L"enabled", true);
    const std::filesystem::path enabledSession = trial_caster::SessionLog::SessionDirectory();
    const std::filesystem::path enabledLogDirectory = trial_caster::SessionLog::LogDirectory();
    const std::filesystem::path enabledLog = trial_caster::SessionLog::LogFilePath();
    trial_caster::SessionLog::Step(L"test", L"explicit diagnostic opt-in");
    trial_caster::SessionDescriptor enabledDescriptor{};
    enabledDescriptor.sessionId = L"logged_handoff";
    enabledDescriptor.launchIndex = 1u;
    enabledDescriptor.casterPid = GetCurrentProcessId();
    enabledDescriptor.diagnosticLogging = true;
    const trial_caster::SessionWriteResult enabledWrite =
        trial_caster::WriteSessionFile(enabledSession, enabledDescriptor);
    trial_caster::RecordLaunchOutcome(
        enabledSession,
        enabledWrite,
        enabledDescriptor.mode,
        1234u,
        enabledWrite.ok);
    trial_caster::SessionLog::Flush();
    const bool enabled = enabledInitialized
        && trial_caster::SessionLog::IsFileLoggingEnabled()
        && std::filesystem::is_directory(enabledSession)
        && std::filesystem::is_directory(enabledLogDirectory)
        && enabledWrite.ok
        && !enabledLog.empty()
        && std::filesystem::is_regular_file(enabledLog)
        && std::filesystem::is_regular_file(enabledLogDirectory / L"launches.tsv");
    trial_caster::SessionLog::Shutdown();

    std::filesystem::remove_all(root, ignored);
    return silent && enabled;
}

bool VerifyCompatibilityInspection(const wchar_t* const dllPath)
{
    if (dllPath == nullptr || dllPath[0] == L'\0')
    {
        return false;
    }
    std::uint32_t compatibility = 0u;
    std::wstring error;
    if (!trial_caster::util::ReadNetplayDllCompatibility(
            dllPath,
            &compatibility,
            &error)
        || compatibility != trialnet::compat::kCompatibilityId)
    {
        std::fwprintf(
            stderr,
            L"compatibility inspection failed: path=%ls read=0x%08X expected=0x%08X error=%ls\n",
            dllPath,
            compatibility,
            trialnet::compat::kCompatibilityId,
            error.c_str());
        return false;
    }

    wchar_t temporaryDirectory[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temporaryDirectory) == 0u)
    {
        return false;
    }
    const std::filesystem::path invalid = std::filesystem::path(temporaryDirectory)
        / (L"trial_invalid_dll_" + std::to_wstring(GetCurrentProcessId()) + L".bin");
    {
        std::ofstream output(invalid, std::ios::binary);
        output << "not a PE";
    }
    compatibility = 123u;
    error.clear();
    const bool rejected = !trial_caster::util::ReadNetplayDllCompatibility(
        invalid,
        &compatibility,
        &error);
    std::error_code ignored;
    std::filesystem::remove(invalid, ignored);
    return rejected && compatibility == 0u && !error.empty();
}

bool VerifyDirectDrawInspection(
    const wchar_t* const directDrawPath,
    const wchar_t* const genericDllPath)
{
    std::wstring error;
    if (directDrawPath == nullptr || directDrawPath[0] == L'\0'
        || !trial_caster::util::ValidateEfzDirectDrawWrapper(directDrawPath, &error)
        || !error.empty())
    {
        return false;
    }
    error.clear();
    const bool rejectedGeneric = genericDllPath != nullptr
        && !trial_caster::util::ValidateEfzDirectDrawWrapper(genericDllPath, &error)
        && !error.empty();
    if (!rejectedGeneric)
    {
        return false;
    }

    wchar_t temporaryDirectory[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temporaryDirectory) == 0u)
    {
        return false;
    }
    const std::filesystem::path stale = std::filesystem::path(temporaryDirectory)
        / (L"trial_stale_ddraw_" + std::to_wstring(GetCurrentProcessId()) + L".dll");
    std::error_code ignored;
    std::filesystem::copy_file(
        directDrawPath,
        stale,
        std::filesystem::copy_options::overwrite_existing,
        ignored);
    if (ignored)
    {
        return false;
    }
    std::fstream image(stale, std::ios::in | std::ios::out | std::ios::binary);
    const std::string bytes{
        std::istreambuf_iterator<char>(image),
        std::istreambuf_iterator<char>()};
    constexpr char kRequiredExport[] = "EFZDDRAW_SetFrameLimit";
    const std::size_t exportOffset = bytes.find(kRequiredExport);
    bool rejectedStale = false;
    if (exportOffset != std::string::npos)
    {
        image.clear();
        image.seekp(static_cast<std::streamoff>(exportOffset));
        image.put('X');
        image.flush();
        image.close();
        error.clear();
        rejectedStale = !trial_caster::util::ValidateEfzDirectDrawWrapper(stale, &error)
            && !error.empty();
    }
    else
    {
        image.close();
    }
    std::filesystem::remove(stale, ignored);
    return rejectedStale;
}

bool VerifyEfzLayoutContract(const wchar_t* const efzPath)
{
    std::wstring error;
    if (efzPath == nullptr
        || !trial_caster::util::ValidateEfzExecutableLayout(efzPath, &error)
        || !error.empty())
    {
        return false;
    }

    wchar_t temporaryDirectory[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temporaryDirectory) == 0u)
    {
        return false;
    }
    const std::filesystem::path mutated = std::filesystem::path(temporaryDirectory)
        / (L"trial_mutated_efz_" + std::to_wstring(GetCurrentProcessId()) + L".exe");
    std::error_code ignored;
    const auto rejectsMutation = [&](const auto& mutate) {
        ignored.clear();
        std::filesystem::copy_file(
            efzPath,
            mutated,
            std::filesystem::copy_options::overwrite_existing,
            ignored);
        if (ignored)
        {
            return false;
        }
        std::fstream image(mutated, std::ios::in | std::ios::out | std::ios::binary);
        IMAGE_DOS_HEADER dos{};
        if (!image.read(reinterpret_cast<char*>(&dos), sizeof(dos))
            || dos.e_magic != IMAGE_DOS_SIGNATURE)
        {
            return false;
        }
        image.seekg(dos.e_lfanew, std::ios::beg);
        IMAGE_NT_HEADERS32 nt{};
        if (!image.read(reinterpret_cast<char*>(&nt), sizeof(nt))
            || nt.Signature != IMAGE_NT_SIGNATURE)
        {
            return false;
        }
        mutate(nt);
        image.seekp(dos.e_lfanew, std::ios::beg);
        image.write(reinterpret_cast<const char*>(&nt), sizeof(nt));
        image.close();
        error.clear();
        return !trial_caster::util::ValidateEfzExecutableLayout(mutated, &error)
            && !error.empty();
    };
    const bool mutationsRejected =
        rejectsMutation([](IMAGE_NT_HEADERS32& nt) {
            nt.OptionalHeader.AddressOfEntryPoint ^= 0x10u;
        })
        && rejectsMutation([](IMAGE_NT_HEADERS32& nt) {
            nt.FileHeader.Characteristics &=
                static_cast<WORD>(~IMAGE_FILE_RELOCS_STRIPPED);
        })
        && rejectsMutation([](IMAGE_NT_HEADERS32& nt) {
            IMAGE_DATA_DIRECTORY& relocations =
                nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            relocations.VirtualAddress = 0x1000u;
            relocations.Size = sizeof(DWORD);
        });
    std::filesystem::remove(mutated, ignored);
    if (!mutationsRejected)
    {
        return false;
    }

    const std::filesystem::path invalid = std::filesystem::path(temporaryDirectory)
        / (L"trial_invalid_efz_" + std::to_wstring(GetCurrentProcessId()) + L".exe");
    {
        std::ofstream output(invalid, std::ios::binary);
        output << "not EFZ";
    }
    error.clear();
    const bool rejected = !trial_caster::util::ValidateEfzExecutableLayout(invalid, &error);
    std::filesystem::remove(invalid, ignored);
    return rejected && !error.empty();
}

bool VerifyDestinationParsing()
{
    trial_caster::util::ConnectDestination destination{};
    if (!trial_caster::util::ParseConnectDestination(L"127.0.0.1", 7500u, &destination)
        || destination.host != L"127.0.0.1" || destination.port != 7500u
        || destination.explicitPort
        || trial_caster::util::FormatConnectDestination(destination) != L"127.0.0.1")
    {
        return false;
    }
    if (!trial_caster::util::ParseConnectDestination(L" 127.0.0.1 : 8123 ", 7500u, &destination)
        || destination.host != L"127.0.0.1" || destination.port != 8123u
        || !destination.explicitPort
        || trial_caster::util::FormatConnectDestination(destination) != L"127.0.0.1:8123")
    {
        return false;
    }
    if (!trial_caster::util::ParseConnectDestination(L" LOCALHOST:8123 ", 7500u, &destination)
        || destination.host != L"localhost" || destination.port != 8123u
        || !destination.explicitPort
        || trial_caster::util::FormatConnectDestination(destination) != L"localhost:8123")
    {
        return false;
    }
    if (!trial_caster::util::ParseConnectDestination(L"match.example.test", 7500u, &destination)
        || destination.host != L"match.example.test" || destination.port != 7500u
        || destination.explicitPort)
    {
        return false;
    }
    if (trial_caster::util::ResolveConnectHostIpv4(L"localhost") != L"127.0.0.1")
    {
        return false;
    }
    return !trial_caster::util::ParseConnectDestination(L"127.0.0.1:0", 7500u, &destination)
        && !trial_caster::util::ParseConnectDestination(L"127.0.0.1:65536", 7500u, &destination)
        && !trial_caster::util::ParseConnectDestination(L"127.0.0.1:abc", 7500u, &destination)
        && !trial_caster::util::ParseConnectDestination(L"999.1.1.1", 7500u, &destination)
        && !trial_caster::util::ParseConnectDestination(L"bad host", 7500u, &destination)
        && !trial_caster::util::ParseConnectDestination(L"[::1]:7500", 7500u, &destination);
}

bool VerifyOutboundRecentSemantics()
{
    wchar_t temporaryDirectory[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temporaryDirectory) == 0u)
    {
        return false;
    }
    const std::filesystem::path ini = std::filesystem::path(temporaryDirectory)
        / (L"trial_launcher_recents_" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    std::error_code ignored;
    std::filesystem::remove(ini, ignored);

    trial_caster::ConfigStore store(ini);
    store.Mutable().port = 7500u;
    store.Mutable().uiScalePercent = 135u;
    store.SetLastConnectIp(L"127.0.0.2");
    if (!store.Get().recentConnectIps.empty())
    {
        std::filesystem::remove(ini, ignored);
        return false;
    }
    store.RememberSuccessfulConnect(L"127.0.0.1:8123");
    store.SetLastConnectIp(L"127.0.0.3");
    if (store.Get().recentConnectIps.size() != 1u
        || store.Get().recentConnectIps.front() != L"127.0.0.1:8123")
    {
        std::filesystem::remove(ini, ignored);
        return false;
    }

    trial_caster::ConfigStore reloaded(ini);
    const bool loaded = reloaded.Load();
    const bool valid = loaded && reloaded.Get().lastConnectIp == L"127.0.0.3"
        && reloaded.Get().uiScalePercent == 135u
        && reloaded.Get().recentConnectIps.size() == 1u
        && reloaded.Get().recentConnectIps.front() == L"127.0.0.1:8123";
    std::filesystem::remove(ini, ignored);
    return valid;
}

bool VerifyPortPersistenceAndValidation()
{
    wchar_t temporaryDirectory[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temporaryDirectory) == 0u)
    {
        return false;
    }
    const std::filesystem::path ini = std::filesystem::path(temporaryDirectory)
        / (L"trial_launcher_port_" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    std::error_code ignored;
    std::filesystem::remove(ini, ignored);

    trial_caster::ConfigStore saved(ini);
    saved.Mutable().port = 38123u;
    saved.Mutable().targetFps = 64u;
    saved.Mutable().allowPracticeKeys = true;
    saved.Mutable().diagnosticLogging = true;
    saved.Mutable().controls.shortcuts[trialnet::shortcuts::Action::StepFrame] = L"F12";
    if (!saved.Save())
    {
        std::filesystem::remove(ini, ignored);
        return false;
    }
    trial_caster::ConfigStore reloaded(ini);
    if (!reloaded.Load() || reloaded.Get().port != 38123u
        || reloaded.Get().targetFps != 64u
        || !reloaded.Get().allowPracticeKeys
        || !reloaded.Get().diagnosticLogging
        || reloaded.Get().controls.shortcuts[trialnet::shortcuts::Action::StepFrame] != L"F12")
    {
        std::filesystem::remove(ini, ignored);
        return false;
    }

    WritePrivateProfileStringW(L"net", L"port", L"0", ini.wstring().c_str());
    trial_caster::ConfigStore zeroPort(ini);
    if (!zeroPort.Load() || zeroPort.Get().port != 7500u)
    {
        std::filesystem::remove(ini, ignored);
        return false;
    }
    WritePrivateProfileStringW(L"net", L"port", L"70000", ini.wstring().c_str());
    trial_caster::ConfigStore oversizedPort(ini);
    const bool oversizedValid = oversizedPort.Load() && oversizedPort.Get().port == 7500u;

    WritePrivateProfileStringW(L"net", L"allow_practice_keys", L"2", ini.wstring().c_str());
    WritePrivateProfileStringW(L"net", L"max_retry", L"9999", ini.wstring().c_str());
    WritePrivateProfileStringW(L"net", L"packet_resend", L"9999", ini.wstring().c_str());
    WritePrivateProfileStringW(L"net", L"timeout_ms", L"999999", ini.wstring().c_str());
    WritePrivateProfileStringW(L"net", L"target_fps", L"63", ini.wstring().c_str());
    WritePrivateProfileStringW(L"diagnostics", L"file_logging", L"2", ini.wstring().c_str());
    trial_caster::ConfigStore malformed(ini);
    const bool strictValid = malformed.Load()
        && !malformed.Get().allowPracticeKeys
        && !malformed.Get().diagnosticLogging
        && malformed.Get().targetFps == trial_caster::proto::kDefaultTargetFps
        && malformed.Get().maxRetryCount == trial_caster::proto::kMaxRetryCount
        && malformed.Get().packetResendCount == trial_caster::proto::kMaxOverlappedPacket
        && malformed.Get().maxTimeoutMs == 60000u;
    malformed.Mutable().targetFps = 63u;
    const bool invalidSaveRejected = !malformed.Save();
    std::filesystem::remove(ini, ignored);
    return oversizedValid && strictValid && invalidSaveRejected;
}

bool VerifyConflictingControlSaveRejected()
{
    wchar_t temporaryDirectory[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temporaryDirectory) == 0u)
    {
        return false;
    }
    const std::filesystem::path ini = std::filesystem::path(temporaryDirectory)
        / (L"trial_launcher_conflict_" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    std::error_code ignored;
    std::filesystem::remove(ini, ignored);

    trial_caster::ConfigStore store(ini);
    const auto action = trialnet::shortcuts::Action::TrainingMode;
    const std::wstring defaultShortcut = store.Get().controls.shortcuts[action];
    store.Mutable().controls.shortcuts[action] = store.Get().controls.p1.keys[4u];
    const bool rejected = !store.Save();

    store.Mutable().controls.shortcuts[action] = defaultShortcut;
    const bool restored = store.Save();
    std::filesystem::remove(ini, ignored);
    return rejected && restored;
}

bool SessionPracticePolicyEquals(
    const std::filesystem::path& ini,
    const bool expected)
{
    wchar_t value[8] = {};
    return GetPrivateProfileStringW(
               L"session",
               L"allow_practice_keys",
               L"",
               value,
               static_cast<DWORD>(std::size(value)),
               ini.c_str()) == 1u
        && std::wstring(value) == (expected ? L"1" : L"0");
}

bool SessionTargetFpsEquals(
    const std::filesystem::path& ini,
    const std::uint8_t expected)
{
    wchar_t value[8] = {};
    return GetPrivateProfileStringW(
               L"session",
               L"target_fps",
               L"",
               value,
               static_cast<DWORD>(std::size(value)),
               ini.c_str()) > 0u
        && std::wstring(value) == std::to_wstring(expected);
}

bool WriteNegotiatedPracticeHandoff(
    const std::filesystem::path& root,
    const wchar_t* const sessionId,
    const unsigned long launchIndex,
    const trial_caster::SessionMode mode,
    const trial_caster::CasterConfig& config,
    const trial_caster::NetSessionInfo& info)
{
    trial_caster::SessionDescriptor descriptor{};
    descriptor.mode = mode;
    descriptor.localIsP1 = info.localIsP1;
    descriptor.isHost = info.isHost;
    descriptor.port = config.port;
    descriptor.peerPort = info.peerPort;
    descriptor.inputDelay = info.inputDelay;
    descriptor.rollbackFrames = config.rollbackFrames;
    descriptor.targetFps = info.targetFps;
    descriptor.allowPracticeKeys = info.allowPracticeKeys;
    descriptor.diagnosticLogging = config.diagnosticLogging;
    descriptor.pingMs = info.averageLatencyMs;
    descriptor.usedAutopunch = info.connectionMethod == trial_caster::ConnectionMethod::AutoPunch;
    descriptor.peerIp = info.peerIp;
    descriptor.peerName = info.peerName;
    descriptor.playerName = config.playerName;
    descriptor.sessionId = sessionId;
    descriptor.launchIndex = launchIndex;
    descriptor.casterPid = GetCurrentProcessId();
    descriptor.requireRollback = true;
    descriptor.controls = config.controls;

    const trial_caster::SessionWriteResult written =
        trial_caster::WriteSessionFile(root, descriptor);
    return written.ok
        && SessionPracticePolicyEquals(written.iniPath, false)
        && SessionTargetFpsEquals(written.iniPath, info.targetFps);
}

bool VerifyOneSidedPracticeOptOut(
    const bool hostAllowsPractice,
    const bool clientAllowsPractice,
    const std::uint16_t port)
{
    trial_caster::CasterConfig hostConfig{};
    hostConfig.port = port;
    hostConfig.packetResendCount = 1u;
    hostConfig.maxTimeoutMs = 2000u;
    hostConfig.playerName = L"OptOutHost";
    hostConfig.allowPracticeKeys = hostAllowsPractice;

    trial_caster::CasterConfig clientConfig = hostConfig;
    clientConfig.port = static_cast<std::uint16_t>(port + 100u);
    clientConfig.playerName = L"OptOutClient";
    clientConfig.allowPracticeKeys = clientAllowsPractice;

    trial_caster::NetSession host(hostConfig);
    trial_caster::NetSession client(clientConfig);
    trial_caster::NetSessionInfo hostInfo{};
    trial_caster::NetSessionInfo clientInfo{};
    std::atomic<trial_caster::NetResult> hostResult{trial_caster::NetResult::Denied};
    std::thread hostThread([&]() { hostResult.store(host.HostHandshake(&hostInfo)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const trial_caster::NetResult clientResult =
        client.ConnectHandshake(L"127.0.0.1", &clientInfo, port);
    hostThread.join();
    host.Close();
    client.Close();

    if (hostResult.load() != trial_caster::NetResult::Ok
        || clientResult != trial_caster::NetResult::Ok
        || hostInfo.allowPracticeKeys
        || clientInfo.allowPracticeKeys)
    {
        return false;
    }

    wchar_t temporaryDirectory[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temporaryDirectory) == 0u)
    {
        return false;
    }
    const std::filesystem::path root = std::filesystem::path(temporaryDirectory)
        / (L"trial_practice_optout_" + std::to_wstring(GetCurrentProcessId())
            + L"_" + std::to_wstring(port));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    const bool hostHandoff = WriteNegotiatedPracticeHandoff(
        root / L"host",
        L"practice_optout_host",
        1u,
        trial_caster::SessionMode::Host,
        hostConfig,
        hostInfo);
    const bool clientHandoff = WriteNegotiatedPracticeHandoff(
        root / L"client",
        L"practice_optout_client",
        1u,
        trial_caster::SessionMode::Client,
        clientConfig,
        clientInfo);
    std::filesystem::remove_all(root, ignored);
    return hostHandoff && clientHandoff;
}

bool VerifyStaleVersionRejected()
{
    SOCKET const socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET)
    {
        return false;
    }

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(kTestPort);
    InetPtonA(AF_INET, "127.0.0.1", &target.sin_addr);

    trial_caster::proto::ConnectionRequest request{};
    request.minorVersion = static_cast<std::uint16_t>(trial_caster::proto::kVerMinor - 1u);
    const int sent = sendto(
        socketHandle,
        reinterpret_cast<const char*>(&request),
        sizeof(request),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target));
    if (sent != sizeof(request))
    {
        closesocket(socketHandle);
        return false;
    }

    fd_set readSet{};
    FD_ZERO(&readSet);
    FD_SET(socketHandle, &readSet);
    timeval timeout{};
    timeout.tv_sec = 2;
    const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
    std::uint8_t reply = 0xFFu;
    if (ready > 0)
    {
        recvfrom(socketHandle, reinterpret_cast<char*>(&reply), sizeof(reply), 0, nullptr, nullptr);
    }
    closesocket(socketHandle);
    return ready > 0 && reply == trial_caster::proto::kReplyVerNotMatch;
}

bool VerifyAlreadyStartedReply()
{
    SOCKET const hostSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (hostSocket == INVALID_SOCKET)
    {
        return false;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(kBusyTestPort);
    local.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
    if (bind(hostSocket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
    {
        closesocket(hostSocket);
        return false;
    }

    std::atomic<bool> replied{false};
    std::thread responder([&]()
    {
        sockaddr_in source{};
        int sourceLength = sizeof(source);
        trial_caster::proto::ConnectionRequest request{};
        const int received = recvfrom(
            hostSocket,
            reinterpret_cast<char*>(&request),
            sizeof(request),
            0,
            reinterpret_cast<sockaddr*>(&source),
            &sourceLength);
        if (received == sizeof(request)
            && request.controlWord == trial_caster::proto::kActionConnect)
        {
            trial_caster::proto::AlreadyInPlayPacket response{};
            const int sent = sendto(
                hostSocket,
                reinterpret_cast<const char*>(&response),
                sizeof(response),
                0,
                reinterpret_cast<const sockaddr*>(&source),
                sourceLength);
            replied.store(sent == sizeof(response));
        }
    });

    trial_caster::CasterConfig config{};
    config.port = kBusyTestPort;
    config.maxTimeoutMs = 2000;
    trial_caster::NetSession client(config);
    const trial_caster::NetResult result = client.ConnectHandshake(L"127.0.0.1", nullptr);
    responder.join();
    client.Close();
    closesocket(hostSocket);
    return replied.load() && result == trial_caster::NetResult::AlreadyInPlay;
}

}

int wmain(int argc, wchar_t** argv)
{
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0)
    {
        return 1;
    }

    const auto require = [](const bool passed, const char* const name) {
        if (!passed)
        {
            std::fprintf(stderr, "launcher session test failed: %s\n", name);
        }
        return passed;
    };
    if (!require(argc >= 4, "required artifact arguments")
        || !require(VerifyDestinationParsing(), "destination parsing")
        || !require(VerifyOutboundRecentSemantics(), "recent destinations")
        || !require(VerifyPortPersistenceAndValidation(), "port persistence")
        || !require(VerifyConflictingControlSaveRejected(), "conflicting control save rejection")
        || !require(VerifyEnvironmentAndWineOverrides(), "child environment/Wine overrides")
        || !require(VerifyWritableStateRoot(), "writable state root")
        || !require(VerifyDiagnosticLoggingOptIn(), "diagnostic logging opt-in")
        || !require(VerifySessionHandoffPersistence(), "session handoff persistence")
        || !require(
            VerifyOneSidedPracticeOptOut(true, false, kClientOptOutTestPort),
            "client practice-key opt-out handshake and handoff")
        || !require(
            VerifyOneSidedPracticeOptOut(false, true, kHostOptOutTestPort),
            "host practice-key opt-out handshake and handoff")
        || !require(VerifyCompatibilityInspection(argv[1]), "netplay compatibility export")
        || !require(argv[2][0] == 0 || VerifyEfzLayoutContract(argv[2]), "EFZ executable layout")
        || !require(VerifyDirectDrawInspection(argv[3], argv[1]), "DirectDraw wrapper exports"))
    {
        WSACleanup();
        return 2;
    }

    if (!VerifyAlreadyStartedReply())
    {
        WSACleanup();
        return 3;
    }

    trial_caster::CasterConfig hostConfig{};
    hostConfig.port = kTestPort;
    hostConfig.packetResendCount = 1;
    hostConfig.maxTimeoutMs = 2000;
    hostConfig.playerName = L"TestHost";
    hostConfig.allowPracticeKeys = true;
    hostConfig.targetFps = 64u;
    trial_caster::CasterConfig clientConfig = hostConfig;
    clientConfig.port = 37993u;
    clientConfig.playerName = L"TestClient";
    clientConfig.allowPracticeKeys = true;
    clientConfig.targetFps = 60u;

    trial_caster::NetSession host(hostConfig);
    trial_caster::NetSession client(clientConfig);
    std::atomic<trial_caster::NetResult> hostResult{trial_caster::NetResult::Denied};
    trial_caster::NetSessionInfo hostInfo{};
    std::thread hostThread([&]() { hostResult.store(host.HostHandshake(&hostInfo)); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!VerifyStaleVersionRejected())
    {
        host.Close();
        hostThread.join();
        WSACleanup();
        return 4;
    }

    trial_caster::NetSession idleSpectatorProbe(hostConfig);
    const trial_caster::NetResult idleProbeResult =
        idleSpectatorProbe.ProbeSpectatorHost(L"127.0.0.1");
    idleSpectatorProbe.Close();
    if (idleProbeResult != trial_caster::NetResult::HostWaiting)
    {
        host.Close();
        hostThread.join();
        WSACleanup();
        return 5;
    }

    trial_caster::NetSessionInfo clientInfo{};
    const trial_caster::NetResult clientResult =
        client.ConnectHandshake(L"localhost", &clientInfo, kTestPort);
    hostThread.join();

    std::atomic<trial_caster::NetResult> startingQueryServiceResult{trial_caster::NetResult::Denied};
    std::thread startingQueryHostThread([&]()
    {
        startingQueryServiceResult.store(host.ServiceOccupiedHost(2000u));
    });
    trial_caster::NetSession startingSpectatorProbe(hostConfig);
    const trial_caster::NetResult startingProbeResult =
        startingSpectatorProbe.ProbeSpectatorHost(L"127.0.0.1");
    startingQueryHostThread.join();
    startingSpectatorProbe.Close();

    std::atomic<trial_caster::NetResult> occupiedServiceResult{trial_caster::NetResult::Denied};
    std::thread occupiedHostThread([&]()
    {
        occupiedServiceResult.store(host.ServiceOccupiedHost(2000u));
    });
    trial_caster::CasterConfig lateConfig = hostConfig;
    lateConfig.playerName = L"LatePlayer";
    trial_caster::NetSession lateClient(lateConfig);
    const trial_caster::NetResult lateResult = lateClient.ConnectHandshake(L"127.0.0.1", nullptr);
    occupiedHostThread.join();

    std::uint8_t committedDelay = 0u;
    std::uint8_t committedTargetFps = 0u;
    std::uint32_t committedLaunchLeadMs = 0u;
    std::atomic<trial_caster::NetResult> launchGoResult{trial_caster::NetResult::Denied};
    std::thread launchGoWaiter([&]()
    {
        launchGoResult.store(client.WaitForLaunchGo(
            &committedDelay,
            &committedTargetFps,
            &committedLaunchLeadMs,
            2000u));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool invalidLaunchGoRejected = !host.SendLaunchGo(
        hostInfo.inputDelay,
        63u,
        321u);
    const bool launchGoSent = host.SendLaunchGo(
        hostInfo.inputDelay,
        hostInfo.targetFps,
        trial_caster::proto::kLaunchSyncLeadMs);
    launchGoWaiter.join();

    host.Close();
    client.Close();
    lateClient.Close();
    WSACleanup();

    if (hostResult.load() != trial_caster::NetResult::Ok
        || clientResult != trial_caster::NetResult::Ok)
    {
        return 6;
    }
    if (startingQueryServiceResult.load() != trial_caster::NetResult::Ok
        || startingProbeResult != trial_caster::NetResult::HostStarting)
    {
        return 7;
    }
    if (occupiedServiceResult.load() != trial_caster::NetResult::Ok
        || lateResult != trial_caster::NetResult::HostStarting)
    {
        return 8;
    }
    if (hostInfo.connectionMethod != trial_caster::ConnectionMethod::Direct
        || clientInfo.connectionMethod != trial_caster::ConnectionMethod::Direct)
    {
        return 9;
    }
    if (clientInfo.peerPort != kTestPort || hostInfo.peerPort != 0u)
    {
        return 10;
    }
    if (!hostInfo.allowPracticeKeys || !clientInfo.allowPracticeKeys)
    {
        return 11;
    }
    if (hostInfo.targetFps != 64u || clientInfo.targetFps != 64u)
    {
        return 12;
    }
    if (!invalidLaunchGoRejected
        || !launchGoSent
        || launchGoResult.load() != trial_caster::NetResult::Ok
        || committedDelay != hostInfo.inputDelay
        || committedTargetFps != 64u
        || committedLaunchLeadMs != trial_caster::proto::kLaunchSyncLeadMs)
    {
        return 13;
    }
    return 0;
}
