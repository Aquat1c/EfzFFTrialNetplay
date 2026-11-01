#include "netplay_input.h"

#include "debug_log.h"
#include "lifecycle_log.h"
#include "mem_write_log.h"
#include "trial_addresses.h"

#include <atomic>
#include <cstring>

namespace thv::netplay_input
{
namespace
{

constexpr int kBindingSlotCount = 8;

std::atomic<bool> g_bindingsSwapped{false};

void SwapBindingBlocks()
{
    auto* const p1Keys = static_cast<std::uint8_t*>(trial::ResolveVa(trial::kVaDiKeyCodesP1));
    auto* const p2Keys = static_cast<std::uint8_t*>(trial::ResolveVa(trial::kVaDiKeyCodesP2));
    auto* const p1Types = static_cast<std::uint8_t*>(trial::ResolveVa(trial::kVaDiDeviceTypesP1));
    auto* const p2Types = static_cast<std::uint8_t*>(trial::ResolveVa(trial::kVaDiDeviceTypesP2));
    if (p1Keys == nullptr || p2Keys == nullptr || p1Types == nullptr || p2Types == nullptr)
    {
        return;
    }

    for (int slot = 0; slot < kBindingSlotCount; ++slot)
    {
        const std::uint8_t keyA = p1Keys[slot];
        const std::uint8_t keyB = p2Keys[slot];
        if (keyA != keyB)
        {
            memlog::LogScalarWrite(
                "cleanup_pair",
                trial::kVaDiKeyCodesP1 + static_cast<std::uintptr_t>(slot),
                slot == 0 ? "p1_key_swap" : "p1_key",
                keyA,
                keyB,
                1);
        }
        p1Keys[slot] = keyB;
        p2Keys[slot] = keyA;

        const std::uint8_t typeA = p1Types[slot];
        const std::uint8_t typeB = p2Types[slot];
        if (typeA != typeB)
        {
            memlog::LogScalarWrite(
                "cleanup_pair",
                trial::kVaDiDeviceTypesP1 + static_cast<std::uintptr_t>(slot),
                slot == 0 ? "p1_dev_swap" : "p1_dev",
                typeA,
                typeB,
                1);
        }
        p1Types[slot] = typeB;
        p2Types[slot] = typeA;
    }
}

}

void SyncCleanupPairForLocalSide(const int localSide)
{
    const bool wantSwapped = localSide == 1;
    if (wantSwapped == g_bindingsSwapped.load())
    {
        return;
    }

    SwapBindingBlocks();
    g_bindingsSwapped.store(wantSwapped);
    lifecycle::InputPath("CleanupPair %s", wantSwapped ? "client bindings swapped" : "bindings restored");
    log::Step("netplay", "CleanupPair %s", wantSwapped ? "client bindings swapped" : "bindings restored");
}

void RestoreCleanupPair()
{
    if (!g_bindingsSwapped.load())
    {
        return;
    }

    SwapBindingBlocks();
    g_bindingsSwapped.store(false);
    lifecycle::InputPath("CleanupPair restored on session end");
    log::Step("netplay", "CleanupPair restored");
}

bool IsCleanupPairSwapped()
{
    return g_bindingsSwapped.load();
}

}
