#pragma once

#include "determinism.h"
#include "spectator_protocol.h"
#include "trial_addresses.h"
#include "trial_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace thv::spectator
{

struct Checkpoint
{
    std::uint32_t generation = 0u;
    std::uint32_t frame = 0u;
    std::uint32_t simFrame = 0u;
    std::uint32_t rngShadow = 0u;
    std::uint32_t visualRngShadow = 0u;
    std::uint8_t p1CharacterId = 0u;
    std::uint8_t p2CharacterId = 0u;
    std::uint8_t stageId = 0u;
    std::array<std::uint8_t, det::kFpuStateSize> fpu{};
    std::array<std::uint8_t, trial::kAuxStateSize> shakeAux{};
    std::vector<std::uint8_t> battle;
    std::vector<std::uint8_t> visualEffects;
    std::array<std::uint32_t, proto::kFrameHashRegions> regionHashes{};
};

std::uint32_t CheckpointCrc32(const void* bytes, std::size_t size);

// Must match the hashes Engine::SaveInputs computes.
bool ComputeCheckpointRegionHashes(
    const Checkpoint& checkpoint,
    std::array<std::uint32_t, proto::kFrameHashRegions>* outHashes,
    std::string* outError = nullptr);
bool ValidateCheckpoint(const Checkpoint& checkpoint, std::string* outError = nullptr);

bool SerializeCheckpoint(
    const Checkpoint& checkpoint,
    std::vector<std::uint8_t>* outBytes,
    std::string* outError = nullptr);
bool DeserializeCheckpoint(
    const void* bytes,
    std::size_t size,
    Checkpoint* outCheckpoint,
    std::string* outError = nullptr);

bool BuildBootstrapTransfer(
    const Checkpoint& checkpoint,
    std::uint32_t sessionTag,
    std::uint32_t firstSequence,
    std::uint8_t inputDelay,
    std::uint8_t rollbackFrames,
    std::uint8_t targetFps,
    protocol::Bootstrap* outBootstrap,
    std::vector<protocol::BootstrapChunk>* outChunks,
    std::string* outError = nullptr);
bool ReassembleBootstrapTransfer(
    const protocol::Bootstrap& bootstrap,
    const std::vector<protocol::BootstrapChunk>& chunks,
    Checkpoint* outCheckpoint,
    std::string* outError = nullptr);

}
