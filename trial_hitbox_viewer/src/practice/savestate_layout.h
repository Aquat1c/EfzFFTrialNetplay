#pragma once

#include "trial_addresses.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace thv::savestate_layout
{

constexpr std::uint32_t kFileVersionV3 = 3u;
constexpr std::uint32_t kFileVersionV4 = 4u;

constexpr std::size_t kEffectSlotStride = 482u;
constexpr std::size_t kEffectSlotCount = 241u;
constexpr std::size_t kEffectSystemSize = kEffectSlotStride * kEffectSlotCount;

constexpr std::uintptr_t kVaSceneContext = 0x004A3EC8u;
constexpr std::uintptr_t kVaSceneContextEnd = 0x004A3F60u;
constexpr std::size_t kSceneContextSize = kVaSceneContextEnd - kVaSceneContext;

// v4 payload excludes the scene, effects and dispatch tail.
constexpr std::size_t kSnapshotPayloadSizeV4 =
    trial::kBattleSimSize + trial::kMatchAuxSize + 1u + sizeof(std::int32_t);

// v3 legacy payload sizes (monolithic battle_data + scene + effects + peripheral fields).
constexpr std::size_t kLegacyPayloadSizeV3 =
    trial::kBattleStateSize + kSceneContextSize + kEffectSystemSize;
constexpr std::size_t kSnapshotPayloadSizeV3 = kLegacyPayloadSizeV3 + 1u + sizeof(std::int32_t);

struct RegionSpec
{
    const char* name;
    std::uintptr_t va;
    std::size_t size;
};

constexpr RegionSpec kRegionsV4[] = {
    {"battle_sim", trial::kVaBattleStateBegin, trial::kBattleSimSize},
    {"match_aux", trial::kVaMatchAuxBegin, trial::kMatchAuxSize},
    {"win_count", trial::kVaWinCount, 1u},
    {"frame_substeps", trial::kVaFrameSubsteps, sizeof(std::int32_t)},
};

constexpr std::size_t kRegionCountV4 = sizeof(kRegionsV4) / sizeof(kRegionsV4[0]);

constexpr std::size_t kFrameBlobBytes = trial::kFrameBlobStride;

#pragma pack(push, 1)
struct FrameBlobCapture
{
    std::int16_t moveId = 0;
    std::uint8_t subframe = 0;
    std::uint8_t reserved = 0;
    std::uint8_t blob[kFrameBlobBytes] = {};
};

struct RegionEntry
{
    std::uint32_t va = 0;
    std::uint32_t size = 0;
    std::uint32_t payloadOffset = 0;
    char name[16] = {};
};

struct SavestateHeaderV3Fields
{
    std::uint32_t payloadSize = 0;
    std::uint32_t regionCount = 0;
    std::uint32_t payloadCrc32 = 0;
    FrameBlobCapture p1Frame{};
    FrameBlobCapture p2Frame{};
};
#pragma pack(pop)

struct SnapshotPayload
{
    SavestateHeaderV3Fields meta{};
    std::vector<RegionEntry> regions{};
    std::vector<std::uint8_t> bytes{};
};

bool CaptureSnapshot(SnapshotPayload* out);
bool ApplySnapshot(const SnapshotPayload& snapshot);
bool ApplySnapshotBytes(
    const std::uint8_t* payload,
    std::size_t payloadSize,
    const SavestateHeaderV3Fields& meta,
    const RegionEntry* regions,
    std::uint32_t regionCount);

std::size_t ExpectedPayloadSizeV4();
std::size_t ExpectedPayloadSizeV3();
std::size_t LegacyPayloadSizeV3();
bool IsSupportedPayloadSize(std::size_t size);
bool IsLegacyBrokenLayout(const SnapshotPayload& snapshot);

}
