#pragma once

#include <string>

namespace trial_caster
{

enum class CompatibilityRuntimeKind
{
    Windows,
    Wine,
    Proton,
};

struct CompatibilityRuntimeInfo
{
    CompatibilityRuntimeKind kind = CompatibilityRuntimeKind::Windows;
    std::wstring display = L"Windows";
    std::wstring wineVersion;
    std::wstring wineBuildId;
    std::wstring hostSystem;
    std::wstring prefixPath;
    std::wstring steamAppId;

    bool IsWine() const
    {
        return kind == CompatibilityRuntimeKind::Wine
            || kind == CompatibilityRuntimeKind::Proton;
    }
};

CompatibilityRuntimeInfo DetectCompatibilityRuntime();

std::wstring CompatibilityLaunchAdvice(const CompatibilityRuntimeInfo& runtime);

std::wstring ConfigureProcessDpiAwareness();

}
