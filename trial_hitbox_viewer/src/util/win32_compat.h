#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace thv::win32
{

struct LoadedPeIdentity
{
    std::uint16_t machine = 0u;
    std::uint16_t optionalMagic = 0u;
    std::uint16_t characteristics = 0u;
    std::uint32_t timeDateStamp = 0u;
    std::uint32_t addressOfEntryPoint = 0u;
    std::uint32_t preferredImageBase = 0u;
    std::uint32_t sizeOfImage = 0u;
    std::uint32_t relocationRva = 0u;
    std::uint32_t relocationBytes = 0u;
    std::uintptr_t runtimeImageBase = 0u;
};

enum class TrialHostStatus
{
    Ok,
    NullModule,
    InvalidDosHeader,
    InvalidNtHeader,
    WrongMachine,
    WrongOptionalHeader,
    WrongTimestamp,
    WrongEntryPoint,
    WrongImageSize,
    WrongPreferredBase,
    WrongRelocationContract,
    RelocatedWithoutRelocations,
};

bool ReadLoadedPeIdentity(HMODULE module, LoadedPeIdentity* out) noexcept;
TrialHostStatus ValidateTrialHostIdentity(const LoadedPeIdentity& identity) noexcept;
TrialHostStatus ValidateTrialHostImage(HMODULE module, LoadedPeIdentity* out = nullptr) noexcept;
const char* TrialHostStatusName(TrialHostStatus status) noexcept;

void** FindImportAddressSlot(
    HMODULE targetModule,
    const char* dllName,
    const char* functionName) noexcept;
// `outOriginal` is populated before exposure: point it at the storage the hook reads.
bool PatchImportAddressSlot(
    HMODULE targetModule,
    const char* dllName,
    const char* functionName,
    void* replacement,
    void** outOriginal) noexcept;
bool RestoreImportAddressSlot(
    HMODULE targetModule,
    const char* dllName,
    const char* functionName,
    void* expectedReplacement,
    void* original) noexcept;

bool IsReadableRange(const void* address, std::size_t size) noexcept;
bool IsAddressInModule(HMODULE module, const void* address) noexcept;

struct RuntimeInfo
{
    bool wine = false;
    char wineVersion[64] = {};
    char wineBuildId[128] = {};
    char hostSystem[64] = {};
    char hostRelease[128] = {};
};

RuntimeInfo QueryRuntimeInfo() noexcept;
bool IsWineRuntime() noexcept;

bool SecureRandomBytes(void* output, std::size_t size) noexcept;
bool PrewarmSecureRandomProvider() noexcept;

// Correct across the 32-bit tick wrap while deadlines are under 2^31 ms away.
inline bool TickDeadlineReached(const std::uint32_t now, const std::uint32_t deadline) noexcept
{
    return deadline != 0u && static_cast<std::int32_t>(now - deadline) >= 0;
}

}
