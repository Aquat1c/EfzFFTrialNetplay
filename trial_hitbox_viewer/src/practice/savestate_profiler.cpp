#include "savestate_profiler.h"

#include "debug_log.h"
#include "trial_addresses.h"

#include <atomic>
#include <cstring>
#include <vector>

namespace thv::savestate_profiler
{
namespace
{

constexpr std::size_t kPageSize = 4096u;

std::atomic<bool> g_enabled{false};

std::vector<std::uint8_t> g_prev;
std::vector<std::uint32_t> g_changeHits;
std::uint32_t g_baseVa = 0;
std::uint64_t g_frames = 0;
std::size_t g_changedPagesEver = 0;
bool g_havePrev = false;

void ResetState()
{
    g_prev.clear();
    g_changeHits.clear();
    g_baseVa = 0;
    g_frames = 0;
    g_changedPagesEver = 0;
    g_havePrev = false;
}

}

void SetEnabled(const bool enabled)
{
    const bool was = g_enabled.exchange(enabled);
    if (was == enabled)
    {
        return;
    }
    if (enabled)
    {
        ResetState();
        log::Info("ss_profile", "savestate profiler ENABLED — play a match, then toggle off to dump");
    }
    else
    {
        Dump("disabled");
    }
}

bool IsEnabled()
{
    return g_enabled.load();
}

void OnFrameCaptured(const std::uint8_t* data, const std::size_t size, const std::uint32_t baseVa)
{
    if (!g_enabled.load() || data == nullptr || size == 0)
    {
        return;
    }

    const std::size_t pageCount = (size + kPageSize - 1u) / kPageSize;

    if (!g_havePrev || g_prev.size() != size)
    {
        // Guard bad_alloc: an uncaught exception would abort the game.
        try
        {
            g_prev.assign(data, data + size);
            g_changeHits.assign(pageCount, 0u);
        }
        catch (...)
        {
            g_enabled.store(false);
            g_prev.clear();
            g_changeHits.clear();
            g_havePrev = false;
            log::Error("ss_profile", "baseline alloc failed (%zu B) — profiler disabled", size);
            return;
        }
        g_baseVa = baseVa;
        g_frames = 0;
        g_changedPagesEver = 0;
        g_havePrev = true;
        return;
    }

    for (std::size_t page = 0; page < pageCount; ++page)
    {
        const std::size_t off = page * kPageSize;
        const std::size_t len = (off + kPageSize <= size) ? kPageSize : (size - off);
        if (std::memcmp(data + off, g_prev.data() + off, len) != 0)
        {
            if (g_changeHits[page] == 0u)
            {
                ++g_changedPagesEver;
            }
            ++g_changeHits[page];
        }
    }

    std::memcpy(g_prev.data(), data, size);
    ++g_frames;

    if ((g_frames % 600u) == 0u)
    {
        Dump("periodic");
    }
}

void SampleLiveRegion()
{
    if (!g_enabled.load())
    {
        return;
    }

    const auto* const mode =
        static_cast<const std::uint8_t*>(trial::ResolveVa(trial::kVaBattleScreenMode));
    if (mode == nullptr || *mode != trial::kScreenBattle)
    {
        return;
    }

    const std::size_t size = trial::kBattleSimSize + trial::kMatchAuxSize;
    const auto* const live =
        static_cast<const std::uint8_t*>(trial::ResolveVa(trial::kVaBattleStateBegin));
    if (live == nullptr)
    {
        return;
    }

    OnFrameCaptured(live, size, static_cast<std::uint32_t>(trial::kVaBattleStateBegin));
}

void Dump(const char* reason)
{
    if (!g_havePrev || g_changeHits.empty())
    {
        log::Info("ss_profile", "dump (%s): no data captured yet", reason != nullptr ? reason : "?");
        return;
    }

    const std::size_t totalPages = g_changeHits.size();
    log::Info(
        "ss_profile",
        "=== dirty-page map (%s) frames=%llu base=0x%08X pages=%zu changed=%zu (%.1f KB of %.1f KB ever dirty) ===",
        reason != nullptr ? reason : "?",
        static_cast<unsigned long long>(g_frames),
        g_baseVa,
        totalPages,
        g_changedPagesEver,
        static_cast<double>(g_changedPagesEver * kPageSize) / 1024.0,
        static_cast<double>(totalPages * kPageSize) / 1024.0);

    std::size_t runStart = 0;
    bool inRun = false;
    std::uint32_t runPeak = 0;
    for (std::size_t page = 0; page <= totalPages; ++page)
    {
        const bool dirty = (page < totalPages) && (g_changeHits[page] > 0u);
        if (dirty && !inRun)
        {
            inRun = true;
            runStart = page;
            runPeak = g_changeHits[page];
        }
        else if (dirty)
        {
            if (g_changeHits[page] > runPeak)
            {
                runPeak = g_changeHits[page];
            }
        }
        else if (inRun)
        {
            const std::uint32_t vaLo = g_baseVa + static_cast<std::uint32_t>(runStart * kPageSize);
            const std::uint32_t vaHi = g_baseVa + static_cast<std::uint32_t>(page * kPageSize);
            log::Info(
                "ss_profile",
                "  range 0x%08X-0x%08X  %zu page(s)  %.1f KB  peakHits=%u",
                vaLo,
                vaHi,
                page - runStart,
                static_cast<double>((page - runStart) * kPageSize) / 1024.0,
                runPeak);
            inRun = false;
        }
    }
    log::Info("ss_profile", "=== end dirty-page map ===");
    log::FlushFile();
}

void Reset()
{
    ResetState();
}

}
