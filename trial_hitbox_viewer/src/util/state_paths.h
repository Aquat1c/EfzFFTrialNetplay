#pragma once

#include <filesystem>
#include <string>

namespace thv::paths
{

const std::filesystem::path& MutableStateRoot();
const char* MutableStateRootSource();

// Asset/legacy-read location only. New mutable files must use MutableStateRoot().
std::filesystem::path GameDirectory();

// Unlike path::string(), independent of the Wine prefix's ANSI code page.
std::string PathUtf8(const std::filesystem::path& path) noexcept;

}
