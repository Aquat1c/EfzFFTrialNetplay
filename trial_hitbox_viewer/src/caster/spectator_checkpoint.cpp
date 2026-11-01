#include "spectator_checkpoint.h"

#include "debug_log.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace thv::spectator
{
namespace
{

constexpr std::uint32_t kCheckpointMagic = 0x435A4645u; // "EFZC"
constexpr std::uint16_t kCheckpointVersion = trialnet::compat::kCompatibilityVersion;

#pragma pack(push, 1)
struct SerializedCheckpointHeader
{
    std::uint32_t magic = kCheckpointMagic;
    std::uint16_t version = kCheckpointVersion;
    std::uint16_t headerBytes = 0u;
    std::uint32_t generation = 0u;
    std::uint32_t frame = 0u;
    std::uint32_t simFrame = 0u;
    std::uint32_t rngShadow = 0u;
    std::uint32_t visualRngShadow = 0u;
    std::uint32_t battleBytes = 0u;
    std::uint32_t fpuBytes = 0u;
    std::uint32_t shakeAuxBytes = 0u;
    std::uint32_t visualEffectBytes = 0u;
    std::uint32_t payloadBytes = 0u;
    std::uint32_t payloadCrc32 = 0u;
    std::uint8_t p1CharacterId = 0u;
    std::uint8_t p2CharacterId = 0u;
    std::uint8_t stageId = 0u;
    std::uint8_t reserved = 0u;
    std::array<std::uint32_t, proto::kFrameHashRegions> regionHashes{};
};
#pragma pack(pop)

void SetError(std::string* const outError, const char* const message)
{
    if (outError != nullptr)
    {
        *outError = message != nullptr ? message : "spectator checkpoint error";
    }
}

bool ValidProtocolHeader(const protocol::Header& header, const protocol::MessageKind kind)
{
    return header.magic == protocol::kMagic
        && header.version == protocol::kVersion
        && header.kind == kind;
}

}

std::uint32_t CheckpointCrc32(const void* const bytes, const std::size_t size)
{
    if (bytes == nullptr && size != 0u)
    {
        return 0u;
    }
    const auto* const data = static_cast<const std::uint8_t*>(bytes);
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
        {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

bool ComputeCheckpointRegionHashes(
    const Checkpoint& checkpoint,
    std::array<std::uint32_t, proto::kFrameHashRegions>* const outHashes,
    std::string* const outError)
{
    if (outHashes == nullptr)
    {
        SetError(outError, "output hash array is null");
        return false;
    }
    if (checkpoint.battle.size() != trial::kDynamicBattleSize)
    {
        SetError(outError, "dynamic battle payload size mismatch");
        return false;
    }
    if (checkpoint.visualEffects.size() != trial::kEffectDynamicStateSize)
    {
        SetError(outError, "visual effect payload size mismatch");
        return false;
    }

    std::array<std::uint32_t, proto::kFrameHashRegions> hashes{};
    std::size_t offset = 0u;
    for (int region = 0; region < trial::kDynamicRegionCount; ++region)
    {
        const std::size_t regionSize = trial::kDynamicBattleRegions[region].size;
        if (offset + regionSize > checkpoint.battle.size())
        {
            SetError(outError, "dynamic battle region exceeds payload");
            return false;
        }
        hashes[region] = CheckpointCrc32(checkpoint.battle.data() + offset, regionSize);
        offset += regionSize;
    }
    if (offset != checkpoint.battle.size()
        || trial::kDynamicRegionCount + 2 != static_cast<int>(proto::kFrameHashRegions))
    {
        SetError(outError, "rollback region layout mismatch");
        return false;
    }

    std::array<std::uint8_t, sizeof(std::uint16_t) + sizeof(std::uint32_t)> determinism{};
    std::memcpy(determinism.data(), checkpoint.fpu.data(), sizeof(std::uint16_t));
    std::memcpy(
        determinism.data() + sizeof(std::uint16_t),
        &checkpoint.rngShadow,
        sizeof(checkpoint.rngShadow));
    hashes[trial::kDynamicRegionCount] = CheckpointCrc32(determinism.data(), determinism.size());
    hashes[trial::kDynamicRegionCount + 1] =
        CheckpointCrc32(checkpoint.shakeAux.data(), checkpoint.shakeAux.size());
    *outHashes = hashes;
    return true;
}

bool ValidateCheckpoint(const Checkpoint& checkpoint, std::string* const outError)
{
    std::array<std::uint32_t, proto::kFrameHashRegions> computed{};
    if (!ComputeCheckpointRegionHashes(checkpoint, &computed, outError))
    {
        return false;
    }
    if (computed != checkpoint.regionHashes)
    {
        (void)log::EnableFileLogging();
        SetError(outError, "checkpoint region hash mismatch");
        return false;
    }
    return true;
}

bool SerializeCheckpoint(
    const Checkpoint& checkpoint,
    std::vector<std::uint8_t>* const outBytes,
    std::string* const outError)
{
    if (outBytes == nullptr)
    {
        SetError(outError, "output checkpoint buffer is null");
        return false;
    }
    if (!ValidateCheckpoint(checkpoint, outError))
    {
        return false;
    }

    const std::size_t payloadBytes = checkpoint.battle.size()
        + checkpoint.fpu.size()
        + checkpoint.shakeAux.size()
        + checkpoint.visualEffects.size();
    if (payloadBytes > std::numeric_limits<std::uint32_t>::max())
    {
        SetError(outError, "checkpoint payload is too large");
        return false;
    }

    SerializedCheckpointHeader header{};
    header.headerBytes = static_cast<std::uint16_t>(sizeof(header));
    header.generation = checkpoint.generation;
    header.frame = checkpoint.frame;
    header.simFrame = checkpoint.simFrame;
    header.rngShadow = checkpoint.rngShadow;
    header.visualRngShadow = checkpoint.visualRngShadow;
    header.battleBytes = static_cast<std::uint32_t>(checkpoint.battle.size());
    header.fpuBytes = static_cast<std::uint32_t>(checkpoint.fpu.size());
    header.shakeAuxBytes = static_cast<std::uint32_t>(checkpoint.shakeAux.size());
    header.visualEffectBytes = static_cast<std::uint32_t>(checkpoint.visualEffects.size());
    header.payloadBytes = static_cast<std::uint32_t>(payloadBytes);
    header.p1CharacterId = checkpoint.p1CharacterId;
    header.p2CharacterId = checkpoint.p2CharacterId;
    header.stageId = checkpoint.stageId;
    header.regionHashes = checkpoint.regionHashes;

    outBytes->assign(sizeof(header) + payloadBytes, 0u);
    std::size_t offset = sizeof(header);
    auto append = [&](const void* const data, const std::size_t size) {
        std::memcpy(outBytes->data() + offset, data, size);
        offset += size;
    };
    append(checkpoint.battle.data(), checkpoint.battle.size());
    append(checkpoint.fpu.data(), checkpoint.fpu.size());
    append(checkpoint.shakeAux.data(), checkpoint.shakeAux.size());
    append(checkpoint.visualEffects.data(), checkpoint.visualEffects.size());
    header.payloadCrc32 = CheckpointCrc32(outBytes->data() + sizeof(header), payloadBytes);
    std::memcpy(outBytes->data(), &header, sizeof(header));
    return true;
}

bool DeserializeCheckpoint(
    const void* const bytes,
    const std::size_t size,
    Checkpoint* const outCheckpoint,
    std::string* const outError)
{
    if (bytes == nullptr || outCheckpoint == nullptr || size < sizeof(SerializedCheckpointHeader))
    {
        SetError(outError, "checkpoint packet is truncated");
        return false;
    }

    SerializedCheckpointHeader header{};
    std::memcpy(&header, bytes, sizeof(header));
    if (header.magic != kCheckpointMagic || header.version != kCheckpointVersion
        || header.headerBytes != sizeof(header))
    {
        SetError(outError, "checkpoint header is incompatible");
        return false;
    }
    if (header.battleBytes != trial::kDynamicBattleSize
        || header.fpuBytes != det::kFpuStateSize
        || header.shakeAuxBytes != trial::kAuxStateSize
        || header.visualEffectBytes != trial::kEffectDynamicStateSize)
    {
        SetError(outError, "checkpoint component size mismatch");
        return false;
    }

    const std::size_t payloadBytes = static_cast<std::size_t>(header.battleBytes)
        + header.fpuBytes + header.shakeAuxBytes + header.visualEffectBytes;
    if (payloadBytes != header.payloadBytes || sizeof(header) + payloadBytes != size)
    {
        SetError(outError, "checkpoint total size mismatch");
        return false;
    }
    const auto* const raw = static_cast<const std::uint8_t*>(bytes);
    if (CheckpointCrc32(raw + sizeof(header), payloadBytes) != header.payloadCrc32)
    {
        SetError(outError, "checkpoint CRC mismatch");
        return false;
    }

    Checkpoint checkpoint{};
    checkpoint.generation = header.generation;
    checkpoint.frame = header.frame;
    checkpoint.simFrame = header.simFrame;
    checkpoint.rngShadow = header.rngShadow;
    checkpoint.visualRngShadow = header.visualRngShadow;
    checkpoint.p1CharacterId = header.p1CharacterId;
    checkpoint.p2CharacterId = header.p2CharacterId;
    checkpoint.stageId = header.stageId;
    checkpoint.regionHashes = header.regionHashes;
    std::size_t offset = sizeof(header);
    checkpoint.battle.assign(raw + offset, raw + offset + header.battleBytes);
    offset += header.battleBytes;
    std::memcpy(checkpoint.fpu.data(), raw + offset, checkpoint.fpu.size());
    offset += checkpoint.fpu.size();
    std::memcpy(checkpoint.shakeAux.data(), raw + offset, checkpoint.shakeAux.size());
    offset += checkpoint.shakeAux.size();
    checkpoint.visualEffects.assign(raw + offset, raw + offset + header.visualEffectBytes);

    if (!ValidateCheckpoint(checkpoint, outError))
    {
        return false;
    }
    *outCheckpoint = std::move(checkpoint);
    return true;
}

bool BuildBootstrapTransfer(
    const Checkpoint& checkpoint,
    const std::uint32_t sessionTag,
    const std::uint32_t firstSequence,
    const std::uint8_t inputDelay,
    const std::uint8_t rollbackFrames,
    const std::uint8_t targetFps,
    protocol::Bootstrap* const outBootstrap,
    std::vector<protocol::BootstrapChunk>* const outChunks,
    std::string* const outError)
{
    if (outBootstrap == nullptr || outChunks == nullptr)
    {
        SetError(outError, "bootstrap output is null");
        return false;
    }
    if (!protocol::IsSupportedTargetFps(targetFps))
    {
        SetError(outError, "bootstrap target fps is invalid");
        return false;
    }
    std::vector<std::uint8_t> serialized;
    if (!SerializeCheckpoint(checkpoint, &serialized, outError))
    {
        return false;
    }
    const std::size_t chunkCount =
        (serialized.size() + protocol::kBootstrapChunkBytes - 1u) / protocol::kBootstrapChunkBytes;
    if (chunkCount == 0u || chunkCount > std::numeric_limits<std::uint16_t>::max())
    {
        SetError(outError, "bootstrap chunk count is invalid");
        return false;
    }

    protocol::Bootstrap bootstrap{};
    bootstrap.header.sessionTag = sessionTag;
    bootstrap.header.sequence = firstSequence;
    bootstrap.rngShadow = checkpoint.rngShadow;
    bootstrap.generation = checkpoint.generation;
    bootstrap.startFrame = checkpoint.frame;
    bootstrap.simFrame = checkpoint.simFrame;
    bootstrap.checkpointBytes = static_cast<std::uint32_t>(serialized.size());
    bootstrap.checkpointCrc32 = CheckpointCrc32(serialized.data(), serialized.size());
    bootstrap.chunkCount = static_cast<std::uint16_t>(chunkCount);
    bootstrap.inputDelay = inputDelay;
    bootstrap.rollbackFrames = rollbackFrames;
    bootstrap.p1CharacterId = checkpoint.p1CharacterId;
    bootstrap.p2CharacterId = checkpoint.p2CharacterId;
    bootstrap.stageId = checkpoint.stageId;
    bootstrap.targetFps = targetFps;

    outChunks->clear();
    outChunks->reserve(chunkCount);
    for (std::size_t i = 0; i < chunkCount; ++i)
    {
        protocol::BootstrapChunk chunk{};
        chunk.header.sessionTag = sessionTag;
        chunk.header.sequence = firstSequence + 1u + static_cast<std::uint32_t>(i);
        chunk.offset = static_cast<std::uint32_t>(i * protocol::kBootstrapChunkBytes);
        const std::size_t remaining = serialized.size() - chunk.offset;
        const std::size_t payload = std::min(remaining, protocol::kBootstrapChunkBytes);
        chunk.payloadBytes = static_cast<std::uint16_t>(payload);
        std::memcpy(chunk.payload.data(), serialized.data() + chunk.offset, payload);
        outChunks->push_back(chunk);
    }
    *outBootstrap = bootstrap;
    return true;
}

bool ReassembleBootstrapTransfer(
    const protocol::Bootstrap& bootstrap,
    const std::vector<protocol::BootstrapChunk>& chunks,
    Checkpoint* const outCheckpoint,
    std::string* const outError)
{
    if (outCheckpoint == nullptr || !ValidProtocolHeader(bootstrap.header, protocol::MessageKind::Bootstrap)
        || !protocol::IsSupportedTargetFps(bootstrap.targetFps))
    {
        SetError(outError, "bootstrap manifest is invalid");
        return false;
    }
    if (bootstrap.checkpointBytes == 0u || bootstrap.chunkCount == 0u
        || chunks.size() != bootstrap.chunkCount)
    {
        SetError(outError, "bootstrap chunk set is incomplete");
        return false;
    }

    std::vector<std::uint8_t> serialized(bootstrap.checkpointBytes, 0u);
    std::vector<bool> covered(bootstrap.chunkCount, false);
    for (const protocol::BootstrapChunk& chunk : chunks)
    {
        if (!ValidProtocolHeader(chunk.header, protocol::MessageKind::BootstrapChunk)
            || chunk.header.sessionTag != bootstrap.header.sessionTag
            || chunk.payloadBytes == 0u
            || chunk.payloadBytes > chunk.payload.size()
            || chunk.offset >= serialized.size()
            || static_cast<std::size_t>(chunk.offset) + chunk.payloadBytes > serialized.size()
            || chunk.offset % protocol::kBootstrapChunkBytes != 0u)
        {
            SetError(outError, "bootstrap chunk is invalid");
            return false;
        }
        const std::size_t index = chunk.offset / protocol::kBootstrapChunkBytes;
        if (index >= covered.size() || covered[index])
        {
            SetError(outError, "bootstrap chunk overlaps or duplicates data");
            return false;
        }
        const std::size_t expected = std::min<std::size_t>(
            protocol::kBootstrapChunkBytes,
            serialized.size() - chunk.offset);
        if (chunk.payloadBytes != expected)
        {
            SetError(outError, "bootstrap chunk length is not canonical");
            return false;
        }
        std::memcpy(serialized.data() + chunk.offset, chunk.payload.data(), chunk.payloadBytes);
        covered[index] = true;
    }
    if (std::find(covered.begin(), covered.end(), false) != covered.end()
        || CheckpointCrc32(serialized.data(), serialized.size()) != bootstrap.checkpointCrc32)
    {
        SetError(outError, "bootstrap transfer CRC or coverage mismatch");
        return false;
    }
    if (!DeserializeCheckpoint(serialized.data(), serialized.size(), outCheckpoint, outError))
    {
        return false;
    }
    if (outCheckpoint->generation != bootstrap.generation
        || outCheckpoint->frame != bootstrap.startFrame
        || outCheckpoint->simFrame != bootstrap.simFrame
        || outCheckpoint->p1CharacterId != bootstrap.p1CharacterId
        || outCheckpoint->p2CharacterId != bootstrap.p2CharacterId
        || outCheckpoint->stageId != bootstrap.stageId)
    {
        SetError(outError, "bootstrap manifest does not match checkpoint");
        return false;
    }
    return true;
}

}
