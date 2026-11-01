#include "trial_versions.h"
#include "version_info.h"

#include <filesystem>

namespace
{

bool Matches(
    const trial_caster::BinaryVersion& actual,
    const trialnet::version::Version& expected)
{
    return actual.major == expected.major
        && actual.minor == expected.minor
        && actual.patch == expected.patch
        && actual.build == expected.build;
}

trial_caster::BinaryVersion ToBinaryVersion(const trialnet::version::Version& version)
{
    return {version.major, version.minor, version.patch, version.build};
}

}

int wmain(const int argc, wchar_t** const argv)
{
    if (argc != 3)
    {
        return 1;
    }

    const auto launcher = trial_caster::QueryBinaryVersion(argv[1]);
    if (!launcher.available
        || launcher.display
            != trial_caster::FormatBinaryVersion(ToBinaryVersion(trialnet::version::kLauncher))
        || !Matches(launcher.value, trialnet::version::kLauncher))
    {
        return 2;
    }

    const auto dll = trial_caster::QueryBinaryVersion(argv[2]);
    if (!dll.available
        || dll.display
            != trial_caster::FormatBinaryVersion(ToBinaryVersion(trialnet::version::kNetplayDll))
        || !Matches(dll.value, trialnet::version::kNetplayDll))
    {
        return 3;
    }

    const auto unavailable = trial_caster::QueryBinaryVersion(
        std::filesystem::path(argv[1]).parent_path() / L"definitely-missing-netplay-component.bin");
    if (unavailable.available
        || unavailable.display != L"Unavailable"
        || unavailable.error.empty())
    {
        return 4;
    }

    const trial_caster::BinaryVersion fourPart{2u, 3u, 4u, 5u};
    if (trial_caster::FormatBinaryVersion(fourPart) != L"2.3.4.5")
    {
        return 5;
    }
    return 0;
}
