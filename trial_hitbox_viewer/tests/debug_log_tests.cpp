#include "debug_log.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace
{

bool Expect(const bool condition, const char* const message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

}

int main()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / (L"trial_netplay_debug_log_tests_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    if (!Expect(!error, "create isolated state root")
        || !Expect(
            SetEnvironmentVariableW(L"TRIAL_NETPLAY_STATE_DIR", root.c_str()) != FALSE,
            "set isolated state root"))
    {
        return 1;
    }

    SetEnvironmentVariableW(L"THV_FILE_LOG", nullptr);
    SetEnvironmentVariableW(L"THV_DEBUG_LOG", nullptr);
    SetEnvironmentVariableW(L"THV_MEMLOG", nullptr);
    const std::filesystem::path sessionIni = root / L"session.ini";
    WritePrivateProfileStringW(L"session", L"diagnostic_logging", L"0", sessionIni.c_str());

    bool ok = true;
    ok = Expect(!thv::log::FileLoggingRequested(sessionIni), "session option defaults off") && ok;
    thv::log::ConfigureFileLogging(thv::log::FileLoggingRequested(sessionIni));
    ok = Expect(thv::log::Initialize(), "default logger initializes") && ok;
    thv::log::Info("test", "DEFAULT_DISABLED_MARKER");
    ok = Expect(!thv::log::IsFileLoggingEnabled(), "file logging defaults off") && ok;
    ok = Expect(thv::log::SessionLogPathPath().empty(), "default has no session log path") && ok;
    ok = Expect(!std::filesystem::exists(root / L"logs"), "default creates no log directory") && ok;
    thv::log::Shutdown();

    WritePrivateProfileStringW(L"session", L"diagnostic_logging", L"1", sessionIni.c_str());
    ok = Expect(thv::log::FileLoggingRequested(sessionIni), "session option explicitly enables logs") && ok;
    thv::log::ConfigureFileLogging(thv::log::FileLoggingRequested(sessionIni));
    ok = Expect(thv::log::Initialize(), "explicit logger initializes") && ok;
    thv::log::Info("test", "EXPLICIT_ENABLED_MARKER");
    thv::log::FlushFile();
    const std::filesystem::path explicitPath = thv::log::SessionLogPathPath();
    ok = Expect(thv::log::IsFileLoggingEnabled(), "explicit configuration enables file logging") && ok;
    ok = Expect(!explicitPath.empty() && std::filesystem::exists(explicitPath), "explicit session log exists") && ok;
    ok = Expect(ReadAll(explicitPath).find("EXPLICIT_ENABLED_MARKER") != std::string::npos,
        "explicit session log contains event") && ok;
    thv::log::Shutdown();

    std::filesystem::remove_all(root / L"logs", error);
    error.clear();
    WritePrivateProfileStringW(L"session", L"diagnostic_logging", L"0", sessionIni.c_str());
    thv::log::ConfigureFileLogging(thv::log::FileLoggingRequested(sessionIni));
    ok = Expect(thv::log::Initialize(), "lazy logger initializes disabled") && ok;
    thv::log::Info("test", "BUFFERED_PRE_DESYNC_MARKER");
    ok = Expect(!std::filesystem::exists(root / L"logs"), "pre-desync creates no log directory") && ok;
    thv::log::Event(
        thv::log::EventClass::Desync,
        "test",
        "LAZY_DESYNC_EVIDENCE_MARKER");
    thv::log::FlushFile();
    const std::filesystem::path lazyPath = thv::log::SessionLogPathPath();
    const std::string lazyText = ReadAll(lazyPath);
    const std::size_t buffered = lazyText.find("BUFFERED_PRE_DESYNC_MARKER");
    const std::size_t evidence = lazyText.find("LAZY_DESYNC_EVIDENCE_MARKER");
    ok = Expect(thv::log::IsFileLoggingEnabled(), "desync lazily enables file logging") && ok;
    ok = Expect(!lazyPath.empty() && std::filesystem::exists(lazyPath), "desync session log exists") && ok;
    ok = Expect(buffered != std::string::npos, "desync log backfills buffered context") && ok;
    ok = Expect(evidence != std::string::npos, "desync log contains mismatch evidence") && ok;
    ok = Expect(buffered < evidence, "buffered context precedes mismatch evidence") && ok;
    thv::log::Shutdown();

    std::filesystem::remove_all(root, error);
    if (ok)
    {
        std::cout << "debug log policy tests passed\n";
        return 0;
    }
    return 1;
}
