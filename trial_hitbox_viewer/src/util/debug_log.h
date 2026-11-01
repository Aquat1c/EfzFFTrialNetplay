#pragma once

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <string>

namespace thv::log
{

namespace EventClass
{
constexpr const char* Phase = "phase";
constexpr const char* Sync = "sync";
constexpr const char* Net = "net";
constexpr const char* Input = "input";
constexpr const char* Rollback = "rollback";
constexpr const char* Stall = "stall";
constexpr const char* Exit = "exit";
constexpr const char* Crash = "crash";
constexpr const char* Desync = "desync";
constexpr const char* Init = "init";
constexpr const char* Fault = "fault";
constexpr const char* MemWrite = "memwrite";
}

bool Initialize();
void Shutdown();

bool FileLoggingRequested(const std::filesystem::path& sessionIni);
void ConfigureFileLogging(bool enabled);
bool EnableFileLogging();
bool IsFileLoggingEnabled();

// Session directory: <writableStateRoot>/logs/PID<pid>_<YYYY-MM-DD>/
std::string SessionDirectory();
std::string SessionLogPath();
std::filesystem::path SessionDirectoryPath();
std::filesystem::path SessionLogPathPath();

void CopyLogText(std::string* out);

void FlushFile();
void WriteSessionManifest();
const char* SessionLogPathForCrash();

void NoteEvent(const char* category, const char* message);
void WriteCrashContext(FILE* file);

void WriteSessionTail(FILE* file, std::size_t maxLines = 64);

void WriteClassifiedTail(FILE* file, const char* eventClass, std::size_t maxLines = 64);

void Event(const char* eventClass, const char* subsystem, const char* fmt, ...);
void VEvent(const char* eventClass, const char* subsystem, const char* fmt, va_list args);

void Step(const char* category, const char* fmt, ...);
void Critical(const char* category, const char* fmt, ...);
void Debug(const char* category, const char* fmt, ...);
void Info(const char* category, const char* fmt, ...);
void Warn(const char* category, const char* fmt, ...);
void Error(const char* category, const char* fmt, ...);

void VStep(const char* category, const char* fmt, va_list args);
void VDebug(const char* category, const char* fmt, va_list args);
void VInfo(const char* category, const char* fmt, va_list args);
void VWarn(const char* category, const char* fmt, va_list args);
void VError(const char* category, const char* fmt, va_list args);

}
