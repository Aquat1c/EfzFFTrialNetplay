#include "mem_write_log.h"

#include "debug_log.h"
#include "trial_addresses.h"

#include <cstdio>
#include <cstring>

namespace thv::memlog
{
namespace
{

constexpr const char* kClass = log::EventClass::MemWrite;
constexpr std::size_t kMaxDiffEntries = 64;
constexpr std::size_t kMaxScanBytes = 2u * 1024u * 1024u;
constexpr std::size_t kSmallHexDump = 64u;
constexpr std::size_t kSparseDiffMax = 8u;
constexpr std::size_t kLargeRegionThreshold = 4096u;

struct ScalarWatch
{
    std::uintptr_t va;
    const char* name;
    int widthBytes;
};

const ScalarWatch kScalarWatches[] = {
    {trial::kVaBattleScreenMode, "screen_mode", 1},
    {trial::kVaScreenSubState, "screen_substate", 4},
    {trial::kVaScreenAnimCounter, "screen_anim", 4},
    {trial::kVaFramePacingLastTick, "frame_pacing_tick", 4},
    {trial::kVaFramePacingToggle, "frame_pacing_battle", 4},
    {trial::kVaFramePacingBusyFlag, "frame_pacing_busy", 4},
    {trial::kVaFrameSubsteps, "frame_substeps", 4},
    {trial::kVaSubstepHoldFlag, "substep_hold", 4},
    {trial::kVaDiActiveFlag, "di_active", 4},
    {trial::kVaCameraX, "camera_x", 4},
    {trial::kVaCameraY, "camera_y", 4},
    {trial::kVaP1MoveId, "p1_move_id", 4},
    {trial::kVaMainMenuCursor, "main_menu_cursor", 1},
    {trial::kVaVsHumanFlagP1, "p1_cpu_vs_human", 4},
    {trial::kVaVsHumanFlagP2, "p2_cpu_vs_human", 4},
    {trial::kVaCharSelectP1State, "charsel_p1_state", 1},
    {trial::kVaCharSelectP2State, "charsel_p2_state", 1},
    {trial::kVaCharSelectP1Pick, "charsel_p1_pick", 4},
    {trial::kVaCharSelectP2Pick, "charsel_p2_pick", 4},
    {trial::kVaCharSelectSide, "charsel_side", 4},
    {trial::kVaCharSelectTimerP1, "charsel_p1_timer", 4},
    {trial::kVaCharSelectTimerP2, "charsel_p2_timer", 4},
    {trial::kVaDebugDrawFlag, "debug_draw", 4},
};

std::uintptr_t ModuleBase()
{
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
}

std::uintptr_t PointerToVa(const void* ptr)
{
    if (ptr == nullptr)
    {
        return 0u;
    }

    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(ptr);
    const std::uintptr_t base = ModuleBase();
    if (base == 0u || address < base)
    {
        return address;
    }
    return address - base + trial::kImageBase;
}

std::uint64_t ReadScalar(const void* ptr, const int widthBytes)
{
    if (ptr == nullptr)
    {
        return 0u;
    }

    switch (widthBytes)
    {
    case 1:
        return *static_cast<const std::uint8_t*>(ptr);
    case 2:
        return *static_cast<const std::uint16_t*>(ptr);
    case 4:
        return *static_cast<const std::uint32_t*>(ptr);
    default:
        return 0u;
    }
}

void AppendHex(char* out, const std::size_t outSize, const std::uint8_t* bytes, const std::size_t count)
{
    if (out == nullptr || outSize == 0 || bytes == nullptr || count == 0u)
    {
        return;
    }

    std::size_t written = 0;
    for (std::size_t i = 0; i < count && written + 3 < outSize; ++i)
    {
        const int n = std::snprintf(out + written, outSize - written, "%02X", bytes[i]);
        if (n <= 0)
        {
            break;
        }
        written += static_cast<std::size_t>(n);
    }
}

bool RegionsOverlap(
    const std::uintptr_t aBegin,
    const std::size_t aSize,
    const std::uintptr_t bBegin,
    const std::size_t bSize)
{
    if (aSize == 0u || bSize == 0u)
    {
        return false;
    }

    const std::uintptr_t aEnd = aBegin + aSize;
    const std::uintptr_t bEnd = bBegin + bSize;
    return aBegin < bEnd && bBegin < aEnd;
}

void LogWatchpointMismatches(
    const char* context,
    const char* label,
    const char* subsystem,
    const void* live,
    const void* expected,
    const std::size_t size)
{
    if (live == nullptr || expected == nullptr || size == 0u)
    {
        return;
    }

    const std::uintptr_t liveBegin = reinterpret_cast<std::uintptr_t>(live);
    const auto* const liveBytes = static_cast<const std::uint8_t*>(live);
    const auto* const expectedBytes = static_cast<const std::uint8_t*>(expected);

    for (const ScalarWatch& watch : kScalarWatches)
    {
        void* const watchLive = trial::ResolveVa(watch.va);
        if (watchLive == nullptr)
        {
            continue;
        }

        const std::uintptr_t watchBegin = reinterpret_cast<std::uintptr_t>(watchLive);
        if (!RegionsOverlap(liveBegin, size, watchBegin, static_cast<std::size_t>(watch.widthBytes)))
        {
            continue;
        }

        const std::size_t offset = watchBegin - liveBegin;
        const std::uint64_t liveValue = ReadScalar(liveBytes + offset, watch.widthBytes);
        const std::uint64_t expectedValue = ReadScalar(expectedBytes + offset, watch.widthBytes);
        if (liveValue == expectedValue)
        {
            continue;
        }

        log::Event(
            kClass,
            subsystem,
            "ctx=%s lbl=%s field=%s va=0x%08X live=0x%llX expected=0x%llX",
            context != nullptr ? context : "?",
            label != nullptr ? label : "?",
            watch.name,
            static_cast<unsigned>(watch.va),
            static_cast<unsigned long long>(liveValue),
            static_cast<unsigned long long>(expectedValue));
    }
}

void LogWatchpointHits(
    const char* context,
    const char* label,
    const void* dst,
    const void* src,
    const std::size_t size)
{
    if (dst == nullptr || src == nullptr || size == 0u)
    {
        return;
    }

    const std::uintptr_t dstBegin = reinterpret_cast<std::uintptr_t>(dst);
    const auto* const beforeBytes = static_cast<const std::uint8_t*>(dst);
    const auto* const afterBytes = static_cast<const std::uint8_t*>(src);

    for (const ScalarWatch& watch : kScalarWatches)
    {
        void* const live = trial::ResolveVa(watch.va);
        if (live == nullptr)
        {
            continue;
        }

        const std::uintptr_t watchBegin = reinterpret_cast<std::uintptr_t>(live);
        if (!RegionsOverlap(dstBegin, size, watchBegin, static_cast<std::size_t>(watch.widthBytes)))
        {
            continue;
        }

        const std::size_t offset = watchBegin - dstBegin;
        const std::uint64_t before = ReadScalar(beforeBytes + offset, watch.widthBytes);
        const std::uint64_t after = ReadScalar(afterBytes + offset, watch.widthBytes);
        if (before == after)
        {
            continue;
        }

        log::Event(
            kClass,
            label != nullptr ? label : "watch",
            "ctx=%s field=%s va=0x%08X before=0x%llX after=0x%llX (%llu -> %llu)",
            context != nullptr ? context : "?",
            watch.name,
            static_cast<unsigned>(watch.va),
            static_cast<unsigned long long>(before),
            static_cast<unsigned long long>(after),
            static_cast<unsigned long long>(before),
            static_cast<unsigned long long>(after));
    }
}

std::uint32_t Fnv1a32(const void* data, const std::size_t size)
{
    if (data == nullptr || size == 0u)
    {
        return 0u;
    }

    const auto* const bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t hash = 2166136261u;
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

bool IsMemlogVerbose()
{
    return GetEnvironmentVariableA("THV_MEMLOG_VERBOSE", nullptr, 0) > 0;
}

// Off by default: region scans cost frame rate. THV_MEMLOG=1 enables.
bool MemlogEnabled()
{
    static const bool enabled = GetEnvironmentVariableA("THV_MEMLOG", nullptr, 0) > 0;
    return enabled;
}

void LogDiffSparse(
    const char* context,
    const char* label,
    const void* dst,
    const void* src,
    const std::size_t size,
    const std::size_t maxEntries)
{
    if (dst == nullptr || src == nullptr || size < 4u || maxEntries == 0u)
    {
        return;
    }

    const auto* const beforeBytes = static_cast<const std::uint8_t*>(dst);
    const auto* const afterBytes = static_cast<const std::uint8_t*>(src);
    const std::uintptr_t dstBegin = reinterpret_cast<std::uintptr_t>(dst);

    std::size_t changed = 0u;
    std::size_t logged = 0u;
    for (std::size_t offset = 0; offset + 4u <= size; offset += 4u)
    {
        const std::uint32_t before = *reinterpret_cast<const std::uint32_t*>(beforeBytes + offset);
        const std::uint32_t after = *reinterpret_cast<const std::uint32_t*>(afterBytes + offset);
        if (before == after)
        {
            continue;
        }

        ++changed;
        if (logged >= maxEntries)
        {
            continue;
        }

        ++logged;
        log::Event(
            kClass,
            label != nullptr ? label : "diff_sparse",
            "ctx=%s off=0x%zX va=0x%08X before=0x%08X after=0x%08X",
            context != nullptr ? context : "?",
            offset,
            static_cast<unsigned>(PointerToVa(reinterpret_cast<const void*>(dstBegin + offset))),
            static_cast<unsigned>(before),
            static_cast<unsigned>(after));
    }

    log::Event(
        kClass,
        label != nullptr ? label : "diff_sparse",
        "ctx=%s changed_u32=%zu logged=%zu total=%zu",
        context != nullptr ? context : "?",
        changed,
        logged,
        size);
}

void LogDiffSummary(
    const char* context,
    const char* label,
    const void* dst,
    const void* src,
    const std::size_t size)
{
    if (dst == nullptr || src == nullptr || size < 4u)
    {
        return;
    }

    const auto* const beforeBytes = static_cast<const std::uint8_t*>(dst);
    const auto* const afterBytes = static_cast<const std::uint8_t*>(src);
    const std::uintptr_t dstBegin = reinterpret_cast<std::uintptr_t>(dst);
    const std::size_t scanBytes = std::min(size, kMaxScanBytes);

    std::size_t changed = 0u;
    std::size_t logged = 0u;
    for (std::size_t offset = 0; offset + 4u <= scanBytes; offset += 4u)
    {
        const std::uint32_t before = *reinterpret_cast<const std::uint32_t*>(beforeBytes + offset);
        const std::uint32_t after = *reinterpret_cast<const std::uint32_t*>(afterBytes + offset);
        if (before == after)
        {
            continue;
        }

        ++changed;
        if (logged >= kMaxDiffEntries)
        {
            continue;
        }

        ++logged;
        const std::uintptr_t absVa = dstBegin + offset;
        log::Event(
            kClass,
            label != nullptr ? label : "diff",
            "ctx=%s off=0x%zX va=0x%08X before=0x%08X after=0x%08X",
            context != nullptr ? context : "?",
            offset,
            static_cast<unsigned>(PointerToVa(reinterpret_cast<const void*>(absVa))),
            static_cast<unsigned>(before),
            static_cast<unsigned>(after));
    }

    if (changed > logged)
    {
        log::Event(
            kClass,
            label != nullptr ? label : "diff",
            "ctx=%s diff_truncated changed_u32=%zu logged=%zu scan=%zu total=%zu",
            context != nullptr ? context : "?",
            changed,
            logged,
            scanBytes,
            size);
    }
    else if (changed == 0u && size > kSmallHexDump)
    {
        log::Event(
            kClass,
            label != nullptr ? label : "diff",
            "ctx=%s no_u32_changes_in_scan scan=%zu total=%zu",
            context != nullptr ? context : "?",
            scanBytes,
            size);
    }
}

}

void LogGameHeadline(const char* tag)
{
    if (!MemlogEnabled()) { return; }
    const std::uint8_t screenMode = ReadScalar(trial::ResolveVa(trial::kVaBattleScreenMode), 1) & 0xFFu;
    const std::uint32_t subState = static_cast<std::uint32_t>(ReadScalar(trial::ResolveVa(trial::kVaScreenSubState), 4));
    const std::int32_t anim = static_cast<std::int32_t>(ReadScalar(trial::ResolveVa(trial::kVaScreenAnimCounter), 4));
    const std::uint32_t diActive = static_cast<std::uint32_t>(ReadScalar(trial::ResolveVa(trial::kVaDiActiveFlag), 4));
    const std::uint32_t cameraX = static_cast<std::uint32_t>(ReadScalar(trial::ResolveVa(trial::kVaCameraX), 4));
    const std::uint32_t cameraY = static_cast<std::uint32_t>(ReadScalar(trial::ResolveVa(trial::kVaCameraY), 4));
    const std::uint32_t p1Move = static_cast<std::uint32_t>(ReadScalar(trial::ResolveVa(trial::kVaP1MoveId), 4));
    const std::uint32_t p1Pick = static_cast<std::uint32_t>(ReadScalar(trial::ResolveVa(trial::kVaCharSelectP1Pick), 4));
    const std::uint32_t p2Pick = static_cast<std::uint32_t>(ReadScalar(trial::ResolveVa(trial::kVaCharSelectP2Pick), 4));
    const std::uint32_t pacingTick =
        static_cast<std::uint32_t>(ReadScalar(trial::ResolveVa(trial::kVaFramePacingLastTick), 4));
    const std::uint32_t substeps = static_cast<std::uint32_t>(ReadScalar(trial::ResolveVa(trial::kVaFrameSubsteps), 4));
    const std::uint32_t pacingBusy = static_cast<std::uint32_t>(ReadScalar(trial::ResolveVa(trial::kVaFramePacingBusyFlag), 4));
    const std::uint32_t holdFlag = static_cast<std::uint32_t>(ReadScalar(trial::ResolveVa(trial::kVaSubstepHoldFlag), 4));
    const std::uint32_t p1Cpu = static_cast<std::uint32_t>(ReadScalar(trial::ResolveVa(trial::kVaVsHumanFlagP1), 4));
    const std::uint32_t p2Cpu = static_cast<std::uint32_t>(ReadScalar(trial::ResolveVa(trial::kVaVsHumanFlagP2), 4));
    const void* const p2Base = trial::ResolveVa(trial::kVaP2Player);
    const void* const p2MovePtr =
        p2Base != nullptr ? static_cast<const std::uint8_t*>(p2Base) + trial::kOffMoveId : nullptr;
    const std::uint32_t p2Move = static_cast<std::uint32_t>(ReadScalar(p2MovePtr, 4));

    log::Event(
        kClass,
        "headline",
        "[%s] screen=%u sub=%u anim=%d di=%u cam=(%u,%u) p1_move=%u p2_move=%u cpu=(%u,%u) picks=(%u,%u) pace_tick=%u busy=%u hold=%u substeps=%u",
        tag != nullptr ? tag : "?",
        static_cast<unsigned>(screenMode),
        static_cast<unsigned>(subState),
        anim,
        static_cast<unsigned>(diActive),
        static_cast<unsigned>(cameraX),
        static_cast<unsigned>(cameraY),
        static_cast<unsigned>(p1Move),
        static_cast<unsigned>(p2Move),
        static_cast<unsigned>(p1Cpu),
        static_cast<unsigned>(p2Cpu),
        static_cast<unsigned>(p1Pick),
        static_cast<unsigned>(p2Pick),
        static_cast<unsigned>(pacingTick),
        static_cast<unsigned>(pacingBusy),
        static_cast<unsigned>(holdFlag),
        static_cast<unsigned>(substeps));
}

void LogScalarWrite(
    const char* context,
    const std::uintptr_t va,
    const char* fieldName,
    const std::uint64_t before,
    const std::uint64_t after,
    const int widthBytes)
{
    if (!MemlogEnabled()) { return; }
    if (before == after)
    {
        return;
    }

    log::Event(
        kClass,
        "scalar",
        "ctx=%s field=%s va=0x%08X w=%d before=0x%llX after=0x%llX (%llu -> %llu)",
        context != nullptr ? context : "?",
        fieldName != nullptr ? fieldName : "?",
        static_cast<unsigned>(va),
        widthBytes,
        static_cast<unsigned long long>(before),
        static_cast<unsigned long long>(after),
        static_cast<unsigned long long>(before),
        static_cast<unsigned long long>(after));
}

void LogRegionWrite(
    const char* context,
    const char* label,
    void* dst,
    const void* src,
    const std::size_t size)
{
    if (!MemlogEnabled()) { return; }
    if (dst == nullptr || src == nullptr || size == 0u)
    {
        return;
    }

    const std::uint32_t liveHash = Fnv1a32(dst, size);
    const std::uint32_t payloadHash = Fnv1a32(src, size);

    log::Event(
        kClass,
        label != nullptr ? label : "region",
        "ctx=%s dst=0x%p rva=0x%08X size=%zu live_fnv=0x%08X payload_fnv=0x%08X %s",
        context != nullptr ? context : "?",
        dst,
        static_cast<unsigned>(PointerToVa(dst)),
        size,
        liveHash,
        payloadHash,
        liveHash == payloadHash ? "identical" : "DIFFER");

    if (size <= kSmallHexDump)
    {
        char beforeHex[256] = {};
        char afterHex[256] = {};
        AppendHex(beforeHex, sizeof(beforeHex), static_cast<const std::uint8_t*>(dst), size);
        AppendHex(afterHex, sizeof(afterHex), static_cast<const std::uint8_t*>(src), size);
        log::Event(
            kClass,
            label != nullptr ? label : "hex",
            "ctx=%s before=%s after=%s",
            context != nullptr ? context : "?",
            beforeHex,
            afterHex);
    }

    LogWatchpointHits(context, label, dst, src, size);

    const bool verbose = IsMemlogVerbose();
    if (verbose || size <= kLargeRegionThreshold)
    {
        LogDiffSummary(context, label, dst, src, size);
    }
    else
    {
        LogDiffSparse(context, label, dst, src, size, kSparseDiffMax);
        log::Event(
            kClass,
            label != nullptr ? label : "diff",
            "ctx=%s full_diff_skipped size=%zu (set THV_MEMLOG_VERBOSE=1 for full u32 scan)",
            context != nullptr ? context : "?",
            size);
    }
}

void LogPostWriteVerify(
    const char* context,
    const char* label,
    const void* dst,
    const void* expected,
    const std::size_t size)
{
    if (!MemlogEnabled()) { return; }
    if (dst == nullptr || expected == nullptr || size == 0u)
    {
        return;
    }

    const std::uint32_t liveHash = Fnv1a32(dst, size);
    const std::uint32_t expectedHash = Fnv1a32(expected, size);
    if (liveHash != expectedHash)
    {
        log::Event(
            kClass,
            label != nullptr ? label : "verify",
            "ctx=%s VERIFY_FAIL fnv live=0x%08X expected=0x%08X size=%zu",
            context != nullptr ? context : "?",
            liveHash,
            expectedHash,
            size);
        LogDiffSparse(context, label, dst, expected, std::min(size, kLargeRegionThreshold), 4u);
    }
    else
    {
        log::Event(
            kClass,
            label != nullptr ? label : "verify",
            "ctx=%s verify_ok fnv=0x%08X size=%zu",
            context != nullptr ? context : "?",
            liveHash,
            size);
    }

    LogWatchpointMismatches(context, label, "verify_watch", dst, expected, size);
}

void LogBlobHash(const char* context, const char* label, const void* data, const std::size_t size)
{
    if (!MemlogEnabled()) { return; }
    if (data == nullptr || size == 0u)
    {
        return;
    }

    log::Event(
        kClass,
        label != nullptr ? label : "hash",
        "ctx=%s size=%zu fnv=0x%08X",
        context != nullptr ? context : "?",
        size,
        Fnv1a32(data, size));
}

void LogBufferFingerprint(
    const char* context,
    const char* label,
    const void* live,
    const void* payload,
    const std::size_t size)
{
    if (!MemlogEnabled()) { return; }
    if (live == nullptr || payload == nullptr || size == 0u)
    {
        return;
    }

    const std::uint32_t liveHash = Fnv1a32(live, size);
    const std::uint32_t payloadHash = Fnv1a32(payload, size);
    log::Event(
        kClass,
        label != nullptr ? label : "fingerprint",
        "ctx=%s size=%zu live_fnv=0x%08X payload_fnv=0x%08X %s",
        context != nullptr ? context : "?",
        size,
        liveHash,
        payloadHash,
        liveHash == payloadHash ? "match" : "MISMATCH");
}

void LogMemsetWrite(
    const char* context,
    const char* label,
    void* dst,
    const int fillByte,
    const std::size_t size)
{
    if (!MemlogEnabled()) { return; }
    if (dst == nullptr || size == 0u)
    {
        return;
    }

    const auto fill = static_cast<std::uint8_t>(fillByte & 0xFF);
    log::Event(
        kClass,
        label != nullptr ? label : "memset",
        "ctx=%s dst=0x%p rva=0x%08X size=%zu fill=0x%02X",
        context != nullptr ? context : "?",
        dst,
        static_cast<unsigned>(PointerToVa(dst)),
        size,
        static_cast<unsigned>(fill));

    std::uint8_t fillPattern[256] = {};
    const std::size_t patternSize = std::min(size, sizeof(fillPattern));
    std::memset(fillPattern, fill, patternSize);
    LogWatchpointHits(context, label, dst, fillPattern, patternSize);

    if (size <= kSmallHexDump)
    {
        char beforeHex[256] = {};
        char afterHex[256] = {};
        AppendHex(beforeHex, sizeof(beforeHex), static_cast<const std::uint8_t*>(dst), size);
        std::memset(fillPattern, fill, size);
        AppendHex(afterHex, sizeof(afterHex), fillPattern, size);
        log::Event(
            kClass,
            label != nullptr ? label : "memset_hex",
            "ctx=%s before=%s after=%s",
            context != nullptr ? context : "?",
            beforeHex,
            afterHex);
    }
}

}
