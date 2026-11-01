#pragma once

#include <filesystem>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace trial_caster
{

struct LaunchResult
{
    bool ok = false;
    std::wstring message;
    unsigned long processId = 0;
    std::wstring sessionId;
    std::filesystem::path sessionIniPath;
};

class GameLauncher
{
public:
    LaunchResult Launch(
        const std::filesystem::path& efzExe,
        const std::vector<std::filesystem::path>& dlls,
        bool highPriority,
        const std::filesystem::path& sessionIniPath = {},
        const std::atomic<bool>* cancel = nullptr,
        const std::function<void()>& beforeResume = {}) const;
};

}
