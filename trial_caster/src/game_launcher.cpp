#include "game_launcher.h"

#include "compatibility_runtime.h"
#include "session_log.h"
#include "util.h"

#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace trial_caster
{
namespace
{

constexpr DWORD kEntryPointTimeoutMs = 30000u;
constexpr DWORD kInjectionTimeoutMs = 30000u;

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

bool ResolveMainEntryPoint(HANDLE process, DWORD* const entryPoint, std::wstring* const error)
{
    static_assert(sizeof(void*) == 4u, "EFZ launcher must be built for x86");
    using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(
        HANDLE,
        PROCESSINFOCLASS,
        PVOID,
        ULONG,
        PULONG);
    HMODULE const ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto queryProcess = ntdll != nullptr
        ? reinterpret_cast<NtQueryInformationProcessFn>(
            GetProcAddress(ntdll, "NtQueryInformationProcess"))
        : nullptr;
    if (queryProcess == nullptr)
    {
        if (error != nullptr)
        {
            *error = L"Could not inspect the new game process (NtQueryInformationProcess unavailable).";
        }
        return false;
    }

    PROCESS_BASIC_INFORMATION basic{};
    const NTSTATUS status = queryProcess(
        process,
        ProcessBasicInformation,
        &basic,
        sizeof(basic),
        nullptr);
    if (status < 0 || basic.PebBaseAddress == nullptr)
    {
        if (error != nullptr)
        {
            *error = util::Format(L"Could not inspect the new game process (NTSTATUS 0x%08X).", status);
        }
        return false;
    }

    // In a 32-bit PEB, ImageBaseAddress follows the four flag bytes and the Mutant pointer.
    DWORD imageBase = 0u;
    const auto imageBaseField = reinterpret_cast<const std::uint8_t*>(basic.PebBaseAddress)
        + 4u + sizeof(void*);
    SIZE_T read = 0u;
    if (!ReadProcessMemory(
            process,
            imageBaseField,
            &imageBase,
            sizeof(imageBase),
            &read)
        || read != sizeof(imageBase)
        || imageBase == 0u)
    {
        if (error != nullptr)
        {
            *error = util::Format(
                L"Could not read the new game image base: %s",
                util::Win32Error(GetLastError()).c_str());
        }
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS32 nt{};
    if (!ReadProcessMemory(
            process,
            reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(imageBase)),
            &dos,
            sizeof(dos),
            &read)
        || read != sizeof(dos)
        || dos.e_magic != IMAGE_DOS_SIGNATURE
        || dos.e_lfanew < 0
        || !ReadProcessMemory(
            process,
            reinterpret_cast<LPCVOID>(
                static_cast<std::uintptr_t>(imageBase) + static_cast<std::uint32_t>(dos.e_lfanew)),
            &nt,
            sizeof(nt),
            &read)
        || read != sizeof(nt)
        || nt.Signature != IMAGE_NT_SIGNATURE
        || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386
        || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    {
        if (error != nullptr)
        {
            *error = L"EFZ.EXE is not a readable 32-bit Windows executable.";
        }
        return false;
    }

    const std::uint64_t resolved = static_cast<std::uint64_t>(imageBase)
        + nt.OptionalHeader.AddressOfEntryPoint;
    if (resolved > std::numeric_limits<DWORD>::max())
    {
        if (error != nullptr)
        {
            *error = L"EFZ.EXE has an invalid entry point.";
        }
        return false;
    }
    *entryPoint = static_cast<DWORD>(resolved);
    return true;
}

bool FindRemoteModule(
    const DWORD processId,
    const std::wstring& moduleName,
    std::uintptr_t* const remoteBase,
    DWORD* const remoteSize,
    std::wstring* const error)
{
    constexpr unsigned kSnapshotAttempts = 20u;
    for (unsigned attempt = 0u; attempt < kSnapshotAttempts; ++attempt)
    {
        SetLastError(ERROR_SUCCESS);
        HANDLE const snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
            processId);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            const DWORD snapshotError = GetLastError();
            if (snapshotError == ERROR_BAD_LENGTH)
            {
                Sleep(10u);
                continue;
            }
            if (error != nullptr)
            {
                *error = util::Format(
                    L"Could not inspect loaded game modules: %s",
                    util::Win32Error(snapshotError).c_str());
            }
            return false;
        }

        MODULEENTRY32W module{};
        module.dwSize = sizeof(module);
        BOOL hasModule = Module32FirstW(snapshot, &module);
        DWORD enumerationError = hasModule ? ERROR_SUCCESS : GetLastError();
        while (hasModule)
        {
            if (_wcsicmp(module.szModule, moduleName.c_str()) == 0)
            {
                *remoteBase = reinterpret_cast<std::uintptr_t>(module.modBaseAddr);
                *remoteSize = module.modBaseSize;
                CloseHandle(snapshot);
                return true;
            }
            hasModule = Module32NextW(snapshot, &module);
            enumerationError = hasModule ? ERROR_SUCCESS : GetLastError();
        }
        CloseHandle(snapshot);
        if (enumerationError == ERROR_BAD_LENGTH)
        {
            Sleep(10u);
            continue;
        }
        break;
    }

    if (error != nullptr)
    {
        *error = L"The game process did not expose the system module needed for DLL loading.";
    }
    return false;
}

bool ResolveRemoteLoadLibrary(
    HANDLE process,
    const DWORD processId,
    LPTHREAD_START_ROUTINE* const remoteProcedure,
    std::wstring* const ownerName,
    std::wstring* const error)
{
    FARPROC const localProcedure = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (localProcedure == nullptr)
    {
        if (error != nullptr)
        {
            *error = L"The Windows DLL loader entry point is unavailable.";
        }
        return false;
    }

    HMODULE localOwner = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(localProcedure),
            &localOwner)
        || localOwner == nullptr)
    {
        if (error != nullptr)
        {
            *error = L"Could not identify the system module that owns LoadLibraryW.";
        }
        return false;
    }

    std::vector<wchar_t> ownerPath(512u, L'\0');
    DWORD ownerLength = 0u;
    for (;;)
    {
        SetLastError(ERROR_SUCCESS);
        ownerLength = GetModuleFileNameW(
            localOwner,
            ownerPath.data(),
            static_cast<DWORD>(ownerPath.size()));
        if (ownerLength == 0u)
        {
            break;
        }
        if (ownerLength < ownerPath.size() - 1u)
        {
            break;
        }
        if (ownerPath.size() >= 32768u)
        {
            ownerLength = 0u;
            break;
        }
        ownerPath.resize(
            (std::min<std::size_t>)(ownerPath.size() * 2u, 32768u),
            L'\0');
    }
    if (ownerLength == 0u)
    {
        if (error != nullptr)
        {
            *error = L"Could not identify the DLL loader's system module name.";
        }
        return false;
    }
    const std::wstring moduleName =
        std::filesystem::path(std::wstring(ownerPath.data(), ownerLength)).filename().wstring();

    const std::uintptr_t localBase = reinterpret_cast<std::uintptr_t>(localOwner);
    const std::uintptr_t localAddress = reinterpret_cast<std::uintptr_t>(localProcedure);
    if (localAddress < localBase)
    {
        if (error != nullptr)
        {
            *error = L"The DLL loader address is outside its owning system module.";
        }
        return false;
    }
    const std::uintptr_t procedureRva = localAddress - localBase;

    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(localBase);
    const auto* const nt = dos->e_magic == IMAGE_DOS_SIGNATURE
        ? reinterpret_cast<const IMAGE_NT_HEADERS32*>(localBase + static_cast<std::uint32_t>(dos->e_lfanew))
        : nullptr;
    if (nt == nullptr || nt->Signature != IMAGE_NT_SIGNATURE
        || procedureRva >= nt->OptionalHeader.SizeOfImage)
    {
        if (error != nullptr)
        {
            *error = L"The local DLL loader module has an invalid image layout.";
        }
        return false;
    }

    std::uintptr_t remoteBase = 0u;
    DWORD remoteSize = 0u;
    if (!FindRemoteModule(processId, moduleName, &remoteBase, &remoteSize, error))
    {
        return false;
    }
    if (procedureRva >= remoteSize)
    {
        if (error != nullptr)
        {
            *error = L"The game has a different system DLL layout from the launcher process.";
        }
        return false;
    }

    IMAGE_DOS_HEADER remoteDos{};
    IMAGE_NT_HEADERS32 remoteNt{};
    SIZE_T read = 0u;
    if (!ReadProcessMemory(
            process,
            reinterpret_cast<LPCVOID>(remoteBase),
            &remoteDos,
            sizeof(remoteDos),
            &read)
        || read != sizeof(remoteDos)
        || remoteDos.e_magic != IMAGE_DOS_SIGNATURE
        || remoteDos.e_lfanew < 0
        || !ReadProcessMemory(
            process,
            reinterpret_cast<LPCVOID>(remoteBase + static_cast<std::uint32_t>(remoteDos.e_lfanew)),
            &remoteNt,
            sizeof(remoteNt),
            &read)
        || read != sizeof(remoteNt)
        || remoteNt.Signature != IMAGE_NT_SIGNATURE
        || remoteNt.OptionalHeader.SizeOfImage != nt->OptionalHeader.SizeOfImage
        || remoteNt.FileHeader.TimeDateStamp != nt->FileHeader.TimeDateStamp)
    {
        if (error != nullptr)
        {
            *error = L"The game and launcher processes loaded different system DLL builds.";
        }
        return false;
    }

    *remoteProcedure = reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteBase + procedureRva);
    if (ownerName != nullptr)
    {
        *ownerName = moduleName;
    }
    return true;
}

bool InjectDll(
    HANDLE process,
    const DWORD processId,
    const std::filesystem::path& dllPath,
    const std::atomic<bool>* const cancel,
    std::wstring* error)
{
    const std::wstring wide = dllPath.wstring();
    const std::size_t byteCount = (wide.size() + 1u) * sizeof(wchar_t);
    void* const remote = VirtualAllocEx(process, nullptr, byteCount, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == nullptr)
    {
        if (error != nullptr)
        {
            *error = util::Format(L"VirtualAllocEx failed: %s", util::Win32Error(GetLastError()).c_str());
        }
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote, wide.c_str(), byteCount, &written)
        || written != byteCount)
    {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        if (error != nullptr)
        {
            *error = util::Format(L"WriteProcessMemory failed: %s", util::Win32Error(GetLastError()).c_str());
        }
        return false;
    }

    LPTHREAD_START_ROUTINE loadLibrary = nullptr;
    std::wstring loaderOwner;
    if (!ResolveRemoteLoadLibrary(
            process,
            processId,
            &loadLibrary,
            &loaderOwner,
            error))
    {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (thread == nullptr)
    {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        if (error != nullptr)
        {
            *error = util::Format(L"CreateRemoteThread failed: %s", util::Win32Error(GetLastError()).c_str());
        }
        return false;
    }

    const ULONGLONG deadline = GetTickCount64() + kInjectionTimeoutMs;
    DWORD waitResult = WAIT_TIMEOUT;
    while (GetTickCount64() < deadline)
    {
        if (cancel != nullptr && cancel->load())
        {
            if (error != nullptr)
            {
                *error = L"Game launch cancelled.";
            }
            CloseHandle(thread);
            return false;
        }
        waitResult = WaitForSingleObject(thread, 50u);
        if (waitResult != WAIT_TIMEOUT)
        {
            break;
        }
    }
    if (waitResult != WAIT_OBJECT_0)
    {
        const DWORD waitError = waitResult == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
        CloseHandle(thread);
        // The remote thread may still own the path buffer; terminating the child releases it.
        if (error != nullptr)
        {
            *error = util::Format(
                L"The game did not finish loading %s within %u seconds: %s",
                dllPath.filename().wstring().c_str(),
                kInjectionTimeoutMs / 1000u,
                util::Win32Error(waitError).c_str());
        }
        return false;
    }
    DWORD exitCode = 0;
    const BOOL readExitCode = GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);

    if (!readExitCode || exitCode == 0)
    {
        if (error != nullptr)
        {
            *error = util::Format(
                L"LoadLibrary failed for %s (check SDL3.dll beside the DLL and %s)",
                dllPath.wstring().c_str(),
                dllPath.parent_path().wstring().c_str());
        }
        SessionLog::Error(L"launch", L"InjectDll LoadLibraryW returned 0 for %s", dllPath.wstring().c_str());
        return false;
    }

    SessionLog::Step(
        L"launch",
        L"InjectDll ok %s module=0x%08X loader=%s(target RVA)",
        dllPath.wstring().c_str(),
        exitCode,
        loaderOwner.c_str());

    return true;
}

bool WaitForEntryPoint(
    HANDLE process,
    HANDLE thread,
    DWORD entryPoint,
    const std::atomic<bool>* const cancel,
    std::wstring* error)
{
    constexpr WORD kLock = 0xFEEB;
    WORD original = 0;
    SIZE_T transferred = 0u;
    if (!ReadProcessMemory(
            process,
            reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(entryPoint)),
            &original,
            sizeof(original),
            &transferred)
        || transferred != sizeof(original))
    {
        if (error != nullptr)
        {
            *error = util::Format(
                L"Could not read the EFZ entry point: %s",
                util::Win32Error(GetLastError()).c_str());
        }
        return false;
    }

    if (!WriteProcessMemory(
            process,
            reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(entryPoint)),
            &kLock,
            sizeof(kLock),
            &transferred)
        || transferred != sizeof(kLock))
    {
        if (error != nullptr)
        {
            *error = util::Format(
                L"Could not prepare the EFZ entry point: %s",
                util::Win32Error(GetLastError()).c_str());
        }
        return false;
    }

    if (!FlushInstructionCache(
            process,
            reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(entryPoint)),
            sizeof(kLock)))
    {
        if (error != nullptr)
        {
            *error = util::Format(
                L"Could not activate the EFZ startup guard: %s",
                util::Win32Error(GetLastError()).c_str());
        }
        return false;
    }

    CONTEXT ctx{};
    const ULONGLONG deadline = GetTickCount64() + kEntryPointTimeoutMs;
    while (GetTickCount64() < deadline)
    {
        if (cancel != nullptr && cancel->load())
        {
            if (error != nullptr)
            {
                *error = L"Game launch cancelled.";
            }
            return false;
        }
        if (ResumeThread(thread) == static_cast<DWORD>(-1))
        {
            if (error != nullptr)
            {
                *error = util::Format(
                    L"Could not resume EFZ during startup: %s",
                    util::Win32Error(GetLastError()).c_str());
            }
            return false;
        }
        Sleep(5u);
        if (SuspendThread(thread) == static_cast<DWORD>(-1))
        {
            if (error != nullptr)
            {
                *error = util::Format(
                    L"Could not pause EFZ during startup: %s",
                    util::Win32Error(GetLastError()).c_str());
            }
            return false;
        }
        ctx = {};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(thread, &ctx))
        {
            continue;
        }
        if (ctx.Eip == entryPoint)
        {
            break;
        }
    }

    if (ctx.Eip != entryPoint)
    {
        if (error != nullptr)
        {
            *error = util::Format(
                L"EFZ did not reach its startup point within %u seconds.",
                kEntryPointTimeoutMs / 1000u);
        }
        return false;
    }

    if (!WriteProcessMemory(
            process,
            reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(entryPoint)),
            &original,
            sizeof(original),
            &transferred)
        || transferred != sizeof(original))
    {
        if (error != nullptr)
        {
            *error = util::Format(
                L"Could not restore the EFZ entry point: %s",
                util::Win32Error(GetLastError()).c_str());
        }
        return false;
    }
    if (!FlushInstructionCache(
            process,
            reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(entryPoint)),
            sizeof(original)))
    {
        if (error != nullptr)
        {
            *error = util::Format(
                L"Could not activate the restored EFZ entry point: %s",
                util::Win32Error(GetLastError()).c_str());
        }
        return false;
    }
    return true;
}

}

LaunchResult GameLauncher::Launch(
    const std::filesystem::path& efzExe,
    const std::vector<std::filesystem::path>& dlls,
    const bool highPriority,
    const std::filesystem::path& sessionIniPath,
    const std::atomic<bool>* const cancel,
    const std::function<void()>& beforeResume) const
{
    const CompatibilityRuntimeInfo runtime = DetectCompatibilityRuntime();
    std::error_code pathError;
    std::filesystem::path resolvedExe = std::filesystem::absolute(efzExe, pathError);
    if (pathError)
    {
        resolvedExe = efzExe;
    }
    resolvedExe = resolvedExe.lexically_normal();

    SessionLog::Step(
        L"launch",
        L"Launch EFZ=%s dllCount=%zu highPriority=%d sessionIni=%s runtime=%s",
        resolvedExe.wstring().c_str(),
        dlls.size(),
        highPriority ? 1 : 0,
        sessionIniPath.empty() ? L"(legacy)" : sessionIniPath.wstring().c_str(),
        runtime.display.c_str());
    if (runtime.IsWine())
    {
        SessionLog::Info(
            L"compat",
            L"Runtime=%s build=%s host=%s prefix=%s SteamAppId=%s",
            runtime.display.c_str(),
            runtime.wineBuildId.empty() ? L"(unreported)" : runtime.wineBuildId.c_str(),
            runtime.hostSystem.empty() ? L"(unreported)" : runtime.hostSystem.c_str(),
            runtime.prefixPath.empty() ? L"(default)" : runtime.prefixPath.c_str(),
            runtime.steamAppId.empty() ? L"(none)" : runtime.steamAppId.c_str());
    }

    LaunchResult result{};
    result.sessionIniPath = sessionIniPath;
    auto addCompatibilityAdvice = [&]()
    {
        result.message += CompatibilityLaunchAdvice(runtime);
    };
    if (!util::FileExists(resolvedExe))
    {
        result.message = util::Format(L"EFZ executable not found: %s", resolvedExe.wstring().c_str());
        addCompatibilityAdvice();
        return result;
    }

    std::vector<std::filesystem::path> resolvedDlls;
    resolvedDlls.reserve(dlls.size());
    for (const auto& dll : dlls)
    {
        pathError.clear();
        std::filesystem::path resolvedDll = std::filesystem::absolute(dll, pathError);
        if (pathError)
        {
            resolvedDll = dll;
        }
        resolvedDll = resolvedDll.lexically_normal();
        if (!util::FileExists(resolvedDll))
        {
            result.message = util::Format(L"DLL not found: %s", resolvedDll.wstring().c_str());
            addCompatibilityAdvice();
            return result;
        }
        resolvedDlls.push_back(std::move(resolvedDll));
    }

    const std::filesystem::path workDir = resolvedExe.parent_path();

    std::vector<std::pair<std::wstring, std::wstring>> environmentReplacements;
    if (!sessionIniPath.empty())
    {
        environmentReplacements.emplace_back(
            L"TRIAL_CASTER_SESSION_INI",
            sessionIniPath.wstring());
    }
    else
    {
        environmentReplacements.emplace_back(L"TRIAL_CASTER_ONLY", L"1");
    }

    const std::filesystem::path directDraw = workDir / L"ddraw.dll";
    if (runtime.IsWine() && util::FileExists(directDraw))
    {
        const std::wstring overrides = util::EnsureNativeDirectDrawOverride(
            EnvironmentValue(L"WINEDLLOVERRIDES"));
        environmentReplacements.emplace_back(L"WINEDLLOVERRIDES", overrides);
        SessionLog::Step(
            L"compat",
            L"Native DirectDraw wrapper enabled for child: WINEDLLOVERRIDES=%s",
            overrides.c_str());
    }

    std::vector<wchar_t> childEnv =
        util::BuildChildEnvironmentBlock(environmentReplacements);
    if (childEnv.empty())
    {
        result.message = L"Could not copy the launcher environment for the game process.";
        addCompatibilityAdvice();
        return result;
    }
    if (cancel != nullptr && cancel->load())
    {
        result.message = L"Game launch cancelled.";
        return result;
    }

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    DWORD createError = ERROR_SUCCESS;
    auto tryCreate = [&](const DWORD flags) -> bool
    {
        si = {};
        si.cb = sizeof(si);
        pi = {};
        std::wstring command = L"\"" + resolvedExe.wstring() + L"\"";
        std::vector<wchar_t> commandBuffer(command.begin(), command.end());
        commandBuffer.push_back(L'\0');
        const BOOL created = CreateProcessW(
                   resolvedExe.c_str(),
                   commandBuffer.data(),
                   nullptr,
                   nullptr,
                   FALSE,
                   flags,
                   childEnv.data(),
                   workDir.wstring().c_str(),
                   &si,
                   &pi);
        createError = created ? ERROR_SUCCESS : GetLastError();
        return created != FALSE;
    };

    DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
    if (highPriority)
    {
        flags |= HIGH_PRIORITY_CLASS;
    }

    if (!tryCreate(flags))
    {
        const DWORD firstError = createError;
        if (highPriority && (flags & HIGH_PRIORITY_CLASS) != 0)
        {
            const DWORD retryFlags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
            if (tryCreate(retryFlags))
            {
                SessionLog::Warn(
                    L"launch",
                    L"High priority was unavailable; continuing normally (%s)",
                    util::Win32Error(firstError).c_str());
            }
        }

        if (pi.hProcess == nullptr)
        {
            const DWORD finalError = createError != ERROR_SUCCESS ? createError : firstError;
            result.message = util::Format(
                L"The game process could not be started: %s",
                util::Win32Error(finalError).c_str());
            addCompatibilityAdvice();
            return result;
        }
    }

    DWORD entryPoint = 0u;
    if (!ResolveMainEntryPoint(pi.hProcess, &entryPoint, &result.message))
    {
        SessionLog::Error(L"launch", L"%s", result.message.c_str());
        TerminateProcess(pi.hProcess, 1u);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        addCompatibilityAdvice();
        return result;
    }
    SessionLog::Step(L"launch", L"CreateProcess ok pid=%lu entry=0x%08X", pi.dwProcessId, entryPoint);
    if (!WaitForEntryPoint(
            pi.hProcess,
            pi.hThread,
            entryPoint,
            cancel,
            &result.message))
    {
        SessionLog::Error(L"launch", L"%s", result.message.c_str());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        addCompatibilityAdvice();
        return result;
    }
    SessionLog::Step(L"launch", L"Entry point reached 0x%08X", entryPoint);

    // Deferred to the entry point: Wine's loader list is complete there.
    for (const auto& dll : resolvedDlls)
    {
        SessionLog::Step(L"launch", L"InjectDll %s", dll.wstring().c_str());
        if (!InjectDll(
                pi.hProcess,
                pi.dwProcessId,
                dll,
                cancel,
                &result.message))
        {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            addCompatibilityAdvice();
            return result;
        }
    }

    if (cancel != nullptr && cancel->load())
    {
        result.message = L"Game launch cancelled.";
        TerminateProcess(pi.hProcess, 1u);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return result;
    }

    // Release after every DLL loads, before EFZ can bind the same port.
    if (beforeResume)
    {
        beforeResume();
        SessionLog::Step(L"launch", L"Pre-resume resource handoff completed");
    }

    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1))
    {
        result.message = util::Format(
            L"The game was prepared but could not be resumed: %s",
            util::Win32Error(GetLastError()).c_str());
        TerminateProcess(pi.hProcess, 1u);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        addCompatibilityAdvice();
        return result;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    result.ok = true;
    result.processId = pi.dwProcessId;
    result.message = util::Format(L"Launched EFZ (PID %lu)", pi.dwProcessId);
    SessionLog::Step(L"launch", L"%s", result.message.c_str());
    return result;
}

}
