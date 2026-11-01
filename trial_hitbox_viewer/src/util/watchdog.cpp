#include "watchdog.h"

#include "debug_log.h"
#include "lifecycle_log.h"
#include "win32_compat.h"

#include <windows.h>
#include <TlHelp32.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace thv::watchdog
{
namespace
{

constexpr DWORD kStallMs = 2500u;
constexpr DWORD kPollMs = 250u;

std::atomic<bool> g_initialized{false};
std::atomic<bool> g_stop{false};
HANDLE g_thread = nullptr;

// Only the game thread pushes/pops; the watchdog thread reads the published top.
constexpr int kMaxPhaseDepth = 8;
const char* g_phaseStack[kMaxPhaseDepth] = {};
std::atomic<int> g_phaseDepth{0};
std::atomic<const char*> g_phase{nullptr};
std::atomic<DWORD> g_phaseStartMs{0};
std::atomic<DWORD> g_gameTid{0};
std::atomic<std::uint32_t> g_phaseSeq{0};
std::uint32_t g_lastDumpedSeq = 0xFFFFFFFFu;

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
            std::snprintf(mods[count].name, sizeof(mods[count].name), "%s", me.szModule);
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

void DumpStalledThread(const char* phase, DWORD stalledMs, DWORD tid)
{
    // Never take g_mutex here: the stalled game thread likely holds it.
    lifecycle::Fault("watchdog", "game thread STALLED phase=%s for %lums tid=%lu", phase, stalledMs, tid);
    log::Critical("watchdog", "game thread STALLED in phase '%s' for %lu ms tid=%lu", phase, stalledMs, tid);

    // SuspendThread can park a Wine thread holding a lock and deadlock this logging.
    if (win32::IsWineRuntime())
    {
        log::Critical(
            "watchdog",
            "Wine/Proton runtime: destructive SuspendThread stack sample skipped");
        log::FlushFile();
        return;
    }

    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (th == nullptr)
    {
        log::Critical("watchdog", "OpenThread failed (%lu) — cannot walk stalled stack", GetLastError());
        log::FlushFile();
        return;
    }

    if (SuspendThread(th) == static_cast<DWORD>(-1))
    {
        log::Critical("watchdog", "SuspendThread failed (%lu)", GetLastError());
        CloseHandle(th);
        log::FlushFile();
        return;
    }

    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(th, &ctx))
    {
        LoadedModule mods[256];
        const int modCount = BuildModuleTable(mods, 256);

        auto sym = [&](std::uintptr_t a, char* buf, std::size_t n) {
            const LoadedModule* m = FindModule(mods, modCount, a);
            if (m != nullptr)
            {
                std::snprintf(buf, n, "%s+0x%lX", m->name, static_cast<unsigned long>(a - m->base));
            }
            else
            {
                std::snprintf(buf, n, "(unmapped)");
            }
        };

#if defined(_M_IX86)
        char eipS[96] = {};
        sym(ctx.Eip, eipS, sizeof(eipS));
        log::Critical(
            "watchdog",
            "EIP=0x%08lX %s  ESP=0x%08lX EBP=0x%08lX",
            static_cast<unsigned long>(ctx.Eip),
            eipS,
            static_cast<unsigned long>(ctx.Esp),
            static_cast<unsigned long>(ctx.Ebp));

        const auto esp = static_cast<std::uintptr_t>(ctx.Esp);
        int printed = 0;
        for (std::uintptr_t a = esp; a < esp + 0x600 && printed < 32; a += 4)
        {
            if (!RegionReadable(reinterpret_cast<const void*>(a), 4))
            {
                break;
            }
            const auto val = *reinterpret_cast<const std::uintptr_t*>(a);
            const LoadedModule* m = FindModule(mods, modCount, val);
            if (m != nullptr)
            {
                log::Critical(
                    "watchdog",
                    "  [%08lX] %s+0x%lX",
                    static_cast<unsigned long>(a),
                    m->name,
                    static_cast<unsigned long>(val - m->base));
                ++printed;
            }
        }
#endif
    }
    else
    {
        log::Critical("watchdog", "GetThreadContext failed (%lu)", GetLastError());
    }

    ResumeThread(th);
    CloseHandle(th);
    log::Critical("watchdog", "--- end stall dump ---");
    log::FlushFile();
}

DWORD WINAPI WatchdogThread(LPVOID)
{
    while (!g_stop.load())
    {
        Sleep(kPollMs);

        const char* const phase = g_phase.load();
        if (phase == nullptr)
        {
            continue;
        }
        const DWORD tid = g_gameTid.load();
        if (tid == 0)
        {
            continue;
        }

        const DWORD elapsed = GetTickCount() - g_phaseStartMs.load();
        const std::uint32_t seq = g_phaseSeq.load();
        if (elapsed >= kStallMs && seq != g_lastDumpedSeq)
        {
            g_lastDumpedSeq = seq;
            DumpStalledThread(phase, elapsed, tid);
        }
    }
    return 0;
}

}

bool Initialize()
{
    if (g_initialized.exchange(true))
    {
        return true;
    }
    g_stop.store(false);
    g_thread = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
    log::Info("watchdog", "stall watchdog %s (threshold %lums)", g_thread != nullptr ? "online" : "FAILED", kStallMs);
    return g_thread != nullptr;
}

void Shutdown()
{
    if (!g_initialized.exchange(false))
    {
        return;
    }
    g_stop.store(true);
    if (g_thread != nullptr)
    {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

void MarkGameThread()
{
    g_gameTid.store(GetCurrentThreadId());
}

void EnterPhase(const char* name)
{
    const int d = g_phaseDepth.load();
    if (d < kMaxPhaseDepth)
    {
        g_phaseStack[d] = name;
        g_phaseDepth.store(d + 1);
    }
    g_phase.store(name);
    g_phaseStartMs.store(GetTickCount());
    g_phaseSeq.fetch_add(1u);
}

void ExitPhase()
{
    int d = g_phaseDepth.load();
    if (d > 0)
    {
        --d;
        g_phaseDepth.store(d);
    }
    g_phase.store(d > 0 ? g_phaseStack[d - 1] : nullptr);
    g_phaseStartMs.store(GetTickCount());
    g_phaseSeq.fetch_add(1u);
}

}
