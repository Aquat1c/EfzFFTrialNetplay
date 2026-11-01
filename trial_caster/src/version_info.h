#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace trial_caster
{

struct BinaryVersion
{
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    std::uint16_t patch = 0;
    std::uint16_t build = 0;
};

struct VersionQuery
{
    bool available = false;
    BinaryVersion value{};
    std::wstring display = L"Unavailable";
    std::wstring error;
};

struct InstalledComponentVersions
{
    VersionQuery launcher;
    VersionQuery netplayDll;
};

std::wstring FormatBinaryVersion(const BinaryVersion& version);
VersionQuery QueryBinaryVersion(const std::filesystem::path& path);
std::filesystem::path CurrentExecutablePath();
InstalledComponentVersions QueryInstalledComponentVersions();

}
