#include "rng.h"

#include "debug_log.h"
#include "determinism.h"
#include "iat_hook.h"
#include "lifecycle_log.h"
#include "mem_write_log.h"

#include <MinHook.h>

#include <windows.h>

#include <atomic>
#include <cstring>

namespace thv::det::rng
{
namespace
{

constexpr std::uint32_t kLcgMul = 0x343FDu;
constexpr std::uint32_t kLcgAdd = 0x269EC3u;
constexpr std::uint32_t kRandMax = 0x7FFFu;

constexpr const char* kHostDllName = "MSVCRT.dll";

using SrandFn = void(__cdecl*)(unsigned int);
using RandFn = int(__cdecl*)(void);

std::atomic<bool> g_initialized{false};
std::atomic<bool> g_iatSrandHooked{false};
std::atomic<bool> g_iatRandHooked{false};
std::atomic<bool> g_staticRandHooked{false};
std::atomic<bool> g_staticSrandHooked{false};
std::atomic<std::uint32_t> g_shadowState{1u};
std::atomic<std::uint32_t> g_srandCalls{0};
std::atomic<std::uint32_t> g_randCalls{0};
std::atomic<std::uint32_t> g_lastSeed{0};

SrandFn g_originalSrand = nullptr;
RandFn g_originalRand = nullptr;
HMODULE g_hostModule = nullptr;

void* g_staticRandTarget = nullptr;
void* g_staticSrandTarget = nullptr;

thread_local bool g_threadOriginalStateKnown = false;
thread_local std::uint32_t g_threadOriginalState = 1u;

thread_local int g_visualRandDepth = 0;
std::atomic<std::uint32_t> g_visualState{0xC0FFEE01u};
std::atomic<std::uint32_t> g_visualRandCalls{0};

void __cdecl SrandShim(unsigned int seed);
int __cdecl RandShim();

bool GetTextSection(HMODULE mod, std::uint8_t*& outBase, std::size_t& outSize)
{
    auto* const dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    if (dos == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return false;
    }

    auto* const nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<std::uint8_t*>(mod) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
    {
        return false;
    }

    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        if (std::memcmp(sec->Name, ".text", 5) == 0)
        {
            outBase = reinterpret_cast<std::uint8_t*>(mod) + sec->VirtualAddress;
            outSize = sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

std::uint8_t* FindLastFunctionPrologueBefore(std::uint8_t* fromExclusive, std::uint8_t* lo, std::size_t maxBack)
{
    if (fromExclusive == nullptr || lo == nullptr || fromExclusive <= lo + 2)
    {
        return nullptr;
    }

    std::uint8_t* const start = fromExclusive - 1;
    std::uint8_t* const limit =
        (static_cast<std::size_t>(start - lo) > maxBack) ? (start - maxBack) : lo;

    for (std::uint8_t* p = start;; --p)
    {
        if (p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC)
        {
            return p;
        }
        if (p == limit)
        {
            break;
        }
    }
    return nullptr;
}

struct RandScanResult
{
    std::uint8_t* randEntry = nullptr;
    std::uint8_t* srandEntry = nullptr;
};

RandScanResult ScanForStaticCrtRand(HMODULE mod)
{
    RandScanResult result;
    std::uint8_t* base = nullptr;
    std::size_t size = 0;
    if (!GetTextSection(mod, base, size))
    {
        return result;
    }

    if (size < 16)
    {
        return result;
    }

    const std::size_t hi = size - 12;
    for (std::size_t i = 0; i < hi; ++i)
    {
        if (base[i] != 0x69)
        {
            continue;
        }
        if (base[i + 2] != 0xFD || base[i + 3] != 0x43 || base[i + 4] != 0x03 || base[i + 5] != 0x00)
        {
            continue;
        }
        if (base[i + 6] != 0x81 || base[i + 8] != 0xC3 || base[i + 9] != 0x9E || base[i + 10] != 0x26
            || base[i + 11] != 0x00)
        {
            continue;
        }

        result.randEntry = FindLastFunctionPrologueBefore(base + i, base, 64);
        if (result.randEntry != nullptr && result.randEntry > base + 4)
        {
            result.srandEntry = FindLastFunctionPrologueBefore(result.randEntry, base, 64);
        }
        return result;
    }

    return result;
}

bool TryStaticCrtHook()
{
    const RandScanResult scan = ScanForStaticCrtRand(g_hostModule);
    if (scan.randEntry == nullptr)
    {
        return false;
    }

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        return false;
    }

    void* originalRand = nullptr;
    if (MH_CreateHook(scan.randEntry, reinterpret_cast<void*>(&RandShim), &originalRand) != MH_OK)
    {
        return false;
    }
    if (MH_QueueEnableHook(scan.randEntry) != MH_OK)
    {
        return false;
    }

    g_staticRandTarget = scan.randEntry;
    g_originalRand = reinterpret_cast<RandFn>(originalRand);
    g_staticRandHooked.store(true, std::memory_order_release);

    if (scan.srandEntry != nullptr)
    {
        void* originalSrand = nullptr;
        if (MH_CreateHook(scan.srandEntry, reinterpret_cast<void*>(&SrandShim), &originalSrand) == MH_OK
            && MH_QueueEnableHook(scan.srandEntry) == MH_OK)
        {
            g_staticSrandTarget = scan.srandEntry;
            g_originalSrand = reinterpret_cast<SrandFn>(originalSrand);
            g_staticSrandHooked.store(true, std::memory_order_release);
        }
    }

    return true;
}

std::uint32_t AdvanceShadow(std::uint32_t* outPrevState = nullptr)
{
    std::uint32_t prev = g_shadowState.load(std::memory_order_acquire);
    std::uint32_t next = 0;
    do
    {
        next = prev * kLcgMul + kLcgAdd;
    } while (!g_shadowState.compare_exchange_weak(prev, next, std::memory_order_acq_rel));

    if (outPrevState != nullptr)
    {
        *outPrevState = prev;
    }
    return next;
}

void __cdecl SrandShim(unsigned int seed)
{
    g_srandCalls.fetch_add(1, std::memory_order_relaxed);
    g_lastSeed.store(seed, std::memory_order_relaxed);
    g_shadowState.store(seed, std::memory_order_release);

    if (g_originalSrand != nullptr)
    {
        g_originalSrand(seed);
    }

    g_threadOriginalStateKnown = true;
    g_threadOriginalState = seed;
    lifecycle::Netplay("srand seed=0x%08X totalCalls=%u", seed, g_srandCalls.load(std::memory_order_relaxed));
    log::Info("rng", "srand seed=0x%08X totalCalls=%u", seed, g_srandCalls.load(std::memory_order_relaxed));
}

int __cdecl RandShim()
{
    // Leave the shadow and holdrand untouched: the sim sequence can't depend on draw count.
    if (g_visualRandDepth > 0)
    {
        std::uint32_t prev = g_visualState.load(std::memory_order_relaxed);
        std::uint32_t next = 0;
        do
        {
            next = prev * kLcgMul + kLcgAdd;
        } while (!g_visualState.compare_exchange_weak(prev, next, std::memory_order_relaxed));
        g_visualRandCalls.fetch_add(1, std::memory_order_relaxed);
        return static_cast<int>((next >> 16) & kRandMax);
    }

    std::uint32_t prevState = 0;
    const std::uint32_t newState = AdvanceShadow(&prevState);
    const int shadowOut = static_cast<int>((newState >> 16) & kRandMax);

    if (g_originalRand != nullptr)
    {
        if (g_originalSrand != nullptr
            && (!g_threadOriginalStateKnown || g_threadOriginalState != prevState))
        {
            g_originalSrand(prevState);
            g_threadOriginalStateKnown = true;
            g_threadOriginalState = prevState;
        }

        (void)g_originalRand();
        g_threadOriginalStateKnown = true;
        g_threadOriginalState = newState;
    }

    g_randCalls.fetch_add(1, std::memory_order_relaxed);
    return shadowOut;
}

}

bool Initialize()
{
    if (g_initialized.exchange(true))
    {
        return true;
    }

    g_hostModule = GetModuleHandleA(nullptr);
    if (g_hostModule == nullptr)
    {
        return false;
    }

    const bool patchedSrand = detail::PatchIatEntry(
        g_hostModule,
        kHostDllName,
        "srand",
        reinterpret_cast<void*>(&SrandShim),
        reinterpret_cast<void**>(&g_originalSrand));
    const bool patchedRand = detail::PatchIatEntry(
        g_hostModule,
        kHostDllName,
        "rand",
        reinterpret_cast<void*>(&RandShim),
        reinterpret_cast<void**>(&g_originalRand));
    g_iatSrandHooked.store(patchedSrand, std::memory_order_release);
    g_iatRandHooked.store(patchedRand, std::memory_order_release);

    if (!patchedSrand || !patchedRand)
    {
        const bool staticOk = TryStaticCrtHook();
        if (staticOk)
        {
            if (g_staticRandHooked.load(std::memory_order_acquire))
            {
                g_iatRandHooked.store(true, std::memory_order_release);
            }
            if (g_staticSrandHooked.load(std::memory_order_acquire))
            {
                g_iatSrandHooked.store(true, std::memory_order_release);
            }
        }
    }

    const bool hooked = g_iatRandHooked.load(std::memory_order_acquire);
    log::Info(
        "rng",
        "initialized hooked=%d iatRand=%d iatSrand=%d staticRand=%d staticSrand=%d",
        hooked ? 1 : 0,
        g_iatRandHooked.load(std::memory_order_relaxed) ? 1 : 0,
        g_iatSrandHooked.load(std::memory_order_relaxed) ? 1 : 0,
        g_staticRandHooked.load(std::memory_order_relaxed) ? 1 : 0,
        g_staticSrandHooked.load(std::memory_order_relaxed) ? 1 : 0);
    return hooked;
}

void Shutdown()
{
    if (!g_initialized.exchange(false))
    {
        return;
    }

    if (g_iatSrandHooked.load(std::memory_order_acquire) && g_originalSrand != nullptr && !g_staticSrandHooked.load())
    {
        detail::UnpatchIatEntry(
            g_hostModule,
            kHostDllName,
            "srand",
            reinterpret_cast<void*>(&SrandShim),
            reinterpret_cast<void*>(g_originalSrand));
    }
    if (g_iatRandHooked.load(std::memory_order_acquire) && g_originalRand != nullptr && !g_staticRandHooked.load())
    {
        detail::UnpatchIatEntry(
            g_hostModule,
            kHostDllName,
            "rand",
            reinterpret_cast<void*>(&RandShim),
            reinterpret_cast<void*>(g_originalRand));
    }

    if (g_staticRandHooked.exchange(false) && g_staticRandTarget != nullptr)
    {
        MH_DisableHook(g_staticRandTarget);
        MH_RemoveHook(g_staticRandTarget);
        g_staticRandTarget = nullptr;
    }
    if (g_staticSrandHooked.exchange(false) && g_staticSrandTarget != nullptr)
    {
        MH_DisableHook(g_staticSrandTarget);
        MH_RemoveHook(g_staticSrandTarget);
        g_staticSrandTarget = nullptr;
    }
}

std::uint32_t Snapshot()
{
    return g_shadowState.load(std::memory_order_acquire);
}

void Restore(std::uint32_t state)
{
    const std::uint32_t before = g_shadowState.load(std::memory_order_acquire);
    if (before != state)
    {
        memlog::LogScalarWrite("rng_restore", 0u, "rng_shadow", before, state, 4);
    }

    g_shadowState.store(state, std::memory_order_release);
    if (g_originalSrand != nullptr)
    {
        g_originalSrand(state);
    }
    g_threadOriginalStateKnown = true;
    g_threadOriginalState = state;
}

void SetSeed(std::uint32_t seed)
{
    const std::uint32_t before = g_shadowState.load(std::memory_order_acquire);
    if (before != seed)
    {
        memlog::LogScalarWrite("rng_set_seed", 0u, "rng_shadow", before, seed, 4);
    }

    g_lastSeed.store(seed, std::memory_order_relaxed);
    g_shadowState.store(seed, std::memory_order_release);
    if (g_originalSrand != nullptr)
    {
        g_originalSrand(seed);
    }
    g_threadOriginalStateKnown = true;
    g_threadOriginalState = seed;
}

int PredictCanary(const std::uint32_t seed)
{
    const std::uint32_t next = seed * kLcgMul + kLcgAdd;
    return static_cast<int>((next >> 16) & kRandMax);
}

void PushVisualRand()
{
    ++g_visualRandDepth;
}

void PopVisualRand()
{
    if (g_visualRandDepth > 0)
    {
        --g_visualRandDepth;
    }
}

std::uint32_t VisualSnapshot()
{
    return g_visualState.load(std::memory_order_acquire);
}

void VisualRestore(const std::uint32_t state)
{
    g_visualState.store(state, std::memory_order_release);
}

Status GetStatus()
{
    Status status = {};
    status.iatSrandHooked = g_iatSrandHooked.load(std::memory_order_acquire);
    status.iatRandHooked = g_iatRandHooked.load(std::memory_order_acquire);
    status.staticRandHooked = g_staticRandHooked.load(std::memory_order_acquire);
    status.staticSrandHooked = g_staticSrandHooked.load(std::memory_order_acquire);
    status.shadowState = g_shadowState.load(std::memory_order_acquire);
    status.srandCalls = g_srandCalls.load(std::memory_order_acquire);
    status.randCalls = g_randCalls.load(std::memory_order_acquire);
    status.visualRandCalls = g_visualRandCalls.load(std::memory_order_acquire);
    status.lastSeed = g_lastSeed.load(std::memory_order_acquire);
    return status;
}

}
