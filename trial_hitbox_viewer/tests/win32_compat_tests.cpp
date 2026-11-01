#include "win32_compat.h"
#include "state_paths.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <filesystem>

namespace
{

DWORD WINAPI FakeGetCurrentProcessId()
{
    return 0x13572468u;
}

int Fail(const char* const message)
{
    std::fprintf(stderr, "win32 compatibility test failed: %s\n", message);
    return 1;
}

}

int main()
{
    using namespace thv::win32;

    wchar_t tempDirectory[MAX_PATH] = {};
    if (GetTempPathW(static_cast<DWORD>(std::size(tempDirectory)), tempDirectory) == 0u)
    {
        return Fail("GetTempPathW failed");
    }
    wchar_t stateLeaf[96] = {};
    std::swprintf(
        stateLeaf,
        std::size(stateLeaf),
        L"trial_netplay_path_test_\x0442\x0435\x0441\x0442_%lu",
        static_cast<unsigned long>(GetCurrentProcessId()));
    const std::filesystem::path expectedStateRoot =
        std::filesystem::path(tempDirectory) / stateLeaf;
    if (!SetEnvironmentVariableW(
            L"TRIAL_NETPLAY_STATE_DIR", expectedStateRoot.c_str()))
    {
        return Fail("could not set state-root override");
    }
    const std::filesystem::path selectedStateRoot = thv::paths::MutableStateRoot();
    if (selectedStateRoot != expectedStateRoot
        || std::strcmp(thv::paths::MutableStateRootSource(), "TRIAL_NETPLAY_STATE_DIR") != 0)
    {
        return Fail("explicit writable state root was not selected");
    }
    const std::string stateRootUtf8 = thv::paths::PathUtf8(selectedStateRoot);
    if (stateRootUtf8.empty()
        || stateRootUtf8.find("\xD1\x82\xD0\xB5\xD1\x81\xD1\x82") == std::string::npos)
    {
        return Fail("Unicode state-root path was not represented as UTF-8");
    }

    LoadedPeIdentity canonical{};
    canonical.machine = IMAGE_FILE_MACHINE_I386;
    canonical.optionalMagic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    canonical.characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_32BIT_MACHINE
        | IMAGE_FILE_RELOCS_STRIPPED;
    canonical.timeDateStamp = 0x3AD0ACC2u;
    canonical.addressOfEntryPoint = 0x00065570u;
    canonical.preferredImageBase = 0x00400000u;
    canonical.sizeOfImage = 0x008D2000u;
    canonical.runtimeImageBase = 0x00400000u;
    if (ValidateTrialHostIdentity(canonical) != TrialHostStatus::Ok)
    {
        return Fail("canonical Trial identity rejected");
    }

    LoadedPeIdentity wrong = canonical;
    wrong.timeDateStamp ^= 1u;
    if (ValidateTrialHostIdentity(wrong) != TrialHostStatus::WrongTimestamp)
    {
        return Fail("wrong Trial timestamp accepted");
    }
    wrong = canonical;
    wrong.addressOfEntryPoint ^= 1u;
    if (ValidateTrialHostIdentity(wrong) != TrialHostStatus::WrongEntryPoint)
    {
        return Fail("wrong Trial entry point accepted");
    }
    wrong = canonical;
    wrong.characteristics &= static_cast<std::uint16_t>(~IMAGE_FILE_RELOCS_STRIPPED);
    if (ValidateTrialHostIdentity(wrong) != TrialHostStatus::WrongRelocationContract)
    {
        return Fail("Trial image with relocations-not-stripped accepted");
    }
    wrong = canonical;
    wrong.relocationRva = 0x1000u;
    wrong.relocationBytes = 12u;
    if (ValidateTrialHostIdentity(wrong) != TrialHostStatus::WrongRelocationContract)
    {
        return Fail("Trial image with relocation directory accepted");
    }
    wrong = canonical;
    wrong.runtimeImageBase = 0x00500000u;
    if (ValidateTrialHostIdentity(wrong) != TrialHostStatus::RelocatedWithoutRelocations)
    {
        return Fail("non-relocatable rebased Trial image accepted");
    }

    if (!TickDeadlineReached(0x00000010u, 0xFFFFFFF0u)
        || TickDeadlineReached(0xFFFFFFF0u, 0x00000010u)
        || TickDeadlineReached(123u, 0u))
    {
        return Fail("32-bit tick wrap comparison is incorrect");
    }

    void* const page = VirtualAlloc(nullptr, 4096u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (page == nullptr)
    {
        return Fail("VirtualAlloc failed");
    }
    if (!IsReadableRange(page, 4096u))
    {
        VirtualFree(page, 0u, MEM_RELEASE);
        return Fail("readable committed page rejected");
    }
    DWORD oldProtect = 0u;
    if (!VirtualProtect(page, 4096u, PAGE_NOACCESS, &oldProtect)
        || IsReadableRange(page, 1u))
    {
        VirtualFree(page, 0u, MEM_RELEASE);
        return Fail("no-access page accepted as readable");
    }
    VirtualFree(page, 0u, MEM_RELEASE);

    const DWORD pid = GetCurrentProcessId();
    if (pid == 0u)
    {
        return Fail("GetCurrentProcessId returned zero");
    }
    HMODULE const self = GetModuleHandleW(nullptr);
    void** const pidSlot = FindImportAddressSlot(
        self, "KERNEL32.dll", "GetCurrentProcessId");
    if (pidSlot == nullptr || !IsReadableRange(pidSlot, sizeof(void*)) || *pidSlot == nullptr)
    {
        return Fail("could not resolve a named kernel32 IAT slot");
    }
    void* originalPid = nullptr;
    if (!PatchImportAddressSlot(
            self,
            "KERNEL32.dll",
            "GetCurrentProcessId",
            reinterpret_cast<void*>(&FakeGetCurrentProcessId),
            &originalPid)
        || *pidSlot != reinterpret_cast<void*>(&FakeGetCurrentProcessId)
        || !RestoreImportAddressSlot(
            self,
            "KERNEL32.dll",
            "GetCurrentProcessId",
            reinterpret_cast<void*>(&FakeGetCurrentProcessId),
            originalPid)
        || *pidSlot != originalPid)
    {
        return Fail("named IAT patch/restore contract failed");
    }

    LoadedPeIdentity selfIdentity{};
    if (!ReadLoadedPeIdentity(self, &selfIdentity)
        || !IsAddressInModule(self, reinterpret_cast<const void*>(&main)))
    {
        return Fail("loaded PE/module range probe failed");
    }

    std::array<std::uint8_t, 32> randomA{};
    std::array<std::uint8_t, 32> randomB{};
    if (!PrewarmSecureRandomProvider()
        || !SecureRandomBytes(randomA.data(), randomA.size())
        || !SecureRandomBytes(randomB.data(), randomB.size())
        || randomA == randomB)
    {
        return Fail("secure random provider/fallback unavailable");
    }

    const RuntimeInfo runtime = QueryRuntimeInfo();
    SetEnvironmentVariableW(L"TRIAL_NETPLAY_STATE_DIR", nullptr);
    RemoveDirectoryW(expectedStateRoot.c_str());
    std::printf(
        "win32 compatibility tests passed runtime=%s version=%s host=%s %s\n",
        runtime.wine ? "Wine" : "Windows",
        runtime.wineVersion,
        runtime.hostSystem,
        runtime.hostRelease);
    return 0;
}
