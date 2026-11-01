#include "savestate_layout.h"

#include "debug_log.h"
#include "lifecycle_log.h"
#include "rollback_memory.h"
#include "savestate_digest.h"
#include "mem_write_log.h"

#include <cstdio>
#include <cstring>

namespace thv::savestate_layout
{
namespace
{

void AppendRegion(
    SnapshotPayload* snapshot,
    const char* name,
    const std::uintptr_t va,
    const std::size_t size)
{
    if (snapshot == nullptr || name == nullptr || size == 0)
    {
        return;
    }

    RegionEntry entry = {};
    entry.va = static_cast<std::uint32_t>(va);
    entry.size = static_cast<std::uint32_t>(size);
    entry.payloadOffset = static_cast<std::uint32_t>(snapshot->bytes.size());
    std::snprintf(entry.name, sizeof(entry.name), "%s", name);

    const void* const src = trial::ResolveVa(va);
    if (src == nullptr)
    {
        log::Error("savestate.layout", "capture failed: unresolved VA 0x%08X (%s)", entry.va, name);
        return;
    }

    const auto* const bytes = static_cast<const std::uint8_t*>(src);
    snapshot->bytes.insert(snapshot->bytes.end(), bytes, bytes + size);
    snapshot->regions.push_back(entry);
}

void CaptureFrameMeta(SnapshotPayload* out)
{
    if (out == nullptr)
    {
        return;
    }

    const auto captureMeta = [](const int side, FrameBlobCapture* frame) {
        const std::uintptr_t playerVa = (side == 0) ? trial::kVaP1Player : trial::kVaP2Player;
        const auto* const player = static_cast<const std::uint8_t*>(trial::ResolveVa(playerVa));
        if (player == nullptr || frame == nullptr)
        {
            return;
        }

        std::memcpy(&frame->moveId, player + trial::kOffMoveId, sizeof(frame->moveId));
        frame->subframe = player[trial::kOffSubframe];
    };

    captureMeta(0, &out->meta.p1Frame);
    captureMeta(1, &out->meta.p2Frame);
}

const RegionEntry* FindRegion(const SnapshotPayload& snapshot, const char* name)
{
    if (name == nullptr)
    {
        return nullptr;
    }

    for (const RegionEntry& region : snapshot.regions)
    {
        if (std::strncmp(region.name, name, sizeof(region.name)) == 0)
        {
            return &region;
        }
    }
    return nullptr;
}

bool ApplyRegionBytes(const RegionEntry& region, const std::uint8_t* payload, const std::size_t payloadSize)
{
    void* const dst = trial::ResolveVa(region.va);
    if (dst == nullptr)
    {
        log::Error("savestate.layout", "apply failed: unresolved VA 0x%08X (%s)", region.va, region.name);
        return false;
    }

    if (region.payloadOffset + region.size > payloadSize)
    {
        log::Error("savestate.layout", "apply failed: region %s out of payload bounds", region.name);
        return false;
    }

    char context[96] = {};
    std::snprintf(context, sizeof(context), "savestate:%s", region.name);
    if (!rollback::memory::WriteRegion(dst, payload + region.payloadOffset, region.size, context, region.name))
    {
        lifecycle::Rollback("savestate", "apply failed write region %s", region.name);
        log::Error("savestate.layout", "apply failed: write region %s", region.name);
        return false;
    }

    return true;
}

bool ApplyLegacyBattleDataRegion(const RegionEntry& region, const std::uint8_t* payload, const std::size_t payloadSize)
{
    if (region.size != trial::kBattleStateSize)
    {
        return ApplyRegionBytes(region, payload, payloadSize);
    }

    RegionEntry sim = {};
    sim.va = static_cast<std::uint32_t>(trial::kVaBattleStateBegin);
    sim.size = static_cast<std::uint32_t>(trial::kBattleSimSize);
    sim.payloadOffset = region.payloadOffset;

    RegionEntry aux = {};
    aux.va = static_cast<std::uint32_t>(trial::kVaMatchAuxBegin);
    aux.size = static_cast<std::uint32_t>(trial::kMatchAuxSize);
    aux.payloadOffset = region.payloadOffset + static_cast<std::uint32_t>(trial::kBattleSimSize);

    log::Step(
        "savestate.layout",
        "legacy battle_data selective apply sim=%u aux=%u skip dispatch=%zu",
        sim.size,
        aux.size,
        trial::kScreenDispatchSize);

    return ApplyRegionBytes(sim, payload, payloadSize) && ApplyRegionBytes(aux, payload, payloadSize);
}

bool ApplyRequiredRegions(const SnapshotPayload& snapshot)
{
    const RegionEntry* const battleSim = FindRegion(snapshot, "battle_sim");
    const RegionEntry* const battleData = FindRegion(snapshot, "battle_data");
    const RegionEntry* const matchAux = FindRegion(snapshot, "match_aux");
    const RegionEntry* const scene = FindRegion(snapshot, "scene_ctx");
    const RegionEntry* const effects = FindRegion(snapshot, "effects");

    if (battleSim != nullptr)
    {
        if (!ApplyRegionBytes(*battleSim, snapshot.bytes.data(), snapshot.bytes.size()))
        {
            return false;
        }
    }
    else if (battleData != nullptr)
    {
        if (!ApplyLegacyBattleDataRegion(*battleData, snapshot.bytes.data(), snapshot.bytes.size()))
        {
            return false;
        }
    }
    else
    {
        lifecycle::Rollback("savestate", "apply failed missing battle_sim/battle_data");
        log::Error("savestate.layout", "apply failed: missing battle_sim/battle_data region");
        return false;
    }

    if (matchAux != nullptr)
    {
        log::Warn(
            "savestate.layout",
            "skipping match_aux restore (%u bytes) — alice-style sim-only apply",
            matchAux->size);
    }

    if (scene != nullptr)
    {
        log::Warn("savestate.layout", "skipping scene_ctx restore (%u bytes) — live screen pointers kept", scene->size);
    }
    if (effects != nullptr)
    {
        log::Warn("savestate.layout", "skipping effects restore (%u bytes) — live effect pool kept", effects->size);
    }

    static const char* kOptional[] = {"win_count", "frame_substeps"};
    for (const char* name : kOptional)
    {
        const RegionEntry* const region = FindRegion(snapshot, name);
        if (region != nullptr && !ApplyRegionBytes(*region, snapshot.bytes.data(), snapshot.bytes.size()))
        {
            return false;
        }
    }

    return true;
}

}

bool IsLegacyBrokenLayout(const SnapshotPayload& snapshot)
{
    for (const RegionEntry& region : snapshot.regions)
    {
        if (std::strncmp(region.name, "p1_core", sizeof(region.name)) == 0
            || std::strncmp(region.name, "p1_tail", sizeof(region.name)) == 0)
        {
            return true;
        }
    }
    return false;
}

bool CaptureSnapshot(SnapshotPayload* out)
{
    if (out == nullptr)
    {
        return false;
    }

    out->regions.clear();
    out->bytes.clear();
    out->meta = {};

    for (const RegionSpec& region : kRegionsV4)
    {
        AppendRegion(out, region.name, region.va, region.size);
    }

    CaptureFrameMeta(out);

    out->meta.regionCount = static_cast<std::uint32_t>(out->regions.size());
    out->meta.payloadSize = static_cast<std::uint32_t>(out->bytes.size());
    out->meta.payloadCrc32 = savestate_digest::Crc32(out->bytes.data(), out->bytes.size());
    return out->regions.size() >= 2u && out->meta.payloadSize == ExpectedPayloadSizeV4();
}

bool ApplySnapshot(const SnapshotPayload& snapshot)
{
    if (IsLegacyBrokenLayout(snapshot))
    {
        log::Error(
            "savestate.layout",
            "apply rejected: obsolete v3 layout (p1_core/p1_tail) — delete debug.thvs and re-save with F6");
        return false;
    }

    if (snapshot.meta.payloadSize != snapshot.bytes.size())
    {
        log::Error(
            "savestate.layout",
            "apply rejected: payload size mismatch meta=%u bytes=%zu",
            snapshot.meta.payloadSize,
            snapshot.bytes.size());
        return false;
    }

    const std::uint32_t crc = savestate_digest::Crc32(snapshot.bytes.data(), snapshot.bytes.size());
    if (crc != snapshot.meta.payloadCrc32)
    {
        log::Error(
            "savestate.layout",
            "apply rejected: payload CRC mismatch file=0x%08X live=0x%08X",
            snapshot.meta.payloadCrc32,
            crc);
        return false;
    }

    memlog::LogGameHeadline("before_savestate_apply");
    if (!ApplyRequiredRegions(snapshot))
    {
        return false;
    }
    memlog::LogGameHeadline("after_savestate_apply");

    log::Info(
        "savestate.layout",
        "apply ok payload=%u regions=%zu p1Frame move=%d sub=%u p2Frame move=%d sub=%u",
        snapshot.meta.payloadSize,
        snapshot.regions.size(),
        snapshot.meta.p1Frame.moveId,
        snapshot.meta.p1Frame.subframe,
        snapshot.meta.p2Frame.moveId,
        snapshot.meta.p2Frame.subframe);

    return true;
}

bool ApplySnapshotBytes(
    const std::uint8_t* payload,
    const std::size_t payloadSize,
    const SavestateHeaderV3Fields& meta,
    const RegionEntry* regions,
    const std::uint32_t regionCount)
{
    SnapshotPayload snapshot = {};
    snapshot.meta = meta;
    snapshot.bytes.assign(payload, payload + payloadSize);
    if (regions != nullptr && regionCount > 0)
    {
        snapshot.regions.assign(regions, regions + regionCount);
    }
    return ApplySnapshot(snapshot);
}

std::size_t ExpectedPayloadSizeV4()
{
    return kSnapshotPayloadSizeV4;
}

std::size_t ExpectedPayloadSizeV3()
{
    return kSnapshotPayloadSizeV3;
}

std::size_t LegacyPayloadSizeV3()
{
    return kLegacyPayloadSizeV3;
}

bool IsSupportedPayloadSize(const std::size_t size)
{
    return size == kSnapshotPayloadSizeV4 || size == kSnapshotPayloadSizeV3 || size == kLegacyPayloadSizeV3;
}

}
