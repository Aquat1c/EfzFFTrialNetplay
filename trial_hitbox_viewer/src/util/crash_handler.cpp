#include "crash_handler.h"

#include "debug_log.h"
#include "lifecycle_log.h"
#include "state_paths.h"

#include <DbgHelp.h>
#include <TlHelp32.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>

namespace thv::crash
{
namespace
{

std::mutex g_mutex;
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
std::terminate_handler g_previousTerminate = nullptr;
HMODULE g_dbgHelpModule = nullptr;
std::filesystem::path g_stateDirectory;
char g_component[64] = "netplay";
FlushLogsFn g_flushLogs = nullptr;
SessionLogPathFn g_sessionLogPathFn = nullptr;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_dumpWritten{false};
PVOID g_vectoredHandle = nullptr;
HMODULE g_hostModule = nullptr;
HMODULE g_selfModule = nullptr;
wchar_t g_crashModulePath[32768] = {};

bool IsAddressInImage(const HMODULE module, const void* const address)
{
    if (module == nullptr || address == nullptr)
    {
        return false;
    }
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
    {
        return false;
    }
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const BYTE*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
    {
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    return value >= base && value - base < nt->OptionalHeader.SizeOfImage;
}
// Arm only after this delay: the game/CRT throws and catches routinely during init.
std::atomic<DWORD> g_installTickMs{0};
constexpr DWORD kCppCaptureArmDelayMs = 5000u;

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(
    HANDLE,
    DWORD,
    HANDLE,
    MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION,
    PMINIDUMP_USER_STREAM_INFORMATION,
    PMINIDUMP_CALLBACK_INFORMATION);

MiniDumpWriteDumpFn g_miniDumpWriteDump = nullptr;

bool ResolveMiniDumpWriteDump()
{
    if (g_miniDumpWriteDump != nullptr)
    {
        return true;
    }

    const char* const candidates[] = {"dbghelp.dll", "dbgcore.dll"};
    for (const char* candidate : candidates)
    {
        HMODULE module = LoadLibraryA(candidate);
        if (module == nullptr)
        {
            continue;
        }

        auto* const fn = reinterpret_cast<MiniDumpWriteDumpFn>(
            GetProcAddress(module, "MiniDumpWriteDump"));
        if (fn != nullptr)
        {
            g_dbgHelpModule = module;
            g_miniDumpWriteDump = fn;
            return true;
        }

        FreeLibrary(module);
    }

    return false;
}

bool IsFatalExceptionCode(const DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_FLT_DENORMAL_OPERAND:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_INVALID_DISPOSITION:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case 0xC0000374: // STATUS_HEAP_CORRUPTION
    case 0xC0000417: // STATUS_INVALID_CRUNTIME_PARAMETER
    case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN
    case 0xC000060B: // STATUS_FAIL_FAST_EXCEPTION
        return true;
    default:
        return false;
    }
}

std::string PathForLog(const std::filesystem::path& path)
{
    const std::wstring wide = path.wstring();
    if (wide.empty())
    {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.c_str(),
        -1,
        result.data(),
        required,
        nullptr,
        nullptr);
    result.resize(static_cast<std::size_t>(required - 1));
    return result;
}

void ResolveStateDirectory()
{
    g_stateDirectory = paths::MutableStateRoot();
    if (g_stateDirectory.empty())
    {
        g_stateDirectory = L".";
    }
}

bool EnsureCrashDirectory(std::filesystem::path* outDirectory)
{
    if (outDirectory == nullptr)
    {
        return false;
    }

    *outDirectory = g_stateDirectory / L"logs" / L"crashes";
    std::error_code error;
    std::filesystem::create_directories(*outDirectory, error);
    return !error;
}

std::filesystem::path BuildArtifactPath(const wchar_t* extension)
{
    SYSTEMTIME st = {};
    GetLocalTime(&st);

    wchar_t component[64] = {};
    MultiByteToWideChar(
        CP_ACP, 0, g_component, -1, component, static_cast<int>(std::size(component)));
    wchar_t fileName[256] = {};
    std::swprintf(
        fileName,
        std::size(fileName),
        L"trial_%ls_%04u%02u%02u_%02u%02u%02u_pid%lu_tid%lu.%ls",
        component,
        static_cast<unsigned>(st.wYear),
        static_cast<unsigned>(st.wMonth),
        static_cast<unsigned>(st.wDay),
        static_cast<unsigned>(st.wHour),
        static_cast<unsigned>(st.wMinute),
        static_cast<unsigned>(st.wSecond),
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetCurrentThreadId()),
        extension);

    std::filesystem::path directory;
    if (!EnsureCrashDirectory(&directory))
    {
        directory = g_stateDirectory;
    }

    return directory / fileName;
}

const char* ExceptionCodeName(const DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:
        return "IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW:
        return "STACK_OVERFLOW";
    case 0xC0000374:
        return "HEAP_CORRUPTION";
    case 0xC0000417:
        return "INVALID_CRUNTIME_PARAMETER";
    case 0xC0000409:
        return "STACK_BUFFER_OVERRUN";
    case 0xC000060B:
        return "FAIL_FAST";
    default:
        return "UNKNOWN";
    }
}

void LogFatalExceptionToSession(EXCEPTION_POINTERS* pointers, const char* source)
{
    if (pointers == nullptr || pointers->ExceptionRecord == nullptr)
    {
        log::Critical("crash", "%s: null exception pointers", source != nullptr ? source : "?");
        return;
    }

    EXCEPTION_RECORD* record = pointers->ExceptionRecord;
    const DWORD code = record->ExceptionCode;
    const void* address = record->ExceptionAddress;

    log::Critical(
        "crash",
        "%s: code=0x%08lX (%s) at=0x%p tid=%lu",
        source != nullptr ? source : "fatal",
        static_cast<unsigned long>(code),
        ExceptionCodeName(code),
        address,
        static_cast<unsigned long>(GetCurrentThreadId()));

    if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
    {
        const ULONG_PTR operation = record->ExceptionInformation[0];
        const ULONG_PTR accessAddress = record->ExceptionInformation[1];
        log::Critical(
            "crash",
            "AV op=%s target=0x%p",
            operation == 0 ? "read" : operation == 1 ? "write" : operation == 8 ? "DEP" : "other",
            reinterpret_cast<void*>(accessAddress));
    }

    char crashNote[256] = {};
    std::snprintf(
        crashNote,
        sizeof(crashNote),
        "%s at=0x%p tid=%lu",
        ExceptionCodeName(code),
        address,
        static_cast<unsigned long>(GetCurrentThreadId()));
    log::Event(log::EventClass::Crash, source != nullptr ? source : "fatal", "%s", crashNote);

    if (code == 0xC0000374)
    {
        log::Critical(
            "crash",
            "HEAP_CORRUPTION — see memwrite.log watch/diff lines (before/after VA values) and rollback reloads in this report");
    }

    log::FlushFile();
}

// SEH-guarded, so no object with a destructor may live here.
bool ReadCppExceptionTypeName(const EXCEPTION_RECORD* record, char* out, std::size_t outSize)
{
    if (out == nullptr || outSize == 0)
    {
        return false;
    }
    out[0] = '\0';
    __try
    {
        if (record->NumberParameters < 3)
        {
            return false;
        }
        const DWORD* info = reinterpret_cast<const DWORD*>(record->ExceptionInformation);
        const BYTE* throwInfo = reinterpret_cast<const BYTE*>(info[2]);
        if (throwInfo == nullptr)
        {
            return false;
        }
        const DWORD ctaPtr = *reinterpret_cast<const DWORD*>(throwInfo + 12);
        const BYTE* cta = reinterpret_cast<const BYTE*>(ctaPtr);
        if (cta == nullptr)
        {
            return false;
        }
        const DWORD ctPtr = *reinterpret_cast<const DWORD*>(cta + 4);
        const BYTE* ct = reinterpret_cast<const BYTE*>(ctPtr);
        if (ct == nullptr)
        {
            return false;
        }
        const DWORD tdPtr = *reinterpret_cast<const DWORD*>(ct + 4);
        const char* name = reinterpret_cast<const char*>(tdPtr + 8);
        if (name == nullptr)
        {
            return false;
        }
        std::size_t i = 0;
        for (; i + 1 < outSize && name[i] != '\0'; ++i)
        {
            out[i] = name[i];
        }
        out[i] = '\0';
        return i > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        std::snprintf(out, outSize, "<unreadable>");
        return false;
    }
}

void LogFirstChanceCppException(EXCEPTION_POINTERS* pointers)
{
    const DWORD installedAt = g_installTickMs.load(std::memory_order_relaxed);
    if (installedAt == 0u || (GetTickCount() - installedAt) < kCppCaptureArmDelayMs)
    {
        return;
    }

    static std::atomic<int> s_count{0};
    const int n = s_count.fetch_add(1, std::memory_order_relaxed);
    if (n >= 96)
    {
        return;
    }

    char typeName[160] = {};
    ReadCppExceptionTypeName(pointers->ExceptionRecord, typeName, sizeof(typeName));

    auto moduleRange = [](HMODULE mod, uintptr_t& base, uintptr_t& end)
    {
        base = reinterpret_cast<uintptr_t>(mod);
        end = base;
        if (mod == nullptr)
        {
            return;
        }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const BYTE*>(mod) + dos->e_lfanew);
        end = base + nt->OptionalHeader.SizeOfImage;
    };

    uintptr_t efzBase = 0, efzEnd = 0, modBase = 0, modEnd = 0;
    moduleRange(GetModuleHandleA(nullptr), efzBase, efzEnd);

    HMODULE self = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&LogFatalExceptionToSession),
        &self);
    moduleRange(self, modBase, modEnd);

    void* frames[24] = {};
    const USHORT captured = RtlCaptureStackBackTrace(0, 24, frames, nullptr);

    char stackLine[640] = {};
    std::size_t used = 0;
    int shown = 0;
    for (USHORT f = 0; f < captured && shown < 10; ++f)
    {
        const uintptr_t addr = reinterpret_cast<uintptr_t>(frames[f]);
        const char* tag = nullptr;
        uintptr_t off = 0;
        if (addr >= efzBase && addr < efzEnd)
        {
            tag = "EFZ";
            off = addr - efzBase;
        }
        else if (modBase != 0 && addr >= modBase && addr < modEnd)
        {
            tag = "mod";
            off = addr - modBase;
        }
        if (tag == nullptr)
        {
            continue;
        }
        const int written = std::snprintf(
            stackLine + used,
            sizeof(stackLine) - used,
            "%s%s+0x%06lX",
            shown == 0 ? "" : " <- ",
            tag,
            static_cast<unsigned long>(off));
        if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(stackLine) - used)
        {
            break;
        }
        used += static_cast<std::size_t>(written);
        ++shown;
    }

    log::Critical(
        "crash",
        "C++ throw #%d type=%s tid=%lu game-stack: %s",
        n,
        typeName[0] != '\0' ? typeName : "?",
        static_cast<unsigned long>(GetCurrentThreadId()),
        shown > 0 ? stackLine : "(no EFZ frames)");
    log::FlushFile();
}

LONG CALLBACK VectoredExceptionThunk(EXCEPTION_POINTERS* pointers)
{
    if (pointers != nullptr && pointers->ExceptionRecord != nullptr)
    {
        const DWORD code = pointers->ExceptionRecord->ExceptionCode;
        if (IsFatalExceptionCode(code)
            && (IsAddressInImage(g_hostModule, pointers->ExceptionRecord->ExceptionAddress)
                || IsAddressInImage(
                    g_selfModule, pointers->ExceptionRecord->ExceptionAddress)))
        {
            // Wine uses handled first-chance AVs internally; only log faults in EFZ or this DLL.
            LogFatalExceptionToSession(pointers, "VEH first-chance");
        }
        else if (code == 0xE06D7363u)
        {
            LogFirstChanceCppException(pointers);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void WriteAccessViolationDetails(FILE* file, EXCEPTION_RECORD* record)
{
    if (file == nullptr || record == nullptr || record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    {
        return;
    }

    if (record->NumberParameters < 2)
    {
        return;
    }

    const ULONG_PTR operation = record->ExceptionInformation[0];
    const ULONG_PTR address = record->ExceptionInformation[1];
    std::fprintf(
        file,
        "access_operation=%s\n",
        operation == 0 ? "read" : operation == 1 ? "write" : operation == 8 ? "DEP" : "other");
    std::fprintf(file, "access_address=0x%p\n", reinterpret_cast<void*>(address));
}

struct LoadedModule
{
    std::uintptr_t base = 0;
    std::uintptr_t size = 0;
    char name[64] = {};
};

int BuildModuleTable(LoadedModule* mods, int maxMods)
{
    int count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    MODULEENTRY32 me = {};
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me))
    {
        do
        {
            if (count >= maxMods)
            {
                break;
            }
            mods[count].base = reinterpret_cast<std::uintptr_t>(me.modBaseAddr);
            mods[count].size = me.modBaseSize;
#if defined(UNICODE)
            WideCharToMultiByte(
                CP_UTF8,
                0,
                me.szModule,
                -1,
                mods[count].name,
                static_cast<int>(sizeof(mods[count].name)),
                nullptr,
                nullptr);
#else
            std::snprintf(mods[count].name, sizeof(mods[count].name), "%s", me.szModule);
#endif
            ++count;
        } while (Module32Next(snap, &me));
    }

    CloseHandle(snap);
    return count;
}

const LoadedModule* FindModule(const LoadedModule* mods, int count, std::uintptr_t addr)
{
    for (int i = 0; i < count; ++i)
    {
        if (addr >= mods[i].base && addr < mods[i].base + mods[i].size)
        {
            return &mods[i];
        }
    }
    return nullptr;
}

bool RegionReadable(const void* p, std::size_t n)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
    {
        return false;
    }
    if (mbi.State != MEM_COMMIT)
    {
        return false;
    }
    if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0u)
    {
        return false;
    }
    const auto end = reinterpret_cast<std::uintptr_t>(p) + n;
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return end <= regionEnd;
}

#if defined(_M_IX86)
void WriteCrashWalk(FILE* file, CONTEXT* context)
{
    if (file == nullptr || context == nullptr)
    {
        return;
    }

    LoadedModule mods[256];
    const int modCount = BuildModuleTable(mods, 256);

    std::fprintf(file, "\n--- modules ---\n");
    for (int i = 0; i < modCount; ++i)
    {
        std::fprintf(
            file,
            "  %08lX-%08lX %s\n",
            static_cast<unsigned long>(mods[i].base),
            static_cast<unsigned long>(mods[i].base + mods[i].size),
            mods[i].name);
    }
    std::fprintf(file, "--- end modules ---\n");

    auto symLine = [&](const char* label, std::uintptr_t addr)
    {
        const LoadedModule* m = FindModule(mods, modCount, addr);
        if (m != nullptr)
        {
            std::fprintf(
                file,
                "%s=0x%08lX  %s+0x%lX\n",
                label,
                static_cast<unsigned long>(addr),
                m->name,
                static_cast<unsigned long>(addr - m->base));
        }
        else
        {
            std::fprintf(file, "%s=0x%08lX  (unmapped)\n", label, static_cast<unsigned long>(addr));
        }
    };

    std::fprintf(file, "\n--- symbolized ---\n");
    symLine("EIP", context->Eip);
    symLine("ESI", context->Esi);
    symLine("EBX", context->Ebx);
    symLine("EBP", context->Ebp);

    const auto eip = static_cast<std::uintptr_t>(context->Eip);
    if (RegionReadable(reinterpret_cast<const void*>(eip - 16), 32))
    {
        const auto* p = reinterpret_cast<const unsigned char*>(eip - 16);
        std::fprintf(file, "fault_bytes[EIP-16..+15]=");
        for (int i = 0; i < 32; ++i)
        {
            std::fprintf(file, "%s%02X", i == 16 ? "|" : " ", p[i]);
        }
        std::fprintf(file, "\n");
    }
    else
    {
        std::fprintf(file, "fault_bytes=(EIP unmapped — wild jump/call target)\n");
    }

    std::fprintf(file, "\n--- stack_code_addrs (ESP up, module hits) ---\n");
    const auto esp = static_cast<std::uintptr_t>(context->Esp);
    int printed = 0;
    for (std::uintptr_t a = esp; a < esp + 0x800 && printed < 64; a += 4)
    {
        if (!RegionReadable(reinterpret_cast<const void*>(a), 4))
        {
            break;
        }
        const auto val = *reinterpret_cast<const std::uintptr_t*>(a);
        const LoadedModule* m = FindModule(mods, modCount, val);
        if (m != nullptr)
        {
            std::fprintf(
                file,
                "  [%08lX] = %08lX  %s+0x%lX\n",
                static_cast<unsigned long>(a),
                static_cast<unsigned long>(val),
                m->name,
                static_cast<unsigned long>(val - m->base));
            ++printed;
        }
    }
    std::fprintf(file, "--- end stack_code_addrs ---\n");
}
#endif

void WriteCrashTextLog(
    EXCEPTION_POINTERS* pointers,
    const std::filesystem::path& dumpPath,
    bool dumpOk)
{
    const std::filesystem::path txtPath = BuildArtifactPath(L"txt");
    FILE* file = nullptr;
    if (_wfopen_s(&file, txtPath.c_str(), L"w") != 0 || file == nullptr)
    {
        return;
    }

    EXCEPTION_RECORD* record = pointers != nullptr ? pointers->ExceptionRecord : nullptr;
    CONTEXT* context = pointers != nullptr ? pointers->ContextRecord : nullptr;
    const DWORD code = record != nullptr ? record->ExceptionCode : 0;
    const void* address = record != nullptr ? record->ExceptionAddress : nullptr;

    std::fprintf(file, "Trial EFZ crash report\n");
    std::fprintf(file, "component=%s\n", g_component);
    std::fprintf(file, "pid=%lu\n", static_cast<unsigned long>(GetCurrentProcessId()));
    std::fprintf(file, "tid=%lu\n", static_cast<unsigned long>(GetCurrentThreadId()));
    std::fprintf(file, "exception_code=0x%08lX\n", static_cast<unsigned long>(code));
    std::fprintf(file, "exception_name=%s\n", ExceptionCodeName(code));
    std::fprintf(file, "exception_address=0x%p\n", address);
    WriteAccessViolationDetails(file, record);

    if (address != nullptr)
    {
        HMODULE crashModule = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(address),
                &crashModule))
        {
            g_crashModulePath[0] = L'\0';
            const DWORD modulePathLength = GetModuleFileNameW(
                crashModule,
                g_crashModulePath,
                static_cast<DWORD>(std::size(g_crashModulePath)));
            const std::string modulePath = modulePathLength > 0u
                && modulePathLength < std::size(g_crashModulePath)
                ? PathForLog(std::filesystem::path(
                    std::wstring(g_crashModulePath, modulePathLength)))
                : std::string("(module path unavailable or too long)");
            const auto moduleBase = reinterpret_cast<std::uintptr_t>(crashModule);
            const auto rva = reinterpret_cast<std::uintptr_t>(address) - moduleBase;
            std::fprintf(file, "crash_module=%s\n", modulePath.c_str());
            std::fprintf(file, "crash_module_base=0x%08lX\n", static_cast<unsigned long>(moduleBase));
            std::fprintf(file, "crash_rva=0x%08lX\n", static_cast<unsigned long>(rva));
        }
    }

    if (g_sessionLogPathFn != nullptr)
    {
        const char* sessionPath = g_sessionLogPathFn();
        if (sessionPath != nullptr && sessionPath[0] != '\0')
        {
            std::fprintf(file, "session_log=%s\n", sessionPath);
        }
    }

    const std::string dumpPathUtf8 = dumpOk ? PathForLog(dumpPath) : std::string{};
    std::fprintf(file, "minidump=%s\n", dumpPathUtf8.c_str());
    std::fprintf(file, "minidump_ok=%d\n", dumpOk ? 1 : 0);

    lifecycle::WriteModStateSnapshot(file);
    log::WriteCrashContext(file);
    log::WriteClassifiedTail(file, log::EventClass::MemWrite, 96);
    log::WriteClassifiedTail(file, log::EventClass::Rollback, 48);
    log::WriteClassifiedTail(file, log::EventClass::Phase, 32);
    log::WriteSessionTail(file, 128);

#if defined(_M_IX86)
    if (context != nullptr)
    {
        std::fprintf(file, "EIP=0x%08lX\n", static_cast<unsigned long>(context->Eip));
        std::fprintf(file, "ESP=0x%08lX\n", static_cast<unsigned long>(context->Esp));
        std::fprintf(file, "EBP=0x%08lX\n", static_cast<unsigned long>(context->Ebp));
        std::fprintf(file, "EAX=0x%08lX\n", static_cast<unsigned long>(context->Eax));
        std::fprintf(file, "EBX=0x%08lX\n", static_cast<unsigned long>(context->Ebx));
        std::fprintf(file, "ECX=0x%08lX\n", static_cast<unsigned long>(context->Ecx));
        std::fprintf(file, "EDX=0x%08lX\n", static_cast<unsigned long>(context->Edx));
        std::fprintf(file, "ESI=0x%08lX\n", static_cast<unsigned long>(context->Esi));
        std::fprintf(file, "EDI=0x%08lX\n", static_cast<unsigned long>(context->Edi));

        WriteCrashWalk(file, context);
    }
#endif

    std::fclose(file);
}

void WriteCrashArtifacts(EXCEPTION_POINTERS* pointers)
{
    if (g_dumpWritten.exchange(true))
    {
        return;
    }

    if (pointers != nullptr)
    {
        LogFatalExceptionToSession(pointers, "unhandled");
    }

    lifecycle::Checkpoint("unhandled_exception");

    if (g_flushLogs != nullptr)
    {
        g_flushLogs();
    }

    const std::filesystem::path dumpPath = BuildArtifactPath(L"dmp");
    HANDLE dumpFile = CreateFileW(
        dumpPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    bool dumpOk = false;
    if (dumpFile != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei = {};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = pointers;
        mei.ClientPointers = FALSE;

        if (ResolveMiniDumpWriteDump())
        {
            const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
                MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo
                | MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory);

            dumpOk = g_miniDumpWriteDump(
                         GetCurrentProcess(),
                         GetCurrentProcessId(),
                         dumpFile,
                         dumpType,
                         pointers != nullptr ? &mei : nullptr,
                         nullptr,
                         nullptr)
                == TRUE;
        }

        CloseHandle(dumpFile);
    }

    WriteCrashTextLog(pointers, dumpPath, dumpOk);

    wchar_t message[512] = {};
    const DWORD code = pointers != nullptr && pointers->ExceptionRecord != nullptr
        ? pointers->ExceptionRecord->ExceptionCode
        : 0;
    const void* address = pointers != nullptr && pointers->ExceptionRecord != nullptr
        ? pointers->ExceptionRecord->ExceptionAddress
        : nullptr;
    std::swprintf(
        message,
        std::size(message),
        L"Trial crash: code=0x%08lX addr=0x%p dump=%d path=%ls\n",
        static_cast<unsigned long>(code),
        address,
        dumpOk ? 1 : 0,
        dumpPath.c_str());
    OutputDebugStringW(message);
}

constexpr DWORD kCodeCppException = 0xE06D7363u;

LONG WINAPI UnhandledExceptionFilterThunk(EXCEPTION_POINTERS* pointers)
{
    if (pointers != nullptr && pointers->ExceptionRecord != nullptr
        && (IsFatalExceptionCode(pointers->ExceptionRecord->ExceptionCode)
            || pointers->ExceptionRecord->ExceptionCode == kCodeCppException))
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        WriteCrashArtifacts(pointers);
    }

    if (g_previousFilter != nullptr)
    {
        return g_previousFilter(pointers);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

void CaptureSyntheticCrash(std::uint32_t code, const char* what)
{
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    RtlCaptureContext(&ctx);

    EXCEPTION_RECORD record = {};
    record.ExceptionCode = code;
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
#if defined(_M_IX86)
    record.ExceptionAddress = reinterpret_cast<void*>(static_cast<std::uintptr_t>(ctx.Eip));
#else
    record.ExceptionAddress = reinterpret_cast<void*>(static_cast<std::uintptr_t>(ctx.Rip));
#endif

    EXCEPTION_POINTERS pointers = {};
    pointers.ExceptionRecord = &record;
    pointers.ContextRecord = &ctx;

    lifecycle::Fault("crash", "C++/CRT fatal captured: %s (code=0x%08X)", what != nullptr ? what : "?", code);
    log::Critical("crash", "C++/CRT fatal captured: %s (code=0x%08X) — writing dump", what != nullptr ? what : "?", code);

    std::lock_guard<std::mutex> lock(g_mutex);
    WriteCrashArtifacts(&pointers);
}

constexpr std::uint32_t kCodeCppTerminate = 0xE0C7'0001u;
constexpr std::uint32_t kCodeInvalidParam = 0xE0C7'0002u;
constexpr std::uint32_t kCodePureCall = 0xE0C7'0003u;

void __cdecl CrashTerminateHandler()
{
    CaptureSyntheticCrash(kCodeCppTerminate, "std::terminate (uncaught C++ exception / bad_alloc)");
    if (g_previousTerminate != nullptr)
    {
        g_previousTerminate();
    }
    std::abort();
}

void __cdecl CrashInvalidParameterHandler(
    const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
{
    CaptureSyntheticCrash(kCodeInvalidParam, "CRT invalid parameter");
}

void __cdecl CrashPureCallHandler()
{
    CaptureSyntheticCrash(kCodePureCall, "pure virtual call");
}

}

bool Install(const HMODULE module, const Options& options)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_installed.exchange(true))
    {
        return true;
    }

    ResolveStateDirectory();
    g_dumpWritten.store(false);
    g_flushLogs = options.flushLogs;
    g_sessionLogPathFn = options.sessionLogPathFn;
    g_hostModule = GetModuleHandleW(nullptr);
    g_selfModule = module;
    if (g_selfModule == nullptr)
    {
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&Install),
            &g_selfModule);
    }

    if (options.component != nullptr && options.component[0] != '\0')
    {
        std::snprintf(g_component, sizeof(g_component), "%s", options.component);
    }
    else
    {
        std::snprintf(g_component, sizeof(g_component), "netplay");
    }

    g_installTickMs.store(GetTickCount(), std::memory_order_relaxed);
    g_previousFilter = SetUnhandledExceptionFilter(UnhandledExceptionFilterThunk);
    g_vectoredHandle = AddVectoredExceptionHandler(1, VectoredExceptionThunk);
    g_previousTerminate = std::set_terminate(&CrashTerminateHandler);
    _set_invalid_parameter_handler(&CrashInvalidParameterHandler);
    _set_purecall_handler(&CrashPureCallHandler);
    const std::string statePath = PathForLog(g_stateDirectory);
    lifecycle::Init(
        "crash",
        "handlers installed VEH+unhandled+terminate component=%s dir=%s/logs/crashes",
        g_component,
        statePath.c_str());
    log::Step(
        "crash",
        "handlers installed (VEH + unhandled + C++ terminate/invalid-param) component=%s dir=%s/logs/crashes source=%s",
        g_component,
        statePath.c_str(),
        paths::MutableStateRootSource());
    return true;
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_installed.exchange(false))
    {
        return;
    }

    if (g_vectoredHandle != nullptr)
    {
        RemoveVectoredExceptionHandler(g_vectoredHandle);
        g_vectoredHandle = nullptr;
    }

    std::set_terminate(g_previousTerminate);
    SetUnhandledExceptionFilter(g_previousFilter);
    g_previousFilter = nullptr;
    g_flushLogs = nullptr;
    g_sessionLogPathFn = nullptr;
    g_hostModule = nullptr;
    g_selfModule = nullptr;
    log::Step("crash", "handlers removed");

    if (g_dbgHelpModule != nullptr)
    {
        FreeLibrary(g_dbgHelpModule);
        g_dbgHelpModule = nullptr;
        g_miniDumpWriteDump = nullptr;
    }
}

void WriteManualDump(const char* reason, unsigned int detailValue)
{
    static std::atomic<int> s_count{0};
    if (s_count.fetch_add(1, std::memory_order_relaxed) >= 3)
    {
        return;
    }
    char what[192] = {};
    std::snprintf(
        what,
        sizeof(what),
        "%s (detail=0x%08X / %u)",
        reason != nullptr ? reason : "manual dump",
        detailValue,
        detailValue);
    CaptureSyntheticCrash(0xE0C70009u, what);
    // Must not consume the one-shot crash-artifact slot.
    g_dumpWritten.store(false);
}

}
