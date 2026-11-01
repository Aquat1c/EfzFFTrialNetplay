#include "win32_compat.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace thv::win32
{
namespace
{

constexpr std::uint16_t kTrialMachine = IMAGE_FILE_MACHINE_I386;
constexpr std::uint16_t kTrialOptionalMagic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
constexpr std::uint32_t kTrialTimestamp = 0x3AD0ACC2u; // 2001-04-08 Trial EFZ
constexpr std::uint32_t kTrialEntryPointRva = 0x00065570u; // VA 0x00465570
constexpr std::uint32_t kTrialPreferredBase = 0x00400000u;
constexpr std::uint32_t kTrialImageSize = 0x008D2000u;

template <typename T>
T* RvaPointer(
    const std::uintptr_t base,
    const std::size_t imageSize,
    const std::uint32_t rva,
    const std::size_t count = 1u) noexcept
{
    if (count == 0u || rva >= imageSize
        || count > (std::numeric_limits<std::size_t>::max() / sizeof(T)))
    {
        return nullptr;
    }
    const std::size_t bytes = count * sizeof(T);
    if (bytes > imageSize - static_cast<std::size_t>(rva))
    {
        return nullptr;
    }
    return reinterpret_cast<T*>(base + rva);
}

bool StringEqualsInsensitiveInImage(
    const char* candidate,
    const char* expected,
    const std::uintptr_t imageBase,
    const std::size_t imageSize) noexcept
{
    if (candidate == nullptr || expected == nullptr)
    {
        return false;
    }
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(candidate);
    if (address < imageBase || address >= imageBase + imageSize)
    {
        return false;
    }
    const std::size_t available = imageSize - static_cast<std::size_t>(address - imageBase);
    for (std::size_t index = 0u; index < available; ++index)
    {
        const unsigned char left = static_cast<unsigned char>(candidate[index]);
        const unsigned char right = static_cast<unsigned char>(expected[index]);
        const unsigned char foldedLeft = left >= 'A' && left <= 'Z'
            ? static_cast<unsigned char>(left - 'A' + 'a')
            : left;
        const unsigned char foldedRight = right >= 'A' && right <= 'Z'
            ? static_cast<unsigned char>(right - 'A' + 'a')
            : right;
        if (foldedLeft != foldedRight)
        {
            return false;
        }
        if (left == '\0')
        {
            return true;
        }
    }
    return false;
}

void CopyRuntimeString(char* destination, const std::size_t capacity, const char* source) noexcept
{
    if (destination == nullptr || capacity == 0u)
    {
        return;
    }
    destination[0] = '\0';
    if (source == nullptr)
    {
        return;
    }
    std::size_t index = 0u;
    for (; index + 1u < capacity && source[index] != '\0'; ++index)
    {
        destination[index] = source[index];
    }
    destination[index] = '\0';
}

using BCryptGenRandomFn = LONG(WINAPI*)(void*, unsigned char*, ULONG, ULONG);
using RtlGenRandomFn = BOOLEAN(WINAPI*)(PVOID, ULONG);

struct RandomProvider
{
    HMODULE bcryptModule = nullptr; // intentionally held until process teardown
    HMODULE rtlModule = nullptr;
    BCryptGenRandomFn bcrypt = nullptr;
    RtlGenRandomFn rtl = nullptr;
};

const RandomProvider& GetRandomProvider() noexcept
{
    static const RandomProvider provider = []() noexcept
    {
        RandomProvider result{};
        result.bcryptModule = LoadLibraryW(L"bcrypt.dll");
        if (result.bcryptModule != nullptr)
        {
            result.bcrypt = reinterpret_cast<BCryptGenRandomFn>(
                GetProcAddress(result.bcryptModule, "BCryptGenRandom"));
            if (result.bcrypt == nullptr)
            {
                FreeLibrary(result.bcryptModule);
                result.bcryptModule = nullptr;
            }
        }

        // Resolve the fallback regardless: older Wine can export BCryptGenRandom as a failing stub.
        result.rtlModule = LoadLibraryW(L"advapi32.dll");
        if (result.rtlModule != nullptr)
        {
            result.rtl = reinterpret_cast<RtlGenRandomFn>(
                GetProcAddress(result.rtlModule, "SystemFunction036"));
            if (result.rtl == nullptr)
            {
                FreeLibrary(result.rtlModule);
                result.rtlModule = nullptr;
            }
        }
        return result;
    }();
    return provider;
}

}

bool ReadLoadedPeIdentity(const HMODULE module, LoadedPeIdentity* const out) noexcept
{
    if (module == nullptr || out == nullptr)
    {
        return false;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000)
    {
        return false;
    }
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE
        || nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32))
    {
        return false;
    }

    LoadedPeIdentity identity{};
    identity.machine = nt->FileHeader.Machine;
    identity.optionalMagic = nt->OptionalHeader.Magic;
    identity.characteristics = nt->FileHeader.Characteristics;
    identity.timeDateStamp = nt->FileHeader.TimeDateStamp;
    identity.addressOfEntryPoint = nt->OptionalHeader.AddressOfEntryPoint;
    identity.preferredImageBase = nt->OptionalHeader.ImageBase;
    identity.sizeOfImage = nt->OptionalHeader.SizeOfImage;
    identity.relocationRva =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
    identity.relocationBytes =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
    identity.runtimeImageBase = base;
    *out = identity;
    return true;
}

TrialHostStatus ValidateTrialHostIdentity(const LoadedPeIdentity& identity) noexcept
{
    if (identity.machine != kTrialMachine)
    {
        return TrialHostStatus::WrongMachine;
    }
    if (identity.optionalMagic != kTrialOptionalMagic)
    {
        return TrialHostStatus::WrongOptionalHeader;
    }
    if (identity.timeDateStamp != kTrialTimestamp)
    {
        return TrialHostStatus::WrongTimestamp;
    }
    if (identity.addressOfEntryPoint != kTrialEntryPointRva)
    {
        return TrialHostStatus::WrongEntryPoint;
    }
    if (identity.sizeOfImage != kTrialImageSize)
    {
        return TrialHostStatus::WrongImageSize;
    }
    if (identity.preferredImageBase != kTrialPreferredBase)
    {
        return TrialHostStatus::WrongPreferredBase;
    }
    if ((identity.characteristics & IMAGE_FILE_RELOCS_STRIPPED) == 0u
        || identity.relocationRva != 0u
        || identity.relocationBytes != 0u)
    {
        return TrialHostStatus::WrongRelocationContract;
    }
    if (identity.runtimeImageBase != identity.preferredImageBase)
    {
        return TrialHostStatus::RelocatedWithoutRelocations;
    }
    return TrialHostStatus::Ok;
}

TrialHostStatus ValidateTrialHostImage(
    const HMODULE module,
    LoadedPeIdentity* const out) noexcept
{
    if (module == nullptr)
    {
        return TrialHostStatus::NullModule;
    }
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000)
    {
        return TrialHostStatus::InvalidDosHeader;
    }
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
        reinterpret_cast<const std::uint8_t*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
    {
        return TrialHostStatus::InvalidNtHeader;
    }
    LoadedPeIdentity identity{};
    if (!ReadLoadedPeIdentity(module, &identity))
    {
        return TrialHostStatus::InvalidNtHeader;
    }
    if (out != nullptr)
    {
        *out = identity;
    }
    return ValidateTrialHostIdentity(identity);
}

const char* TrialHostStatusName(const TrialHostStatus status) noexcept
{
    switch (status)
    {
    case TrialHostStatus::Ok: return "ok";
    case TrialHostStatus::NullModule: return "null module";
    case TrialHostStatus::InvalidDosHeader: return "invalid DOS header";
    case TrialHostStatus::InvalidNtHeader: return "invalid NT header";
    case TrialHostStatus::WrongMachine: return "not 32-bit x86";
    case TrialHostStatus::WrongOptionalHeader: return "not a PE32 image";
    case TrialHostStatus::WrongTimestamp: return "wrong EFZ build timestamp";
    case TrialHostStatus::WrongEntryPoint: return "wrong EFZ entry point";
    case TrialHostStatus::WrongImageSize: return "wrong EFZ image size";
    case TrialHostStatus::WrongPreferredBase: return "wrong EFZ preferred base";
    case TrialHostStatus::WrongRelocationContract: return "unexpected EFZ relocation contract";
    case TrialHostStatus::RelocatedWithoutRelocations: return "non-relocatable EFZ image was rebased";
    default: return "unknown";
    }
}

void** FindImportAddressSlot(
    const HMODULE targetModule,
    const char* const dllName,
    const char* const functionName) noexcept
{
    if (targetModule == nullptr || dllName == nullptr || functionName == nullptr)
    {
        return nullptr;
    }

    LoadedPeIdentity identity{};
    if (!ReadLoadedPeIdentity(targetModule, &identity) || identity.sizeOfImage == 0u)
    {
        return nullptr;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(targetModule);
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress == 0u || imports.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR))
    {
        return nullptr;
    }

    const std::size_t descriptorCount = imports.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    auto* const descriptors = RvaPointer<IMAGE_IMPORT_DESCRIPTOR>(
        base, identity.sizeOfImage, imports.VirtualAddress, descriptorCount);
    if (descriptors == nullptr)
    {
        return nullptr;
    }

    for (std::size_t descriptorIndex = 0u; descriptorIndex < descriptorCount; ++descriptorIndex)
    {
        const IMAGE_IMPORT_DESCRIPTOR& descriptor = descriptors[descriptorIndex];
        if (descriptor.Name == 0u)
        {
            break;
        }
        const char* const candidateDll = RvaPointer<char>(
            base, identity.sizeOfImage, descriptor.Name);
        if (!StringEqualsInsensitiveInImage(
                candidateDll, dllName, base, identity.sizeOfImage))
        {
            continue;
        }

        // Without a name thunk, a bound IAT holds addresses, not name RVAs: don't dereference.
        if (descriptor.OriginalFirstThunk == 0u || descriptor.FirstThunk == 0u)
        {
            return nullptr;
        }
        auto* const names = RvaPointer<IMAGE_THUNK_DATA32>(
            base, identity.sizeOfImage, descriptor.OriginalFirstThunk);
        auto* const live = RvaPointer<IMAGE_THUNK_DATA32>(
            base, identity.sizeOfImage, descriptor.FirstThunk);
        if (names == nullptr || live == nullptr)
        {
            return nullptr;
        }

        const std::size_t maxNames =
            (identity.sizeOfImage - descriptor.OriginalFirstThunk) / sizeof(IMAGE_THUNK_DATA32);
        const std::size_t maxLive =
            (identity.sizeOfImage - descriptor.FirstThunk) / sizeof(IMAGE_THUNK_DATA32);
        const std::size_t maxThunks = (std::min)(maxNames, maxLive);
        for (std::size_t thunkIndex = 0u; thunkIndex < maxThunks; ++thunkIndex)
        {
            if (names[thunkIndex].u1.AddressOfData == 0u)
            {
                break;
            }
            if (IMAGE_SNAP_BY_ORDINAL32(names[thunkIndex].u1.Ordinal))
            {
                continue;
            }
            auto* const importName = RvaPointer<IMAGE_IMPORT_BY_NAME>(
                base, identity.sizeOfImage, names[thunkIndex].u1.AddressOfData);
            if (importName == nullptr
                || !StringEqualsInsensitiveInImage(
                    reinterpret_cast<const char*>(importName->Name),
                    functionName,
                    base,
                    identity.sizeOfImage))
            {
                continue;
            }
            return reinterpret_cast<void**>(&live[thunkIndex].u1.Function);
        }
        return nullptr;
    }
    return nullptr;
}

bool PatchImportAddressSlot(
    const HMODULE targetModule,
    const char* const dllName,
    const char* const functionName,
    void* const replacement,
    void** const outOriginal) noexcept
{
    if (replacement == nullptr || outOriginal == nullptr)
    {
        return false;
    }
    *outOriginal = nullptr;
    void** const slot = FindImportAddressSlot(targetModule, dllName, functionName);
    if (slot == nullptr)
    {
        return false;
    }
    void* const original = *slot;
    if (original == nullptr || original == replacement)
    {
        return false;
    }
    // Publish the forwarding target first: the replacement can run once the CAS is visible.
    *outOriginal = original;
    DWORD oldProtect = 0u;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
    {
        *outOriginal = nullptr;
        return false;
    }
    void* const observed = InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(slot), replacement, original);
    if (observed != original && observed != replacement)
    {
        DWORD ignored = 0u;
        (void)VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
        *outOriginal = nullptr;
        return false;
    }
    DWORD ignored = 0u;
    const BOOL restored = VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    if (restored != FALSE)
    {
        return true;
    }

    // Already exposed: never roll back the forwarding record.
    OutputDebugStringA(
        "netplay: IAT slot installed but original page protection could not be restored\n");
    return true;
}

bool RestoreImportAddressSlot(
    const HMODULE targetModule,
    const char* const dllName,
    const char* const functionName,
    void* const expectedReplacement,
    void* const original) noexcept
{
    if (original == nullptr)
    {
        return false;
    }
    void** const slot = FindImportAddressSlot(targetModule, dllName, functionName);
    if (slot == nullptr || (expectedReplacement != nullptr && *slot != expectedReplacement))
    {
        return false;
    }
    DWORD oldProtect = 0u;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
    {
        return false;
    }
    if (expectedReplacement != nullptr)
    {
        if (InterlockedCompareExchangePointer(
                reinterpret_cast<void* volatile*>(slot), original, expectedReplacement)
            != expectedReplacement)
        {
            DWORD ignored = 0u;
            (void)VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
            return false;
        }
    }
    else
    {
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), original);
    }
    DWORD ignored = 0u;
    return VirtualProtect(slot, sizeof(void*), oldProtect, &ignored) != FALSE;
}

bool IsReadableRange(const void* const address, const std::size_t size) noexcept
{
    if (address == nullptr || size == 0u)
    {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (size > std::numeric_limits<std::uintptr_t>::max() - begin)
    {
        return false;
    }
    const std::uintptr_t end = begin + size;
    std::uintptr_t cursor = begin;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) == 0u
            || mbi.State != MEM_COMMIT
            || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0u)
        {
            return false;
        }
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
            | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & readable) == 0u)
        {
            return false;
        }
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cursor)
        {
            return false;
        }
        cursor = (std::min<std::uintptr_t>)(regionEnd, end);
    }
    return true;
}

bool IsAddressInModule(const HMODULE module, const void* const address) noexcept
{
    if (module == nullptr || address == nullptr)
    {
        return false;
    }
    LoadedPeIdentity identity{};
    if (!ReadLoadedPeIdentity(module, &identity))
    {
        return false;
    }
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    return value >= identity.runtimeImageBase
        && value - identity.runtimeImageBase < identity.sizeOfImage;
}

RuntimeInfo QueryRuntimeInfo() noexcept
{
    RuntimeInfo info{};
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr)
    {
        return info;
    }
    using WineGetStringFn = const char*(__cdecl*)();
    using WineGetHostVersionFn = void(__cdecl*)(const char**, const char**);
    const auto version = reinterpret_cast<WineGetStringFn>(
        GetProcAddress(ntdll, "wine_get_version"));
    if (version == nullptr)
    {
        return info;
    }
    info.wine = true;
    CopyRuntimeString(info.wineVersion, sizeof(info.wineVersion), version());
    const auto build = reinterpret_cast<WineGetStringFn>(
        GetProcAddress(ntdll, "wine_get_build_id"));
    if (build != nullptr)
    {
        CopyRuntimeString(info.wineBuildId, sizeof(info.wineBuildId), build());
    }
    const auto host = reinterpret_cast<WineGetHostVersionFn>(
        GetProcAddress(ntdll, "wine_get_host_version"));
    if (host != nullptr)
    {
        const char* system = nullptr;
        const char* release = nullptr;
        host(&system, &release);
        CopyRuntimeString(info.hostSystem, sizeof(info.hostSystem), system);
        CopyRuntimeString(info.hostRelease, sizeof(info.hostRelease), release);
    }
    return info;
}

bool IsWineRuntime() noexcept
{
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

bool SecureRandomBytes(void* const output, const std::size_t size) noexcept
{
    if (output == nullptr || size == 0u
        || size > static_cast<std::size_t>((std::numeric_limits<ULONG>::max)()))
    {
        return false;
    }

    constexpr ULONG kUseSystemPreferredRng = 0x00000002u;
    const RandomProvider& provider = GetRandomProvider();
    if (provider.bcrypt != nullptr)
    {
        if (provider.bcrypt(
            nullptr,
            static_cast<unsigned char*>(output),
            static_cast<ULONG>(size),
            kUseSystemPreferredRng) >= 0)
        {
            return true;
        }
    }
    return provider.rtl != nullptr
        && provider.rtl(output, static_cast<ULONG>(size)) != FALSE;
}

bool PrewarmSecureRandomProvider() noexcept
{
    std::uint8_t probe = 0u;
    return SecureRandomBytes(&probe, sizeof(probe));
}

}
