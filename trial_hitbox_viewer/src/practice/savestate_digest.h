#pragma once

#include <cstddef>
#include <cstdint>

namespace thv::savestate_layout
{
struct SnapshotPayload;
}

namespace thv::savestate_digest
{

struct PlayerDigest
{
    std::uint8_t charId = 0;
    std::int16_t moveId = 0;
    std::int32_t hp = 0;
    double eForce = 0.0;
    std::int16_t sp = 0;
    std::int8_t facing = 0;
    std::int8_t cpuFlag = 0;
    double posX = 0.0;
    double posY = 0.0;
    std::int32_t subframe = 0;
    int projectileAlive = 0;
};

struct SavestateDigest
{
    PlayerDigest p1{};
    PlayerDigest p2{};
    std::int32_t cameraX = 0;
    std::int32_t cameraY = 0;
    std::uint8_t screenMode = 0;
    std::uint32_t screenSub = 0;
    std::int32_t screenAnim = 0;
    std::int16_t comboHits[2] = {};
    std::int32_t comboDamage[2] = {};
    std::uint8_t winCount = 0;
    std::int32_t frameSubsteps = 0;
    std::uint32_t blobCrc32 = 0;
    bool fromBlob = false;
};

void LogCaptureContractOnce();

SavestateDigest CaptureLiveDigest();
SavestateDigest CaptureBlobDigest(const std::uint8_t* blob, std::size_t size);

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size);

void LogDigestSummary(const char* tag, const SavestateDigest& digest);
void LogDigestDetail(const char* tag, const SavestateDigest& digest);
void WriteDigestArtifact(const char* tag, const SavestateDigest& digest, const char* extraLines);
void LogDigestDiff(const char* tag, const SavestateDigest& before, const SavestateDigest& after);

void ProbeWritableRegion();

void WriteFullSavestateDump(
    const char* tag,
    const savestate_layout::SnapshotPayload& snapshot,
    const std::uint8_t* fpu,
    std::size_t fpuSize,
    const char* extraLines);
void WriteFullBattleDump(const char* tag, const std::uint8_t* battleBlob, std::size_t battleSize, const char* extraLines);

}
