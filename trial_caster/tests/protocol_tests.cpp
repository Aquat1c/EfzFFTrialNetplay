#include "autopunch_protocol.h"
#include "trial_compatibility.h"
#include "../src/delay_recommendation.h"
#include "../src/protocol.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

int main(const int argc, char** const argv)
{
    using namespace trialnet::autopunch;

    const RegisterPacket registration = MakeRegister(7500u);
    if (registration[0] != 0x1Du || registration[1] != 0x4Cu)
    {
        return 1;
    }

    const std::uint32_t address = 0x04030201u;
    const LookupPacket lookup = MakeLookup(49152u, address, 7500u);
    if (lookup[0] != 0xC0u || lookup[1] != 0x00u
        || lookup[6] != 0x1Du || lookup[7] != 0x4Cu)
    {
        return 2;
    }
    std::uint32_t encodedAddress = 0;
    std::memcpy(&encodedAddress, lookup.data() + 2u, sizeof(encodedAddress));
    if (encodedAddress != address)
    {
        return 3;
    }

    std::uint8_t mappingBytes[8] = {0x1D, 0x4C, 0xC3, 0x50, 1, 2, 3, 4};
    Mapping mapping{};
    if (!ParseMapping(mappingBytes, sizeof(mappingBytes), &mapping)
        || mapping.internalPort != 7500u
        || mapping.natPort != 50000u)
    {
        return 4;
    }
    std::uint32_t mappingAddress = 0;
    std::memcpy(&mappingAddress, mappingBytes + 4u, sizeof(mappingAddress));
    if (mapping.hostNetworkOrder != mappingAddress)
    {
        return 5;
    }

    if (trialnet::compat::kCompatibilityId
        != ((static_cast<std::uint32_t>(trialnet::compat::kLauncherProtocolMajor) << 16u)
            | trialnet::compat::kCompatibilityVersion))
    {
        return 6;
    }

    using namespace trial_caster::proto;
    if (sizeof(InitializationConfigurationPacket) != 8u
        || sizeof(InitializationAckPacket) != 3u
        || sizeof(LaunchGoPacket) != 9u)
    {
        return 10;
    }
    if (PracticeKeysMutuallyEnabled(0u, 0u)
        || PracticeKeysMutuallyEnabled(kCapPracticeKeys, 0u)
        || PracticeKeysMutuallyEnabled(0u, kCapPracticeKeys)
        || !PracticeKeysMutuallyEnabled(kCapPracticeKeys, kCapPracticeKeys))
    {
        return 11;
    }
    if (LocalCapabilities(true, false) != kCapRollback
        || LocalCapabilities(false, false) != kCapLockstep
        || LocalCapabilities(true, true) != (kCapRollback | kCapPracticeKeys))
    {
        return 12;
    }
    if (!IsSupportedTargetFps(60u)
        || !IsSupportedTargetFps(64u)
        || IsSupportedTargetFps(0u)
        || IsSupportedTargetFps(63u))
    {
        return 13;
    }
    LaunchGoPacket launchGo{};
    launchGo.targetFps = 64u;
    if (!IsValidLaunchGoPacket(launchGo))
    {
        return 14;
    }
    launchGo.targetFps = 63u;
    if (IsValidLaunchGoPacket(launchGo))
    {
        return 15;
    }
    launchGo.targetFps = 60u;
    launchGo.launchAtMs = 0u;
    if (IsValidLaunchGoPacket(launchGo))
    {
        return 16;
    }
    launchGo.launchAtMs = kLaunchSyncLeadMs;
    launchGo.reserved[1] = 1u;
    if (IsValidLaunchGoPacket(launchGo))
    {
        return 17;
    }

    if (trial_caster::timing::RecommendedInitialDelay(32u, 0, 60u) != 2
        || trial_caster::timing::RecommendedInitialDelay(32u, 0, 64u) != 3
        || trial_caster::timing::RecommendedInitialDelay(32u, 1, 60u) != 1
        || trial_caster::timing::RecommendedInitialDelay(32u, 1, 64u) != 2)
    {
        return 18;
    }
    if (trial_caster::timing::RecommendedInitialDelay(32u, 0, 63u)
        != trial_caster::timing::RecommendedInitialDelay(32u, 0, 60u))
    {
        return 19;
    }

    if (argc >= 2)
    {
        HMODULE const module = LoadLibraryExA(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (module == nullptr)
        {
            return 7;
        }
        FARPROC compatibilityExport = GetProcAddress(module, "TrialNetplayCompatibilityId");
        if (compatibilityExport == nullptr)
        {
            compatibilityExport = GetProcAddress(module, "_TrialNetplayCompatibilityId");
        }
        if (compatibilityExport == nullptr)
        {
            FreeLibrary(module);
            return 8;
        }
        const std::uint32_t dllCompatibility =
            *reinterpret_cast<const std::uint32_t*>(compatibilityExport);
        FreeLibrary(module);
        if (dllCompatibility != trialnet::compat::kCompatibilityId)
        {
            return 9;
        }
    }
    return 0;
}
