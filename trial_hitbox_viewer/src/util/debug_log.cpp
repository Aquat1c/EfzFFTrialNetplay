#include "debug_log.h"

#include "state_paths.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <mutex>
#include <share.h>
#include <string>

namespace thv::log
{
namespace
{

FILE* OpenSharedAppend(const wchar_t* path)
{
    return _wfsopen(path, L"a", _SH_DENYWR);
}

std::string PathUtf8(const std::filesystem::path& path)
{
    const std::wstring wide = path.wstring();
    if (wide.empty())
    {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.c_str(), -1, result.data(), required, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(required - 1));
    return result;
}

bool IsEnabledText(const wchar_t* const text)
{
    return text != nullptr
        && (std::wcscmp(text, L"1") == 0
            || _wcsicmp(text, L"true") == 0
            || _wcsicmp(text, L"yes") == 0
            || _wcsicmp(text, L"on") == 0);
}

bool EnvironmentFlagEnabled(const wchar_t* const name)
{
    wchar_t value[16] = {};
    const DWORD length = GetEnvironmentVariableW(
        name, value, static_cast<DWORD>(std::size(value)));
    return length > 0u && length < std::size(value) && IsEnabledText(value);
}

constexpr std::size_t kMaxUiLogLines = 4000;
constexpr std::size_t kMaxCrashRingEvents = 256;
constexpr int kClassFileSlots = 14;

struct CrashRingEvent
{
    char time[16] = {};
    char category[16] = {};
    char message[224] = {};
};

std::mutex g_mutex;
FILE* g_logFile = nullptr;
FILE* g_classFiles[kClassFileSlots]{};
char g_classFileKeys[kClassFileSlots][16]{};
std::string g_sessionDir;
std::string g_logFilePath;
std::filesystem::path g_sessionDirPath;
std::filesystem::path g_logFilePathPath;
bool g_fileReady = false;
bool g_initialized = false;
bool g_fileLoggingRequested = false;
std::deque<std::string> g_uiLogLines;
char g_crashSessionLogPath[4096] = {};
CrashRingEvent g_crashRing[kMaxCrashRingEvents]{};
std::size_t g_crashRingWrite = 0;
std::size_t g_crashRingCount = 0;

void FormatTimeStamp(SYSTEMTIME* st, char* out, std::size_t outSize)
{
    if (out == nullptr || outSize == 0)
    {
        return;
    }

    SYSTEMTIME local = {};
    if (st != nullptr)
    {
        local = *st;
    }
    else
    {
        GetLocalTime(&local);
    }

    std::snprintf(
        out,
        outSize,
        "%02u:%02u:%02u.%03u",
        local.wHour,
        local.wMinute,
        local.wSecond,
        local.wMilliseconds);
}

void AppendUiLineUnlocked(const char* line)
{
    g_uiLogLines.emplace_back(line);
    while (g_uiLogLines.size() > kMaxUiLogLines)
    {
        g_uiLogLines.pop_front();
    }
}

int FindClassFileSlot(const char* eventClass)
{
    if (eventClass == nullptr || eventClass[0] == '\0')
    {
        return -1;
    }

    for (int i = 0; i < kClassFileSlots; ++i)
    {
        if (g_classFileKeys[i][0] != '\0' && std::strcmp(g_classFileKeys[i], eventClass) == 0)
        {
            return i;
        }
    }

    for (int i = 0; i < kClassFileSlots; ++i)
    {
        if (g_classFileKeys[i][0] == '\0')
        {
            std::snprintf(g_classFileKeys[i], sizeof(g_classFileKeys[i]), "%s", eventClass);
            return i;
        }
    }

    return -1;
}

bool OpenClassFileUnlocked(int slot, const char* eventClass)
{
    if (slot < 0 || slot >= kClassFileSlots || g_classFiles[slot] != nullptr || g_sessionDir.empty())
    {
        return g_classFiles[slot] != nullptr;
    }

    const auto path = g_sessionDirPath / (std::string(eventClass) + ".log");
    g_classFiles[slot] = OpenSharedAppend(path.c_str());
    if (g_classFiles[slot] == nullptr)
    {
        return false;
    }

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    std::fprintf(
        g_classFiles[slot],
        "\n--- %s log %04u-%02u-%02u %02u:%02u:%02u PID=%lu ---\n",
        eventClass,
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond,
        GetCurrentProcessId());
    std::fflush(g_classFiles[slot]);
    return true;
}

void MirrorClassLineUnlocked(const char* eventClass, const char* line)
{
    if (eventClass == nullptr || line == nullptr || !g_fileReady || g_sessionDir.empty())
    {
        return;
    }

    const int slot = FindClassFileSlot(eventClass);
    if (slot < 0)
    {
        return;
    }

    if (!OpenClassFileUnlocked(slot, eventClass))
    {
        return;
    }

    std::fputs(line, g_classFiles[slot]);
    std::fputc('\n', g_classFiles[slot]);
    std::fflush(g_classFiles[slot]);
}

bool ShouldAutoNote(const char* level)
{
    return level != nullptr
        && (std::strcmp(level, "WARN") == 0 || std::strcmp(level, "ERROR") == 0 || std::strcmp(level, "CRIT") == 0);
}

void NoteEventUnlocked(const char* category, const char* message, const SYSTEMTIME* timeOverride = nullptr)
{
    if (category == nullptr || message == nullptr)
    {
        return;
    }

    SYSTEMTIME st = {};
    if (timeOverride != nullptr)
    {
        st = *timeOverride;
    }
    else
    {
        GetLocalTime(&st);
    }

    CrashRingEvent& slot = g_crashRing[g_crashRingWrite % kMaxCrashRingEvents];
    FormatTimeStamp(&st, slot.time, sizeof(slot.time));
    std::snprintf(slot.category, sizeof(slot.category), "%s", category);
    std::snprintf(slot.message, sizeof(slot.message), "%s", message);
    g_crashRingWrite = (g_crashRingWrite + 1u) % kMaxCrashRingEvents;
    if (g_crashRingCount < kMaxCrashRingEvents)
    {
        ++g_crashRingCount;
    }
}

void WriteLineUnlocked(
    const char* level,
    const char* category,
    const char* message,
    SYSTEMTIME* outTime = nullptr,
    const bool autoNote = true)
{
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    if (outTime != nullptr)
    {
        *outTime = st;
    }

    char line[2048] = {};
    std::snprintf(
        line,
        sizeof(line),
        "[%02u:%02u:%02u.%03u] %-5s %-10s %s",
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds,
        level,
        category,
        message);

    AppendUiLineUnlocked(line);

    if (g_logFile != nullptr)
    {
        std::fputs(line, g_logFile);
        std::fputc('\n', g_logFile);
        std::fflush(g_logFile);
    }

    if (autoNote && ShouldAutoNote(level))
    {
        NoteEventUnlocked(category, message, &st);
        MirrorClassLineUnlocked(category, line);
    }

    OutputDebugStringA(line);
    OutputDebugStringA("\n");
}

void WriteLine(const char* level, const char* category, const char* message)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    WriteLineUnlocked(level, category, message);
}

void LogVa(const char* level, const char* category, const char* fmt, va_list args)
{
    char message[1536] = {};
    vsnprintf(message, sizeof(message), fmt, args);
    WriteLine(level, category, message);
}

std::filesystem::path BuildSessionDirectory()
{
    SYSTEMTIME st = {};
    GetLocalTime(&st);

    wchar_t folderName[64] = {};
    std::swprintf(
        folderName,
        std::size(folderName),
        L"PID%lu_%04u-%02u-%02u",
        static_cast<unsigned long>(GetCurrentProcessId()),
        st.wYear,
        st.wMonth,
        st.wDay);

    const auto sessionDir = paths::MutableStateRoot() / L"logs" / folderName;
    std::error_code ec;
    std::filesystem::create_directories(sessionDir, ec);
    return sessionDir;
}

bool OpenLogFileUnlocked()
{
    if (g_logFile != nullptr)
    {
        return true;
    }

    const auto sessionDir = BuildSessionDirectory();
    g_sessionDirPath = sessionDir;
    g_logFilePathPath = sessionDir / L"session.log";
    g_sessionDir = PathUtf8(g_sessionDirPath);
    g_logFilePath = PathUtf8(g_logFilePathPath);
    std::snprintf(
        g_crashSessionLogPath,
        sizeof(g_crashSessionLogPath),
        "%s",
        g_logFilePath.c_str());

    g_logFile = OpenSharedAppend(g_logFilePathPath.c_str());
    if (g_logFile == nullptr)
    {
        char fallbackName[64] = {};
        std::snprintf(
            fallbackName,
            sizeof(fallbackName),
            "session_pid%lu.log",
            static_cast<unsigned long>(GetCurrentProcessId()));
        g_logFilePathPath = paths::MutableStateRoot() / fallbackName;
        g_logFilePath = PathUtf8(g_logFilePathPath);
        g_logFile = OpenSharedAppend(g_logFilePathPath.c_str());
        if (g_logFile == nullptr)
        {
            return false;
        }
    }

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    std::fprintf(
        g_logFile,
        "\n--- netplay.dll session %04u-%02u-%02u %02u:%02u:%02u PID=%lu ---\n",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond,
        GetCurrentProcessId());
    std::fprintf(
        g_logFile,
        "classified logs: init.log phase.log sync.log net.log input.log rollback.log stall.log exit.log crash.log desync.log fault.log memwrite.log state_snapshot.log\n");
    std::fflush(g_logFile);
    g_fileReady = true;
    return true;
}

void BackfillBufferedLinesUnlocked()
{
    if (g_logFile == nullptr || g_uiLogLines.empty())
    {
        return;
    }

    std::fputs("--- buffered pre-file-log context ---\n", g_logFile);
    for (const std::string& line : g_uiLogLines)
    {
        std::fputs(line.c_str(), g_logFile);
        std::fputc('\n', g_logFile);
    }
    std::fputs("--- end buffered pre-file-log context ---\n", g_logFile);
    std::fflush(g_logFile);
}

void CloseClassFilesUnlocked()
{
    for (int i = 0; i < kClassFileSlots; ++i)
    {
        if (g_classFiles[i] != nullptr)
        {
            std::fclose(g_classFiles[i]);
            g_classFiles[i] = nullptr;
        }
        g_classFileKeys[i][0] = '\0';
    }
}

}

bool FileLoggingRequested(const std::filesystem::path& sessionIni)
{
    if (EnvironmentFlagEnabled(L"THV_FILE_LOG")
        || EnvironmentFlagEnabled(L"THV_DEBUG_LOG")
        || EnvironmentFlagEnabled(L"THV_MEMLOG"))
    {
        return true;
    }
    if (sessionIni.empty())
    {
        return false;
    }

    wchar_t value[16] = {};
    GetPrivateProfileStringW(
        L"session",
        L"diagnostic_logging",
        L"0",
        value,
        static_cast<DWORD>(std::size(value)),
        sessionIni.c_str());
    return IsEnabledText(value);
}

void ConfigureFileLogging(const bool enabled)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        g_fileLoggingRequested = enabled;
    }
}

bool EnableFileLogging()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_fileLoggingRequested = true;
        if (g_fileReady)
        {
            return true;
        }
        if (!OpenLogFileUnlocked())
        {
            return false;
        }

        BackfillBufferedLinesUnlocked();
        WriteLineUnlocked("STEP", EventClass::Init, "file logging enabled");
        char banner[512] = {};
        std::snprintf(banner, sizeof(banner), "session dir: %s", g_sessionDir.c_str());
        WriteLineUnlocked("INFO", EventClass::Init, banner);
        std::snprintf(banner, sizeof(banner), "master log: %s", g_logFilePath.c_str());
        WriteLineUnlocked("INFO", EventClass::Init, banner);
        std::snprintf(
            banner,
            sizeof(banner),
            "writable state root: %s (source=%s)",
            PathUtf8(paths::MutableStateRoot()).c_str(),
            paths::MutableStateRootSource());
        WriteLineUnlocked("INFO", EventClass::Init, banner);
    }

    WriteSessionManifest();
    return true;
}

bool IsFileLoggingEnabled()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_fileReady;
}

bool Initialize()
{
    bool enableFile = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_initialized)
        {
            return true;
        }
        g_initialized = true;
        enableFile = g_fileLoggingRequested;
        if (!enableFile)
        {
            WriteLineUnlocked(
                "INFO",
                EventClass::Init,
                "file logging disabled (enable Diagnostics in the launcher to persist logs)");
        }
    }

    if (!enableFile || EnableFileLogging())
    {
        return true;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_initialized = false;
    return false;
}

void WriteSessionManifest()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_sessionDir.empty())
    {
        return;
    }

    const std::filesystem::path path = g_sessionDirPath / L"session_manifest.txt";
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"w") != 0 || file == nullptr)
    {
        return;
    }

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    std::fprintf(file, "Trial EFZ netplay.dll log session\n");
    std::fprintf(file, "started=%04u-%02u-%02u %02u:%02u:%02u pid=%lu\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
    std::fprintf(file, "master=%s\n", g_logFilePath.c_str());
    std::fprintf(file, "crashes=<stateRoot>/logs/crashes/\n");
    std::fprintf(file, "\nclassified logs (high-signal events + ring buffer source):\n");
    std::fprintf(file, "  init.log       startup, hooks installed\n");
    std::fprintf(file, "  phase.log      screen/nav/bootstrap/rollback phase transitions\n");
    std::fprintf(file, "  sync.log       SyncData apply/reject, lockstep history, handoff\n");
    std::fprintf(file, "  net.log        UDP, seed, launch-go, heartbeats\n");
    std::fprintf(file, "  input.log      DI overlay, char-select wire filter\n");
    std::fprintf(file, "  rollback.log   rewind, reload, savestate, memorial\n");
    std::fprintf(file, "  stall.log      lockstep wait_remote stalls\n");
    std::fprintf(file, "  exit.log       session abort/shutdown\n");
    std::fprintf(file, "  crash.log      first-chance + fatal exceptions\n");
    std::fprintf(file, "  desync.log     FPU/RNG mismatch hints\n");
    std::fprintf(file, "  fault.log      non-fatal errors (UDP, hooks, blocked launch-go)\n");
    std::fprintf(file, "  memwrite.log   game RAM writes: before/after scalars, u32 diffs, watchpoints\n");
    std::fprintf(file, "  state_snapshot.log  mod state checkpoints (also in crash .txt)\n");
    std::fprintf(file, "\nparse order: phase -> sync -> memwrite -> fault -> state_snapshot -> session.log\n");
    std::fprintf(file, "heap corruption: memwrite.log watchpoints + diff lines show va and before/after values\n");
    std::fprintf(file, "THV_MEMLOG_VERBOSE=1 enables full u32 diff scan on large battle restores (>4KB)\n");
    std::fprintf(file, "memwrite region lines include live_fnv vs payload_fnv; DIFFER = bytes will change on write\n");
    std::fprintf(file, "large regions always log first 8 u32 diffs (diff_sparse) even without verbose mode\n");
    std::fprintf(file, "verify_ok/VERIFY_FAIL lines confirm memcpy landed; verify_watch = field still wrong after write\n");
    std::fprintf(file, "rollback_save/seed slot hashes in memwrite.log — compare to slot_payload on reload\n");
    std::fclose(file);
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logFile != nullptr)
    {
        WriteLineUnlocked("STEP", EventClass::Init, "file logging shutdown");
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
    CloseClassFilesUnlocked();
    g_initialized = false;
    g_fileReady = false;
    g_fileLoggingRequested = false;
    g_logFilePath.clear();
    g_sessionDir.clear();
    g_logFilePathPath.clear();
    g_sessionDirPath.clear();
    g_crashSessionLogPath[0] = '\0';
    g_uiLogLines.clear();
    g_crashRingCount = 0;
    g_crashRingWrite = 0;
}

std::string SessionDirectory()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_sessionDir;
}

std::string SessionLogPath()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_logFilePath;
}

std::filesystem::path SessionDirectoryPath()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_sessionDirPath;
}

std::filesystem::path SessionLogPathPath()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_logFilePathPath;
}

void CopyLogText(std::string* out)
{
    if (out == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    out->clear();
    for (const std::string& line : g_uiLogLines)
    {
        out->append(line);
        out->push_back('\n');
    }
}

void FlushFile()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logFile != nullptr)
    {
        std::fflush(g_logFile);
    }
    for (FILE* classFile : g_classFiles)
    {
        if (classFile != nullptr)
        {
            std::fflush(classFile);
        }
    }
}

const char* SessionLogPathForCrash()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_crashSessionLogPath;
}

void NoteEvent(const char* category, const char* message)
{
    if (category == nullptr || message == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    NoteEventUnlocked(category, message);
}

void VEvent(const char* eventClass, const char* subsystem, const char* fmt, va_list args)
{
    // Enable first so this event and the buffered lead-up persist together.
    if (eventClass != nullptr && std::strcmp(eventClass, EventClass::Desync) == 0)
    {
        (void)EnableFileLogging();
    }

    char body[1400] = {};
    vsnprintf(body, sizeof(body), fmt, args);

    char message[1536] = {};
    if (subsystem != nullptr && subsystem[0] != '\0')
    {
        std::snprintf(message, sizeof(message), "%s: %s", subsystem, body);
    }
    else
    {
        std::snprintf(message, sizeof(message), "%s", body);
    }

    SYSTEMTIME st = {};
    std::lock_guard<std::mutex> lock(g_mutex);
    WriteLineUnlocked("STEP", eventClass, message, &st, false);
    NoteEventUnlocked(eventClass, message, &st);
    char line[2048] = {};
    std::snprintf(
        line,
        sizeof(line),
        "[%02u:%02u:%02u.%03u] %-5s %-10s %s",
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds,
        "STEP",
        eventClass,
        message);
    MirrorClassLineUnlocked(eventClass, line);
}

void Event(const char* eventClass, const char* subsystem, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    VEvent(eventClass, subsystem, fmt, args);
    va_end(args);
}

void WriteCrashContext(FILE* file)
{
    if (file == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    std::fprintf(file, "\n--- recent_events (oldest first, max %zu) ---\n", kMaxCrashRingEvents);
    if (g_crashRingCount == 0u)
    {
        std::fprintf(file, "(none)\n");
    }
    else
    {
        const std::size_t start = g_crashRingCount < kMaxCrashRingEvents ? 0u : g_crashRingWrite;
        for (std::size_t i = 0; i < g_crashRingCount; ++i)
        {
            const std::size_t index = (start + i) % kMaxCrashRingEvents;
            const CrashRingEvent& ev = g_crashRing[index];
            if (ev.category[0] == '\0')
            {
                continue;
            }
            std::fprintf(file, "[%s] [%s] %s\n", ev.time, ev.category, ev.message);
        }
    }
    std::fprintf(file, "--- end recent_events ---\n");

    std::fprintf(file, "\n--- recent_memwrite_events (oldest first) ---\n");
    std::size_t memwriteCount = 0u;
    if (g_crashRingCount > 0u)
    {
        const std::size_t start = g_crashRingCount < kMaxCrashRingEvents ? 0u : g_crashRingWrite;
        for (std::size_t i = 0; i < g_crashRingCount; ++i)
        {
            const std::size_t index = (start + i) % kMaxCrashRingEvents;
            const CrashRingEvent& ev = g_crashRing[index];
            if (ev.category[0] == '\0' || std::strcmp(ev.category, EventClass::MemWrite) != 0)
            {
                continue;
            }
            std::fprintf(file, "[%s] %s\n", ev.time, ev.message);
            ++memwriteCount;
        }
    }
    if (memwriteCount == 0u)
    {
        std::fprintf(file, "(none)\n");
    }
    std::fprintf(file, "--- end recent_memwrite_events ---\n");
}

void WriteSessionTail(FILE* file, const std::size_t maxLines)
{
    if (file == nullptr || maxLines == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    std::fprintf(file, "\n--- session_log_tail (last %zu lines) ---\n", maxLines);
    if (g_uiLogLines.empty())
    {
        std::fprintf(file, "(empty)\n");
        return;
    }

    const std::size_t count = std::min(maxLines, g_uiLogLines.size());
    const std::size_t begin = g_uiLogLines.size() - count;
    for (std::size_t i = begin; i < g_uiLogLines.size(); ++i)
    {
        std::fprintf(file, "%s\n", g_uiLogLines[i].c_str());
    }
    std::fprintf(file, "--- end session_log_tail ---\n");
}

void WriteClassifiedTail(FILE* file, const char* eventClass, const std::size_t maxLines)
{
    if (file == nullptr || eventClass == nullptr || eventClass[0] == '\0' || maxLines == 0u)
    {
        return;
    }

    std::filesystem::path sessionDir;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        sessionDir = g_sessionDirPath;
        for (FILE* classFile : g_classFiles)
        {
            if (classFile != nullptr)
            {
                std::fflush(classFile);
            }
        }
    }

    if (sessionDir.empty())
    {
        return;
    }

    const auto path = sessionDir / (std::string(eventClass) + ".log");
    FILE* source = nullptr;
    if (_wfopen_s(&source, path.c_str(), L"rb") != 0 || source == nullptr)
    {
        std::fprintf(file, "\n--- %s_tail ---\n(missing)\n--- end %s_tail ---\n", eventClass, eventClass);
        return;
    }

    std::deque<std::string> tail;
    char buffer[2048] = {};
    while (std::fgets(buffer, sizeof(buffer), source) != nullptr)
    {
        tail.emplace_back(buffer);
        while (tail.size() > maxLines)
        {
            tail.pop_front();
        }
    }
    std::fclose(source);

    const std::string displayPath = PathUtf8(path);
    std::fprintf(file, "\n--- %s_tail (last %zu lines, path=%s) ---\n", eventClass, maxLines, displayPath.c_str());
    if (tail.empty())
    {
        std::fprintf(file, "(empty)\n");
    }
    else
    {
        for (const std::string& line : tail)
        {
            std::fputs(line.c_str(), file);
            if (line.empty() || line.back() != '\n')
            {
                std::fputc('\n', file);
            }
        }
    }
    std::fprintf(file, "--- end %s_tail ---\n", eventClass);
}

void VStep(const char* category, const char* fmt, va_list args)
{
    LogVa("STEP", category, fmt, args);
}

void VDebug(const char* category, const char* fmt, va_list args)
{
#if !defined(_DEBUG)
    static const bool enabled = GetEnvironmentVariableA("THV_DEBUG_LOG", nullptr, 0) > 0;
    if (!enabled)
    {
        return;
    }
#endif
    LogVa("DEBUG", category, fmt, args);
}

void VInfo(const char* category, const char* fmt, va_list args)
{
    LogVa("INFO", category, fmt, args);
}

void VWarn(const char* category, const char* fmt, va_list args)
{
    LogVa("WARN", category, fmt, args);
}

void VError(const char* category, const char* fmt, va_list args)
{
    LogVa("ERROR", category, fmt, args);
}

void Step(const char* category, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    VStep(category, fmt, args);
    va_end(args);
}

void Debug(const char* category, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    VDebug(category, fmt, args);
    va_end(args);
}

void Info(const char* category, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    VInfo(category, fmt, args);
    va_end(args);
}

void Warn(const char* category, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    VWarn(category, fmt, args);
    va_end(args);
}

void Error(const char* category, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    VError(category, fmt, args);
    va_end(args);
}

void Critical(const char* category, const char* fmt, ...)
{
    if (category != nullptr && std::strcmp(category, EventClass::Desync) == 0)
    {
        (void)EnableFileLogging();
    }

    va_list args;
    va_start(args, fmt);
    LogVa("CRIT", category, fmt, args);
    va_end(args);
    FlushFile();
}

}
