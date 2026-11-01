#include "session_log.h"

#include "util.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace trial_caster
{
namespace
{

std::mutex g_mutex;
std::filesystem::path g_sessionDir;
std::filesystem::path g_logDir;
std::filesystem::path g_logFile;
std::string g_logFileNarrow;
bool g_initialized = false;
bool g_ready = false;

std::string PathUtf8NoThrow(const std::filesystem::path& path) noexcept
{
    try
    {
        const std::wstring wide = path.wstring();
        if (wide.empty())
        {
            return {};
        }
        const int required = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            wide.c_str(),
            -1,
            nullptr,
            0,
            nullptr,
            nullptr);
        if (required <= 1)
        {
            return {};
        }
        std::string utf8(static_cast<std::size_t>(required), '\0');
        const int written = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            wide.c_str(),
            -1,
            utf8.data(),
            required,
            nullptr,
            nullptr);
        if (written != required)
        {
            return {};
        }
        utf8.resize(static_cast<std::size_t>(written - 1));
        return utf8;
    }
    catch (...)
    {
        return {};
    }
}

void WriteRawUnlocked(const wchar_t* level, const wchar_t* category, const wchar_t* message)
{
    SYSTEMTIME st = {};
    GetLocalTime(&st);

    const std::wstring line = util::Format(
        L"[%02u:%02u:%02u.%03u] %-5s %-10s %s",
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds,
        level,
        category,
        message);

    if (!g_ready)
    {
        return;
    }

    std::wofstream out(g_logFile, std::ios::app);
    if (out)
    {
        out << line << L'\n';
    }

    OutputDebugStringW(line.c_str());
    OutputDebugStringW(L"\n");
}

void LogVa(const wchar_t* level, const wchar_t* category, const wchar_t* fmt, va_list args)
{
    wchar_t message[1536] = {};
    vswprintf_s(message, fmt, args);
    std::lock_guard lock(g_mutex);
    WriteRawUnlocked(level, category, message);
}

}

bool SessionLog::Initialize(
    const std::filesystem::path& stateDirectory,
    const bool fileLoggingEnabled)
{
    std::lock_guard lock(g_mutex);
    if (g_initialized)
    {
        return true;
    }

    SYSTEMTIME st = {};
    GetLocalTime(&st);

    wchar_t folderName[64] = {};
    std::swprintf(
        folderName,
        std::size(folderName),
        L"PID%lu_%04u-%02u-%02u_%02u-%02u-%02u",
        static_cast<unsigned long>(GetCurrentProcessId()),
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond);

    g_sessionDir = stateDirectory / L"sessions" / folderName;
    g_logDir = stateDirectory / L"logs" / folderName;
    g_logFile = g_logDir / L"session.log";
    // path::string() uses the ANSI locale and can throw on a Unicode Wine-prefix path.
    g_logFileNarrow = PathUtf8NoThrow(g_logFile);

    std::error_code ec;
    std::filesystem::create_directories(g_sessionDir, ec);
    if (ec)
    {
        return false;
    }

    g_initialized = true;
    if (!fileLoggingEnabled)
    {
        g_logDir.clear();
        g_logFile.clear();
        g_logFileNarrow.clear();
        return true;
    }

    std::filesystem::create_directories(g_logDir, ec);
    if (ec)
    {
        g_initialized = false;
        g_sessionDir.clear();
        g_logDir.clear();
        g_logFile.clear();
        g_logFileNarrow.clear();
        return false;
    }

    std::wofstream banner(g_logFile, std::ios::app);
    if (!banner)
    {
        g_initialized = false;
        g_sessionDir.clear();
        g_logDir.clear();
        g_logFile.clear();
        g_logFileNarrow.clear();
        return false;
    }

    banner << L"\n--- TrialLauncher session "
           << st.wYear << L"-" << st.wMonth << L"-" << st.wDay << L" "
           << st.wHour << L":" << st.wMinute << L":" << st.wSecond << L" PID="
           << GetCurrentProcessId() << L" ---\n";
    g_ready = true;

    WriteRawUnlocked(L"STEP", L"init", L"File logging ready");
    WriteRawUnlocked(L"INFO", L"init", util::Format(L"Session dir: %s", g_sessionDir.wstring().c_str()).c_str());
    WriteRawUnlocked(L"INFO", L"init", util::Format(L"Log file: %s", g_logFile.wstring().c_str()).c_str());
    WriteRawUnlocked(
        L"INFO",
        L"init",
        L"Per-launch configs: <session_dir>/launch_N/session.ini (see launches.tsv)");
    return true;
}

void SessionLog::Shutdown()
{
    std::lock_guard lock(g_mutex);
    if (!g_initialized)
    {
        return;
    }

    if (g_ready)
    {
        WriteRawUnlocked(L"STEP", L"init", L"File logging shutdown");
    }
    g_ready = false;
    g_initialized = false;
    g_sessionDir.clear();
    g_logDir.clear();
    g_logFile.clear();
    // Immutable for process lifetime: a fatal handler must not contend on the mutex.
}

bool SessionLog::IsFileLoggingEnabled()
{
    std::lock_guard lock(g_mutex);
    return g_ready;
}

void SessionLog::Flush()
{
    std::lock_guard lock(g_mutex);
    if (!g_ready)
    {
        return;
    }

    std::wofstream out(g_logFile, std::ios::app);
    if (out)
    {
        out.flush();
    }
}

const char* SessionLog::LogFilePathNarrow()
{
    return g_logFileNarrow.c_str();
}

std::filesystem::path SessionLog::SessionDirectory()
{
    std::lock_guard lock(g_mutex);
    return g_sessionDir;
}

std::filesystem::path SessionLog::LogDirectory()
{
    std::lock_guard lock(g_mutex);
    return g_logDir;
}

std::filesystem::path SessionLog::LogFilePath()
{
    std::lock_guard lock(g_mutex);
    return g_logFile;
}

void SessionLog::Step(const wchar_t* category, const wchar_t* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogVa(L"STEP", category, fmt, args);
    va_end(args);
}

void SessionLog::Info(const wchar_t* category, const wchar_t* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogVa(L"INFO", category, fmt, args);
    va_end(args);
}

void SessionLog::Warn(const wchar_t* category, const wchar_t* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogVa(L"WARN", category, fmt, args);
    va_end(args);
}

void SessionLog::Error(const wchar_t* category, const wchar_t* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogVa(L"ERROR", category, fmt, args);
    va_end(args);
}

void SessionLog::WriteLine(const std::wstring& line)
{
    std::lock_guard lock(g_mutex);
    if (!g_ready)
    {
        return;
    }

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    const std::wstring stamped = util::Format(
        L"[%02u:%02u:%02u.%03u] UI       %s",
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds,
        line.c_str());

    std::wofstream out(g_logFile, std::ios::app);
    if (out)
    {
        out << stamped << L'\n';
    }
}

}
