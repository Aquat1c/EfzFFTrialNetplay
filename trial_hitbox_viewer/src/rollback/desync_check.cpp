#include "desync_check.h"

#include "debug_log.h"
#include "lifecycle_log.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace thv::rollback::desync
{
namespace
{

constexpr int kRing = 8192;

// Names match trial::kDynamicBattleRegions order.
const char* const kRegionNames[kRegions] = {
    "p1_core",
    "p1_anim",
    "p2_block",
    "p2_anim",
    "globals",
    "determinism",
    "sim_aux",
};

struct Entry
{
    std::uint32_t frame = 0xFFFFFFFFu;
    std::uint32_t hash[kRegions] = {};
    bool set = false;
};

struct ChainEntry
{
    std::uint32_t frame = 0xFFFFFFFFu;
    std::uint64_t chain = 0;
    bool set = false;
};

std::mutex g_mutex; // leaf lock; may be held under rollback g_mutex, never the reverse
Entry g_local[kRing];
Entry g_remote[kRing];
ChainEntry g_localChain[kRing];
ChainEntry g_remoteChain[kRing];
std::uint32_t g_latestLocalChainFrame = 0xFFFFFFFFu;
std::uint64_t g_latestLocalChain = 14695981039346656037ull;
std::atomic<std::uint32_t> g_mismatches{0};
std::atomic<std::uint32_t> g_lastMismatchFrame{0xFFFFFFFFu};
std::atomic<std::uint32_t> g_resimDivergences{0};
std::uint32_t g_lastLoggedFrame = 0xFFFFFFFFu;
std::uint32_t g_lastOkLogFrame = 0xFFFFFFFFu;
std::uint32_t g_lastChainMismatchFrame = 0xFFFFFFFFu;
std::uint32_t g_lastChainOkLogFrame = 0xFFFFFFFFu;

constexpr int kMaxDumps = 4;
std::atomic<int> g_dumpsRemaining{kMaxDumps};
std::atomic<bool> g_dumpPending{false};
std::atomic<std::uint32_t> g_dumpFrame{0};
std::atomic<std::uint32_t> g_dumpRegionMask{0};

// Caller holds g_mutex.
void CompareLocked(std::uint32_t frame, const std::uint32_t* localH, const std::uint32_t* remoteH)
{
    int diffs = 0;
    std::uint32_t regionMask = 0;
    char detail[256] = {};
    std::size_t off = 0;
    for (int r = 0; r < kRegions; ++r)
    {
        if (localH[r] != remoteH[r])
        {
            ++diffs;
            regionMask |= (1u << r);
            const int n = std::snprintf(
                detail + off,
                sizeof(detail) - off,
                "%s%s(L=%08X R=%08X)",
                off ? " " : "",
                kRegionNames[r],
                localH[r],
                remoteH[r]);
            if (n > 0)
            {
                off += static_cast<std::size_t>(n);
                if (off >= sizeof(detail) - 1)
                {
                    break;
                }
            }
        }
    }

    if (diffs == 0)
    {
        if (frame >= g_lastOkLogFrame + 300u || g_lastOkLogFrame == 0xFFFFFFFFu)
        {
            g_lastOkLogFrame = frame;
            log::Debug("desync", "in sync at frame %u", frame);
        }
        return;
    }

    if (frame == g_lastLoggedFrame)
    {
        return;
    }
    g_lastLoggedFrame = frame;
    const std::uint32_t total = g_mismatches.fetch_add(1u) + 1u;
    g_lastMismatchFrame.store(frame);
    lifecycle::Fault("desync", "DESYNC f=%u regions=%d %s", frame, diffs, detail);
    log::Critical("desync", "STATE DESYNC frame=%u (%d/%d regions): %s [total=%u]", frame, diffs, kRegions, detail, total);

    if (!g_dumpPending.load() && g_dumpsRemaining.load() > 0)
    {
        g_dumpFrame.store(frame);
        g_dumpRegionMask.store(regionMask);
        g_dumpPending.store(true);
        g_dumpsRemaining.fetch_sub(1);
    }
}

std::uint64_t MixWord(std::uint64_t chain, const std::uint32_t word)
{
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;
    for (unsigned shift = 0; shift < 32; shift += 8)
    {
        chain ^= static_cast<std::uint8_t>(word >> shift);
        chain *= kFnvPrime;
    }
    return chain;
}

// Caller holds g_mutex.
void CompareChainLocked(
    const std::uint32_t frame,
    const std::uint64_t localChain,
    const std::uint64_t remoteChain)
{
    if (localChain == remoteChain)
    {
        if (frame >= g_lastChainOkLogFrame + 300u || g_lastChainOkLogFrame == 0xFFFFFFFFu)
        {
            g_lastChainOkLogFrame = frame;
            log::Debug("desync", "confirmed hash chain in sync at frame %u", frame);
        }
        return;
    }
    if (frame == g_lastChainMismatchFrame)
    {
        return;
    }

    g_lastChainMismatchFrame = frame;
    const std::uint32_t total = g_mismatches.fetch_add(1u) + 1u;
    g_lastMismatchFrame.store(frame);
    lifecycle::Fault(
        "desync",
        "CONFIRMED HASH CHAIN DESYNC f=%u local=%016llX remote=%016llX",
        frame,
        static_cast<unsigned long long>(localChain),
        static_cast<unsigned long long>(remoteChain));
    log::Critical(
        "desync",
        "CONFIRMED HASH CHAIN DESYNC frame=%u local=%016llX remote=%016llX [total=%u]",
        frame,
        static_cast<unsigned long long>(localChain),
        static_cast<unsigned long long>(remoteChain),
        total);

    if (!g_dumpPending.load() && g_dumpsRemaining.load() > 0)
    {
        g_dumpFrame.store(frame);
        g_dumpRegionMask.store((1u << kRegions) - 1u);
        g_dumpPending.store(true);
        g_dumpsRemaining.fetch_sub(1);
    }
}

}

void Reset()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < kRing; ++i)
    {
        g_local[i] = Entry{};
        g_remote[i] = Entry{};
        g_localChain[i] = ChainEntry{};
        g_remoteChain[i] = ChainEntry{};
    }
    g_latestLocalChainFrame = 0xFFFFFFFFu;
    g_latestLocalChain = 14695981039346656037ull;
    g_mismatches.store(0);
    g_lastMismatchFrame.store(0xFFFFFFFFu);
    g_resimDivergences.store(0);
    g_lastLoggedFrame = 0xFFFFFFFFu;
    g_lastOkLogFrame = 0xFFFFFFFFu;
    g_lastChainMismatchFrame = 0xFFFFFFFFu;
    g_lastChainOkLogFrame = 0xFFFFFFFFu;
    g_dumpsRemaining.store(kMaxDumps);
    g_dumpPending.store(false);
}

const char* RegionName(int region)
{
    return (region >= 0 && region < kRegions) ? kRegionNames[region] : "?";
}

void RequestDump(std::uint32_t frame, std::uint32_t regionMask)
{
    if (!g_dumpPending.load() && g_dumpsRemaining.load() > 0)
    {
        g_dumpFrame.store(frame);
        g_dumpRegionMask.store(regionMask);
        g_dumpPending.store(true);
        g_dumpsRemaining.fetch_sub(1);
    }
}

bool TakeDumpRequest(std::uint32_t* frame, std::uint32_t* regionMask)
{
    if (!g_dumpPending.exchange(false))
    {
        return false;
    }
    if (frame != nullptr)
    {
        *frame = g_dumpFrame.load();
    }
    if (regionMask != nullptr)
    {
        *regionMask = g_dumpRegionMask.load();
    }
    return true;
}

void RecordLocal(std::uint32_t frame, const std::uint32_t* regionHashes, int count)
{
    if (regionHashes == nullptr || count < kRegions)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    Entry& e = g_local[frame % kRing];
    e.frame = frame;
    std::memcpy(e.hash, regionHashes, sizeof(std::uint32_t) * kRegions);
    e.set = true;

    const Entry& r = g_remote[frame % kRing];
    if (r.set && r.frame == frame)
    {
        CompareLocked(frame, e.hash, r.hash);
    }

    ChainEntry& chainEntry = g_localChain[frame % kRing];
    if (!chainEntry.set || chainEntry.frame != frame)
    {
        // Gaps are expected; a regression would make the rolling chain ambiguous.
        if (g_latestLocalChainFrame != 0xFFFFFFFFu && frame < g_latestLocalChainFrame)
        {
            lifecycle::Fault(
                "desync",
                "confirmed hash chain frame regression latest=%u incoming=%u",
                g_latestLocalChainFrame,
                frame);
            log::Critical(
                "desync",
                "confirmed hash chain frame regression latest=%u incoming=%u",
                g_latestLocalChainFrame,
                frame);
            g_mismatches.fetch_add(1u);
            g_lastMismatchFrame.store(frame);
            return;
        }

        std::uint64_t chain = MixWord(g_latestLocalChain, frame);
        for (int rgn = 0; rgn < kRegions; ++rgn)
        {
            chain = MixWord(chain, regionHashes[rgn]);
        }
        chainEntry.frame = frame;
        chainEntry.chain = chain;
        chainEntry.set = true;
        g_latestLocalChainFrame = frame;
        g_latestLocalChain = chain;
    }

    const ChainEntry& remoteChain = g_remoteChain[frame % kRing];
    if (remoteChain.set && remoteChain.frame == frame)
    {
        CompareChainLocked(frame, chainEntry.chain, remoteChain.chain);
    }
}

void RecordRemote(std::uint32_t frame, const std::uint32_t* regionHashes, int count)
{
    if (regionHashes == nullptr || count < kRegions)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    Entry& e = g_remote[frame % kRing];
    e.frame = frame;
    std::memcpy(e.hash, regionHashes, sizeof(std::uint32_t) * kRegions);
    e.set = true;

    const Entry& l = g_local[frame % kRing];
    if (l.set && l.frame == frame)
    {
        CompareLocked(frame, l.hash, e.hash);
    }
}

bool GetLatestLocalChain(std::uint32_t* frame, std::uint64_t* chain)
{
    if (frame == nullptr || chain == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_latestLocalChainFrame == 0xFFFFFFFFu)
    {
        return false;
    }
    *frame = g_latestLocalChainFrame;
    *chain = g_latestLocalChain;
    return true;
}

void RecordRemoteChain(const std::uint32_t frame, const std::uint64_t chain)
{
    if (frame == 0xFFFFFFFFu)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ChainEntry& e = g_remoteChain[frame % kRing];
    e.frame = frame;
    e.chain = chain;
    e.set = true;

    const ChainEntry& local = g_localChain[frame % kRing];
    if (local.set && local.frame == frame)
    {
        CompareChainLocked(frame, local.chain, e.chain);
    }
}

std::uint32_t Mismatches()
{
    return g_mismatches.load();
}

std::uint32_t LastMismatchFrame()
{
    return g_lastMismatchFrame.load();
}

void NoteResimDivergence()
{
    (void)log::EnableFileLogging();
    g_resimDivergences.fetch_add(1u);
}

std::uint32_t ResimDivergences()
{
    return g_resimDivergences.load();
}

}
