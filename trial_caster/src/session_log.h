#pragma once

#include <filesystem>
#include <string>

namespace trial_caster
{

class SessionLog
{
public:
    static bool Initialize(const std::filesystem::path& stateDirectory, bool fileLoggingEnabled);
    static void Shutdown();

    static bool IsFileLoggingEnabled();
    static std::filesystem::path SessionDirectory();
    static std::filesystem::path LogDirectory();
    static std::filesystem::path LogFilePath();
    static const char* LogFilePathNarrow();
    static void Flush();

    static void Step(const wchar_t* category, const wchar_t* fmt, ...);
    static void Info(const wchar_t* category, const wchar_t* fmt, ...);
    static void Warn(const wchar_t* category, const wchar_t* fmt, ...);
    static void Error(const wchar_t* category, const wchar_t* fmt, ...);

    static void WriteLine(const std::wstring& line);
};

}
