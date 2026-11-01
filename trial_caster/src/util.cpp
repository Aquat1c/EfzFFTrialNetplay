#include "util.h"

#include "session_log.h"

#include <ws2tcpip.h>

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cwctype>
#include <fstream>
#include <limits>
#include <vector>

namespace trial_caster::util
{
namespace
{

constexpr DWORD kResolverTimeoutMs = 3000u;
constexpr std::size_t kResolverHostCapacity = 256u;

struct ResolveIpv4Context
{
    volatile LONG references = 1;
    HANDLE completed = nullptr;
    wchar_t host[kResolverHostCapacity] = {};
    wchar_t resolved[INET_ADDRSTRLEN] = {};
};

// GetAddrInfoW cannot be cancelled on older Wine; keep one abandoned resolver at most.
PVOID volatile g_resolverInFlight = nullptr;

void ReleaseResolveContext(ResolveIpv4Context* const context)
{
    if (context != nullptr && InterlockedDecrement(&context->references) == 0)
    {
        if (context->completed != nullptr)
        {
            CloseHandle(context->completed);
        }
        HeapFree(GetProcessHeap(), 0u, context);
    }
}

DWORD WINAPI ResolveIpv4Worker(void* const parameter)
{
    auto* const context = static_cast<ResolveIpv4Context*>(parameter);
    ADDRINFOW hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    PADDRINFOW result = nullptr;
    if (GetAddrInfoW(context->host, nullptr, &hints, &result) == 0 && result != nullptr)
    {
        for (PADDRINFOW cursor = result; cursor != nullptr; cursor = cursor->ai_next)
        {
            if (cursor->ai_family != AF_INET
                || cursor->ai_addr == nullptr
                || cursor->ai_addrlen < sizeof(sockaddr_in))
            {
                continue;
            }
            const auto* const address = reinterpret_cast<const sockaddr_in*>(cursor->ai_addr);
            if (InetNtopW(
                    AF_INET,
                    const_cast<IN_ADDR*>(&address->sin_addr),
                    context->resolved,
                    static_cast<DWORD>(std::size(context->resolved))) != nullptr)
            {
                break;
            }
        }
        FreeAddrInfoW(result);
    }
    SetEvent(context->completed);
    (void)InterlockedCompareExchangePointer(
        &g_resolverInFlight,
        nullptr,
        context);
    ReleaseResolveContext(context);
    return 0u;
}

std::wstring ResolveIpv4WithDeadline(
    const std::wstring& host,
    const std::atomic<bool>* const cancel)
{
    if (host.empty() || host.size() >= kResolverHostCapacity)
    {
        return {};
    }
    auto* const context = static_cast<ResolveIpv4Context*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ResolveIpv4Context)));
    if (context == nullptr)
    {
        return {};
    }
    context->references = 1;
    context->completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (context->completed == nullptr)
    {
        ReleaseResolveContext(context);
        return {};
    }
    std::copy(host.begin(), host.end(), context->host);

    if (InterlockedCompareExchangePointer(
            &g_resolverInFlight,
            context,
            nullptr) != nullptr)
    {
        SessionLog::Warn(
            L"network",
            L"DNS lookup skipped because a previous bounded lookup is still pending");
        ReleaseResolveContext(context);
        return {};
    }

    InterlockedIncrement(&context->references);
    HANDLE const worker = CreateThread(
        nullptr, 0u, ResolveIpv4Worker, context, 0u, nullptr);
    if (worker == nullptr)
    {
        (void)InterlockedCompareExchangePointer(
            &g_resolverInFlight,
            nullptr,
            context);
        ReleaseResolveContext(context);
        ReleaseResolveContext(context);
        return {};
    }
    CloseHandle(worker);

    const ULONGLONG deadline = GetTickCount64() + kResolverTimeoutMs;
    bool completed = false;
    while (GetTickCount64() < deadline)
    {
        if (cancel != nullptr && cancel->load())
        {
            break;
        }
        const ULONGLONG remaining = deadline - GetTickCount64();
        const DWORD waitMs = static_cast<DWORD>((std::min<ULONGLONG>)(remaining, 50u));
        if (WaitForSingleObject(context->completed, waitMs) == WAIT_OBJECT_0)
        {
            completed = true;
            break;
        }
    }
    const std::wstring resolved = completed ? std::wstring(context->resolved) : std::wstring{};
    ReleaseResolveContext(context);
    return resolved;
}

std::uint32_t RotateLeft32(std::uint32_t value, unsigned count)
{
    count &= 31u;
    return (value << count) | (value >> (32u - count));
}

std::wstring EnvironmentValue(const wchar_t* const name)
{
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0u);
    if (required == 0u)
    {
        return {};
    }
    std::vector<wchar_t> value(static_cast<std::size_t>(required), L'\0');
    const DWORD written = GetEnvironmentVariableW(
        name,
        value.data(),
        static_cast<DWORD>(value.size()));
    if (written == 0u || written >= value.size())
    {
        return {};
    }
    return std::wstring(value.data(), written);
}

bool EnsureWritableDirectory(const std::filesystem::path& directory)
{
    if (directory.empty())
    {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec || !std::filesystem::is_directory(directory, ec))
    {
        return false;
    }

    const std::filesystem::path probe = directory / Format(
        L".trial_write_probe_%lu_%llu.tmp",
        static_cast<unsigned long>(GetCurrentProcessId()),
        GetTickCount64());
    HANDLE const handle = CreateFileW(
        probe.c_str(),
        GENERIC_WRITE,
        0u,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    CloseHandle(handle);
    DeleteFileW(probe.c_str());
    return true;
}

bool CheckedRange(const std::size_t offset, const std::size_t length, const std::size_t size)
{
    return offset <= size && length <= size - offset;
}

bool InspectPeNamedExport(
    const std::filesystem::path& dllPath,
    const char* const* const requestedNames,
    const std::size_t requestedNameCount,
    std::uint32_t* const exportValue,
    const bool readExportValue,
    const wchar_t* const fileLabel,
    std::wstring* const error)
{
    std::error_code fileError;
    const std::uintmax_t fileSize = std::filesystem::file_size(dllPath, fileError);
    constexpr std::uintmax_t kMaximumInspectionBytes = 256u * 1024u * 1024u;
    if (fileError || fileSize < sizeof(IMAGE_DOS_HEADER) || fileSize > kMaximumInspectionBytes)
    {
        if (error != nullptr)
        {
            *error = Format(
                L"Could not inspect %s: invalid or inaccessible file.",
                fileLabel);
        }
        return false;
    }

    std::ifstream input(dllPath, std::ios::binary);
    if (!input)
    {
        if (error != nullptr)
        {
            *error = Format(L"Could not inspect %s: the file could not be opened.", fileLabel);
        }
        return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || static_cast<std::size_t>(input.gcount()) != bytes.size())
    {
        if (error != nullptr)
        {
            *error = Format(
                L"Could not inspect %s: the file could not be read completely.",
                fileLabel);
        }
        return false;
    }

    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0
        || !CheckedRange(
            static_cast<std::size_t>(dos->e_lfanew),
            sizeof(IMAGE_NT_HEADERS32),
            bytes.size()))
    {
        if (error != nullptr)
        {
            *error = Format(L"%s is not a valid 32-bit Windows DLL.", fileLabel);
        }
        return false;
    }

    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
        bytes.data() + static_cast<std::size_t>(dos->e_lfanew));
    constexpr std::size_t kRequiredOptionalHeaderSize =
        offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory)
        + (IMAGE_DIRECTORY_ENTRY_EXPORT + 1u) * sizeof(IMAGE_DATA_DIRECTORY);
    if (nt->Signature != IMAGE_NT_SIGNATURE
        || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC
        || nt->FileHeader.SizeOfOptionalHeader < kRequiredOptionalHeaderSize
        || nt->FileHeader.NumberOfSections == 0u
        || nt->FileHeader.NumberOfSections > 96u)
    {
        if (error != nullptr)
        {
            *error = Format(L"%s is not a compatible 32-bit Windows DLL.", fileLabel);
        }
        return false;
    }

    const std::size_t sectionOffset = static_cast<std::size_t>(dos->e_lfanew)
        + offsetof(IMAGE_NT_HEADERS32, OptionalHeader)
        + nt->FileHeader.SizeOfOptionalHeader;
    const std::size_t sectionBytes = static_cast<std::size_t>(nt->FileHeader.NumberOfSections)
        * sizeof(IMAGE_SECTION_HEADER);
    if (!CheckedRange(sectionOffset, sectionBytes, bytes.size()))
    {
        if (error != nullptr)
        {
            *error = Format(L"%s has a truncated section table.", fileLabel);
        }
        return false;
    }
    const auto* const sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        bytes.data() + sectionOffset);

    auto rvaToOffset = [&](const std::uint32_t rva, const std::size_t length, std::size_t* out) -> bool
    {
        if (rva < nt->OptionalHeader.SizeOfHeaders)
        {
            if (!CheckedRange(static_cast<std::size_t>(rva), length, bytes.size()))
            {
                return false;
            }
            *out = static_cast<std::size_t>(rva);
            return true;
        }
        for (std::uint16_t index = 0u; index < nt->FileHeader.NumberOfSections; ++index)
        {
            const IMAGE_SECTION_HEADER& section = sections[index];
            const std::uint32_t span = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
            if (rva < section.VirtualAddress || rva - section.VirtualAddress >= span)
            {
                continue;
            }
            const std::uint32_t delta = rva - section.VirtualAddress;
            if (delta > section.SizeOfRawData || length > section.SizeOfRawData - delta)
            {
                return false;
            }
            const std::size_t offset = static_cast<std::size_t>(section.PointerToRawData) + delta;
            if (!CheckedRange(offset, length, bytes.size()))
            {
                return false;
            }
            *out = offset;
            return true;
        }
        return false;
    };

    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT)
    {
        if (error != nullptr)
        {
            *error = Format(L"%s has no readable export table.", fileLabel);
        }
        return false;
    }
    const IMAGE_DATA_DIRECTORY exports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    std::size_t exportOffset = 0u;
    if (exports.VirtualAddress == 0u
        || !rvaToOffset(exports.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY), &exportOffset))
    {
        if (error != nullptr)
        {
            *error = Format(L"%s has no readable export table.", fileLabel);
        }
        return false;
    }
    const auto* const directory = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
        bytes.data() + exportOffset);
    constexpr std::uint32_t kMaximumExports = 65536u;
    if (directory->NumberOfNames == 0u || directory->NumberOfNames > kMaximumExports
        || directory->NumberOfFunctions == 0u || directory->NumberOfFunctions > kMaximumExports)
    {
        if (error != nullptr)
        {
            *error = Format(L"%s has an invalid export table.", fileLabel);
        }
        return false;
    }

    std::size_t namesOffset = 0u;
    std::size_t ordinalsOffset = 0u;
    std::size_t functionsOffset = 0u;
    if (!rvaToOffset(
            directory->AddressOfNames,
            static_cast<std::size_t>(directory->NumberOfNames) * sizeof(std::uint32_t),
            &namesOffset)
        || !rvaToOffset(
            directory->AddressOfNameOrdinals,
            static_cast<std::size_t>(directory->NumberOfNames) * sizeof(std::uint16_t),
            &ordinalsOffset)
        || !rvaToOffset(
            directory->AddressOfFunctions,
            static_cast<std::size_t>(directory->NumberOfFunctions) * sizeof(std::uint32_t),
            &functionsOffset))
    {
        if (error != nullptr)
        {
            *error = Format(L"%s has a truncated export table.", fileLabel);
        }
        return false;
    }

    const auto* const names = reinterpret_cast<const std::uint32_t*>(bytes.data() + namesOffset);
    const auto* const ordinals = reinterpret_cast<const std::uint16_t*>(bytes.data() + ordinalsOffset);
    const auto* const functions = reinterpret_cast<const std::uint32_t*>(bytes.data() + functionsOffset);
    for (std::uint32_t index = 0u; index < directory->NumberOfNames; ++index)
    {
        std::size_t nameOffset = 0u;
        if (!rvaToOffset(names[index], 1u, &nameOffset))
        {
            continue;
        }
        constexpr std::size_t kMaximumNameLength = 256u;
        const std::size_t available = std::min(kMaximumNameLength, bytes.size() - nameOffset);
        const char* const name = reinterpret_cast<const char*>(bytes.data() + nameOffset);
        const void* const terminator = std::memchr(name, '\0', available);
        if (terminator == nullptr)
        {
            continue;
        }
        bool requested = false;
        for (std::size_t requestedIndex = 0u;
             requestedIndex < requestedNameCount;
             ++requestedIndex)
        {
            requested = requested
                || (requestedNames[requestedIndex] != nullptr
                    && std::strcmp(name, requestedNames[requestedIndex]) == 0);
        }
        if (!requested)
        {
            continue;
        }
        if (!readExportValue)
        {
            return true;
        }
        const std::uint16_t ordinal = ordinals[index];
        if (ordinal >= directory->NumberOfFunctions)
        {
            break;
        }
        std::size_t valueOffset = 0u;
        if (!rvaToOffset(functions[ordinal], sizeof(std::uint32_t), &valueOffset))
        {
            break;
        }
        std::uint32_t value = 0u;
        std::memcpy(&value, bytes.data() + valueOffset, sizeof(value));
        if (exportValue != nullptr)
        {
            *exportValue = value;
        }
        return true;
    }

    if (error != nullptr)
    {
        *error = Format(L"%s is missing a required package export.", fileLabel);
    }
    return false;
}

}

std::wstring Format(const wchar_t* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    const int needed = _vscwprintf(fmt, copy);
    va_end(copy);
    if (needed <= 0)
    {
        va_end(args);
        return fmt ? fmt : L"";
    }

    std::wstring out(static_cast<std::size_t>(needed) + 1, L'\0');
    vswprintf_s(out.data(), out.size(), fmt, args);
    va_end(args);
    out.resize(static_cast<std::size_t>(needed));
    return out;
}

std::wstring Win32Error(DWORD code)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    std::wstring result = length && buffer ? std::wstring(buffer, length) : L"Unknown error";
    if (buffer != nullptr)
    {
        LocalFree(buffer);
    }
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' '))
    {
        result.pop_back();
    }
    return result;
}

std::wstring WsaError(int code)
{
    return Win32Error(static_cast<DWORD>(code));
}

std::filesystem::path ExeDirectory()
{
    std::vector<wchar_t> buffer(512u, L'\0');
    for (;;)
    {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0u)
        {
            std::error_code ec;
            return std::filesystem::current_path(ec);
        }
        if (length < buffer.size() - 1u)
        {
            return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        }
        if (buffer.size() >= 32768u)
        {
            return {};
        }
        buffer.resize(buffer.size() * 2u, L'\0');
    }
}

std::filesystem::path ResolveWritableStateDirectory(const std::filesystem::path& launcherDir)
{
    const auto absolutePath = [](const std::filesystem::path& path) {
        std::error_code ec;
        std::filesystem::path resolved = path;
        if (resolved.is_relative())
        {
            resolved = std::filesystem::absolute(resolved, ec);
            if (ec)
            {
                return std::filesystem::path{};
            }
        }
        return resolved.lexically_normal();
    };

    const std::wstring overridePath = EnvironmentValue(L"TRIAL_NETPLAY_STATE_DIR");
    if (!overridePath.empty())
    {
        const std::filesystem::path requested = absolutePath(overridePath);
        if (EnsureWritableDirectory(requested))
        {
            return requested;
        }
    }

    const std::filesystem::path absoluteLauncherDir = absolutePath(launcherDir);
    if (EnsureWritableDirectory(absoluteLauncherDir))
    {
        return absoluteLauncherDir;
    }

    const wchar_t* const environmentCandidates[] = {L"LOCALAPPDATA", L"APPDATA", L"TEMP"};
    for (const wchar_t* const variable : environmentCandidates)
    {
        const std::wstring root = EnvironmentValue(variable);
        if (root.empty())
        {
            continue;
        }
        const std::filesystem::path candidate = absolutePath(
            std::filesystem::path(root) / L"EFZ FF Netplay");
        if (EnsureWritableDirectory(candidate))
        {
            return candidate;
        }
    }
    return absoluteLauncherDir.empty() ? launcherDir : absoluteLauncherDir;
}

std::filesystem::path ResolvePath(const std::filesystem::path& base, const std::wstring& userPath)
{
    if (userPath.empty())
    {
        return {};
    }
    std::filesystem::path p(userPath);
    if (p.is_absolute())
    {
        return p;
    }
    return base / p;
}

std::filesystem::path ResolveAssetPath(
    const std::filesystem::path& gameDir,
    const std::filesystem::path& casterDir,
    const std::wstring& userPath,
    const wchar_t* fallbackName)
{
    if (!userPath.empty())
    {
        const std::filesystem::path configured(userPath);
        if (configured.is_absolute())
        {
            if (FileExists(configured))
            {
                SessionLog::Step(L"paths", L"ResolveAssetPath hit absolute %s", configured.wstring().c_str());
                return configured;
            }
        }
        else
        {
            const auto inGameDir = gameDir / configured;
            if (FileExists(inGameDir))
            {
                SessionLog::Step(L"paths", L"ResolveAssetPath hit gameDir %s", inGameDir.wstring().c_str());
                return inGameDir;
            }

            const auto inCasterDir = casterDir / configured;
            if (FileExists(inCasterDir))
            {
                SessionLog::Step(L"paths", L"ResolveAssetPath hit casterDir %s", inCasterDir.wstring().c_str());
                return inCasterDir;
            }
        }
    }

    if (fallbackName != nullptr && fallbackName[0] != L'\0')
    {
        const auto inGameDir = gameDir / fallbackName;
        if (FileExists(inGameDir))
        {
            SessionLog::Step(L"paths", L"ResolveAssetPath fallback gameDir %s", inGameDir.wstring().c_str());
            return inGameDir;
        }

        const auto inCasterDir = casterDir / fallbackName;
        if (FileExists(inCasterDir))
        {
            SessionLog::Step(L"paths", L"ResolveAssetPath fallback casterDir %s", inCasterDir.wstring().c_str());
            return inCasterDir;
        }
    }

    SessionLog::Warn(
        L"paths",
        L"ResolveAssetPath miss userPath=%s fallback=%s",
        userPath.c_str(),
        fallbackName != nullptr ? fallbackName : L"(none)");
    return {};
}

bool FileExists(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

bool CopyIfExists(const std::filesystem::path& src, const std::filesystem::path& dst)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(src, ec))
    {
        return false;
    }

    const auto srcAbs = std::filesystem::weakly_canonical(src, ec);
    const auto dstAbs = std::filesystem::weakly_canonical(dst, ec);
    if (!ec && srcAbs == dstAbs)
    {
        SessionLog::Step(L"paths", L"CopyIfExists skip same file %s", srcAbs.wstring().c_str());
        return true;
    }

    std::filesystem::create_directories(dst.parent_path(), ec);
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        SessionLog::Warn(
            L"paths",
            L"CopyIfExists failed %s -> %s",
            src.wstring().c_str(),
            dst.wstring().c_str());
        return false;
    }
    SessionLog::Step(L"paths", L"CopyIfExists %s -> %s", src.wstring().c_str(), dst.wstring().c_str());
    return true;
}

std::uint32_t ComputeEfzHash(const std::filesystem::path& exePath)
{
    std::ifstream in(exePath, std::ios::binary);
    if (!in)
    {
        return 0;
    }

    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    constexpr std::uint32_t kHashMod = 0x8BC5D801u;
    std::uint32_t hash = 0xFFFFFFFFu;
    std::size_t pos = bytes.size();
    while (pos >= 4)
    {
        pos -= 4;
        const std::uint32_t dword = static_cast<std::uint32_t>(bytes[pos])
            | (static_cast<std::uint32_t>(bytes[pos + 1]) << 8)
            | (static_cast<std::uint32_t>(bytes[pos + 2]) << 16)
            | (static_cast<std::uint32_t>(bytes[pos + 3]) << 24);
        if (dword != 0)
        {
            hash = (RotateLeft32(hash, 5) + dword) ^ kHashMod;
        }
        if (pos == 0)
        {
            break;
        }
    }
    return hash;
}

bool ValidateEfzExecutableLayout(
    const std::filesystem::path& exePath,
    std::wstring* const outError)
{
    if (outError != nullptr)
    {
        outError->clear();
    }
    std::ifstream input(exePath, std::ios::binary);
    IMAGE_DOS_HEADER dos{};
    if (!input.read(reinterpret_cast<char*>(&dos), sizeof(dos))
        || dos.e_magic != IMAGE_DOS_SIGNATURE
        || dos.e_lfanew < static_cast<LONG>(sizeof(dos))
        || dos.e_lfanew > 16 * 1024 * 1024)
    {
        if (outError != nullptr)
        {
            *outError = L"EFZ.EXE is not a valid 32-bit Windows executable.";
        }
        return false;
    }
    input.seekg(dos.e_lfanew, std::ios::beg);
    IMAGE_NT_HEADERS32 nt{};
    if (!input.read(reinterpret_cast<char*>(&nt), sizeof(nt))
        || nt.Signature != IMAGE_NT_SIGNATURE
        || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386
        || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    {
        if (outError != nullptr)
        {
            *outError = L"EFZ.EXE is not the supported 32-bit game build.";
        }
        return false;
    }

    constexpr DWORD kSupportedTimestamp = 0x3AD0ACC2u;
    constexpr DWORD kSupportedImageBase = 0x00400000u;
    constexpr DWORD kSupportedImageSize = 0x008D2000u;
    constexpr DWORD kSupportedEntryPointRva = 0x00065570u;
    const bool dynamicBase =
        (nt.OptionalHeader.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) != 0u;
    const bool relocationsStripped =
        (nt.FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED) != 0u;
    const IMAGE_DATA_DIRECTORY& relocations =
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    const bool hasRelocationDirectory =
        relocations.VirtualAddress != 0u || relocations.Size != 0u;
    if (nt.FileHeader.TimeDateStamp != kSupportedTimestamp
        || nt.OptionalHeader.ImageBase != kSupportedImageBase
        || nt.OptionalHeader.SizeOfImage != kSupportedImageSize
        || nt.OptionalHeader.AddressOfEntryPoint != kSupportedEntryPointRva
        || !relocationsStripped
        || hasRelocationDirectory
        || dynamicBase)
    {
        if (outError != nullptr)
        {
            *outError = Format(
                L"This EFZ.EXE build is not supported (timestamp 0x%08X, base 0x%08X, image 0x%08X, entry RVA 0x%08X, relocations %s%s). Use the supported 32-bit EFZ 1.02 executable; data mods that preserve its executable layout are allowed.",
                nt.FileHeader.TimeDateStamp,
                nt.OptionalHeader.ImageBase,
                nt.OptionalHeader.SizeOfImage,
                nt.OptionalHeader.AddressOfEntryPoint,
                relocationsStripped && !hasRelocationDirectory ? L"stripped" : L"present/inconsistent",
                dynamicBase ? L", ASLR enabled" : L"");
        }
        return false;
    }
    return true;
}

bool ReadNetplayDllCompatibility(
    const std::filesystem::path& dllPath,
    std::uint32_t* const outCompatibilityId,
    std::wstring* const outError)
{
    if (outCompatibilityId != nullptr)
    {
        *outCompatibilityId = 0u;
    }
    if (outError != nullptr)
    {
        outError->clear();
    }

    constexpr const char* kCompatibilityExports[] = {
        "TrialNetplayCompatibilityId",
        "_TrialNetplayCompatibilityId",
    };
    return InspectPeNamedExport(
        dllPath,
        kCompatibilityExports,
        std::size(kCompatibilityExports),
        outCompatibilityId,
        true,
        L"netplay.dll",
        outError);
}

bool ValidateEfzDirectDrawWrapper(
    const std::filesystem::path& dllPath,
    std::wstring* const outError)
{
    if (outError != nullptr)
    {
        outError->clear();
    }
    constexpr const char* kWrapperIdentityExports[] = {
        "EFZDDRAW_RegisterOverlayCallback",
        "EFZDX9_RegisterNetplayHudProvider",
        "EFZDX9_RegisterOverlayProvider",
        "EFZDX9_RegisterTrainingMenuBridge",
    };
    if (!InspectPeNamedExport(
            dllPath,
            kWrapperIdentityExports,
            std::size(kWrapperIdentityExports),
            nullptr,
            false,
            L"ddraw.dll",
            outError))
    {
        if (outError != nullptr)
        {
            *outError = L"ddraw.dll is not the packaged EFZ display wrapper. Reinstall or update the complete package.";
        }
        return false;
    }

    // Any alias satisfies InspectPeNamedExport; the limiter handoff is required on its own.
    constexpr const char* kFrameLimitExport[] = {"EFZDDRAW_SetFrameLimit"};
    if (!InspectPeNamedExport(
            dllPath,
            kFrameLimitExport,
            std::size(kFrameLimitExport),
            nullptr,
            false,
            L"ddraw.dll",
            outError))
    {
        if (outError != nullptr)
        {
            *outError = L"ddraw.dll is an older EFZ wrapper without exact 60/64 FPS support. Reinstall or update the complete package.";
        }
        return false;
    }
    return true;
}

std::vector<wchar_t> BuildChildEnvironmentBlock(
    const std::vector<std::pair<std::wstring, std::wstring>>& replacements)
{
    std::vector<std::wstring> entries;
    constexpr const wchar_t* kCasterOnlyPrefix = L"TRIAL_CASTER_ONLY=";
    constexpr const wchar_t* kSessionIniPrefix = L"TRIAL_CASTER_SESSION_INI=";

    auto startsWithInsensitive = [](const std::wstring& entry, const wchar_t* candidate) {
        const std::size_t length = std::wcslen(candidate);
        return entry.size() >= length && _wcsnicmp(entry.c_str(), candidate, length) == 0;
    };

    wchar_t* env = GetEnvironmentStringsW();
    if (env == nullptr)
    {
        return {};
    }
    for (wchar_t* cursor = env; *cursor != L'\0';)
    {
        const std::wstring entry(cursor);
        const bool replaced = std::any_of(
            replacements.begin(),
            replacements.end(),
            [&entry, &startsWithInsensitive](const auto& replacement) {
                const std::wstring prefix = replacement.first + L"=";
                return startsWithInsensitive(entry, prefix.c_str());
            });
        if (!replaced
            && !startsWithInsensitive(entry, kCasterOnlyPrefix)
            && !startsWithInsensitive(entry, kSessionIniPrefix))
        {
            entries.push_back(entry);
        }
        cursor += entry.size() + 1;
    }
    FreeEnvironmentStringsW(env);

    bool hasCasterOnly = false;
    for (const auto& replacement : replacements)
    {
        if (replacement.first.empty() || replacement.first.find(L'=') != std::wstring::npos)
        {
            continue;
        }
        entries.push_back(replacement.first + L"=" + replacement.second);
        hasCasterOnly = hasCasterOnly
            || _wcsicmp(replacement.first.c_str(), L"TRIAL_CASTER_ONLY") == 0;
    }
    if (!hasCasterOnly)
    {
        entries.emplace_back(L"TRIAL_CASTER_ONLY=1");
    }

    // CreateProcess requires a sorted block; preserve inherited vars, including =C:=.
    std::stable_sort(entries.begin(), entries.end(), [](const std::wstring& left, const std::wstring& right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });

    std::size_t totalChars = 1;
    for (const std::wstring& entry : entries)
    {
        totalChars += entry.size() + 1;
    }

    std::vector<wchar_t> block(totalChars, L'\0');
    std::size_t offset = 0;
    for (const std::wstring& entry : entries)
    {
        std::copy(entry.begin(), entry.end(), block.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += entry.size() + 1;
    }
    return block;
}

std::vector<wchar_t> BuildChildEnvironmentBlock(
    const wchar_t* const key,
    const std::wstring& value)
{
    if (key == nullptr || key[0] == L'\0')
    {
        return {};
    }
    return BuildChildEnvironmentBlock({{key, value}});
}

std::wstring EnsureNativeDirectDrawOverride(const std::wstring& existingOverrides)
{
    auto trim = [](std::wstring text) {
        while (!text.empty() && std::iswspace(text.front()))
        {
            text.erase(text.begin());
        }
        while (!text.empty() && std::iswspace(text.back()))
        {
            text.pop_back();
        }
        return text;
    };
    auto isDirectDraw = [&trim](std::wstring module) {
        module = trim(std::move(module));
        std::transform(module.begin(), module.end(), module.begin(), [](const wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        if (!module.empty() && module.front() == L'*')
        {
            module.erase(module.begin());
        }
        if (module.size() > 4u && module.substr(module.size() - 4u) == L".dll")
        {
            module.resize(module.size() - 4u);
        }
        return module == L"ddraw";
    };

    std::vector<std::wstring> preserved;
    std::size_t cursor = 0u;
    while (cursor <= existingOverrides.size())
    {
        const std::size_t separator = existingOverrides.find(L';', cursor);
        const std::wstring raw = existingOverrides.substr(
            cursor,
            separator == std::wstring::npos ? std::wstring::npos : separator - cursor);
        const std::wstring entry = trim(raw);
        if (!entry.empty())
        {
            const std::size_t equals = entry.find(L'=');
            if (equals == std::wstring::npos)
            {
                preserved.push_back(entry);
            }
            else
            {
                std::vector<std::wstring> modules;
                std::size_t moduleCursor = 0u;
                const std::wstring left = entry.substr(0u, equals);
                while (moduleCursor <= left.size())
                {
                    const std::size_t comma = left.find(L',', moduleCursor);
                    std::wstring module = trim(left.substr(
                        moduleCursor,
                        comma == std::wstring::npos ? std::wstring::npos : comma - moduleCursor));
                    if (!module.empty() && !isDirectDraw(module))
                    {
                        modules.push_back(std::move(module));
                    }
                    if (comma == std::wstring::npos)
                    {
                        break;
                    }
                    moduleCursor = comma + 1u;
                }
                if (!modules.empty())
                {
                    std::wstring rebuilt;
                    for (const std::wstring& module : modules)
                    {
                        if (!rebuilt.empty())
                        {
                            rebuilt += L',';
                        }
                        rebuilt += module;
                    }
                    rebuilt += L"=" + trim(entry.substr(equals + 1u));
                    preserved.push_back(std::move(rebuilt));
                }
            }
        }
        if (separator == std::wstring::npos)
        {
            break;
        }
        cursor = separator + 1u;
    }

    preserved.push_back(L"ddraw=n,b");
    std::wstring result;
    for (const std::wstring& entry : preserved)
    {
        if (!result.empty())
        {
            result += L';';
        }
        result += entry;
    }
    return result;
}

std::wstring TrimConnectIp(std::wstring ip)
{
    while (!ip.empty() && (ip.front() == L' ' || ip.front() == L'\t'))
    {
        ip.erase(ip.begin());
    }
    while (!ip.empty() && (ip.back() == L' ' || ip.back() == L'\t'))
    {
        ip.pop_back();
    }
    return ip;
}

std::wstring NormalizeConnectIpv4(const std::wstring& ip)
{
    const std::wstring trimmed = TrimConnectIp(ip);
    if (trimmed.empty())
    {
        return {};
    }

    IN_ADDR addr{};
    if (InetPtonW(AF_INET, trimmed.c_str(), &addr) != 1)
    {
        return {};
    }

    wchar_t out[16] = {};
    swprintf_s(out, L"%u.%u.%u.%u", addr.S_un.S_un_b.s_b1, addr.S_un.S_un_b.s_b2, addr.S_un.S_un_b.s_b3, addr.S_un.S_un_b.s_b4);
    return out;
}

std::wstring NormalizeConnectHost(const std::wstring& hostText)
{
    std::wstring host = TrimConnectIp(hostText);
    if (host.empty())
    {
        return {};
    }

    const std::wstring ipv4 = NormalizeConnectIpv4(host);
    if (!ipv4.empty())
    {
        return ipv4;
    }

    // Do not pass malformed dotted forms such as 999.1.1.1 to DNS, which may accept them.
    if (std::all_of(host.begin(), host.end(), [](const wchar_t ch) {
            return (ch >= L'0' && ch <= L'9') || ch == L'.';
        }))
    {
        return {};
    }

    if (host.back() == L'.')
    {
        host.pop_back();
    }
    if (host.empty() || host.size() > 253u)
    {
        return {};
    }

    std::size_t labelStart = 0u;
    for (std::size_t i = 0u; i <= host.size(); ++i)
    {
        if (i != host.size() && host[i] != L'.')
        {
            const wchar_t ch = host[i];
            const bool valid = (ch >= L'a' && ch <= L'z')
                || (ch >= L'A' && ch <= L'Z')
                || (ch >= L'0' && ch <= L'9')
                || ch == L'-';
            if (!valid)
            {
                return {};
            }
            host[i] = static_cast<wchar_t>(std::towlower(ch));
            continue;
        }

        const std::size_t labelLength = i - labelStart;
        if (labelLength == 0u || labelLength > 63u
            || host[labelStart] == L'-' || host[i - 1u] == L'-')
        {
            return {};
        }
        labelStart = i + 1u;
    }
    return host;
}

std::wstring ResolveConnectHostIpv4(
    const std::wstring& hostText,
    const std::atomic<bool>* const cancel)
{
    const std::wstring host = NormalizeConnectHost(hostText);
    if (host.empty())
    {
        return {};
    }

    const std::wstring ipv4 = NormalizeConnectIpv4(host);
    if (!ipv4.empty())
    {
        return ipv4;
    }

    if (cancel != nullptr && cancel->load())
    {
        return {};
    }

    return ResolveIpv4WithDeadline(host, cancel);
}

bool IsValidConnectIpv4(const std::wstring& ip)
{
    return !NormalizeConnectIpv4(ip).empty();
}

bool ParseConnectDestination(
    const std::wstring& text,
    const std::uint16_t defaultPort,
    ConnectDestination* const out)
{
    if (out == nullptr || defaultPort == 0u)
    {
        return false;
    }

    const std::wstring trimmed = TrimConnectIp(text);
    if (trimmed.empty())
    {
        return false;
    }

    std::wstring hostText = trimmed;
    std::uint16_t port = defaultPort;
    bool explicitPort = false;

    // Parse brackets so an IPv6 literal is rejected whole, not read as host plus port.
    if (trimmed.front() == L'[' || trimmed.find(L']') != std::wstring::npos)
    {
        return false;
    }
    const std::size_t colon = trimmed.find(L':');
    if (colon != std::wstring::npos)
    {
        if (trimmed.find(L':', colon + 1u) != std::wstring::npos)
        {
            return false;
        }
        hostText = TrimConnectIp(trimmed.substr(0u, colon));
        const std::wstring portText = TrimConnectIp(trimmed.substr(colon + 1u));
        if (hostText.empty() || portText.empty()
            || !std::all_of(portText.begin(), portText.end(), [](const wchar_t ch) {
                return ch >= L'0' && ch <= L'9';
            }))
        {
            return false;
        }
        unsigned long parsedPort = 0u;
        try
        {
            parsedPort = std::stoul(portText);
        }
        catch (...)
        {
            return false;
        }
        if (parsedPort == 0u || parsedPort > 65535u)
        {
            return false;
        }
        port = static_cast<std::uint16_t>(parsedPort);
        explicitPort = true;
    }

    const std::wstring host = NormalizeConnectHost(hostText);
    if (host.empty())
    {
        return false;
    }
    out->host = host;
    out->port = port;
    out->explicitPort = explicitPort;
    return true;
}

std::wstring FormatConnectDestination(const ConnectDestination& destination)
{
    if (destination.host.empty() || destination.port == 0u)
    {
        return {};
    }
    if (!destination.explicitPort)
    {
        return destination.host;
    }
    return destination.host + L":" + std::to_wstring(destination.port);
}

std::wstring NormalizeConnectDestination(
    const std::wstring& text,
    const std::uint16_t defaultPort)
{
    ConnectDestination destination{};
    return ParseConnectDestination(text, defaultPort, &destination)
        ? FormatConnectDestination(destination) : std::wstring{};
}

}
