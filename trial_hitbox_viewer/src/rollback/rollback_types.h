#pragma once

#include "rollback_config.h"

#include "determinism.h"
#include "trial_addresses.h"
#include "trial_protocol.h"

#include <array>
#include <cstdint>
#include <vector>

namespace thv::rollback
{

// Normalized index, not the raw screen byte at 0xCCDBA4.
enum class RollbackSubState : int
{
    InitCatchUp = 0,
    MenuIdle = 1,
    PreRoll = 2,
    Battle = 3,
    Other = -1,
};

enum class TransportPhase : std::uint8_t
{
    Idle = 0,
    WaitingRemote = 1,
    Predicting = 2,
    BatchCatchUp = 3,
    Reloading = 4,
};

struct RollbackFrameSlot
{
    std::uint32_t frame = 0;
    std::uint32_t simFrame = 0;
    std::uint8_t localInput = 0;
    std::uint8_t remoteInput = 0;
    std::uint8_t localSystemState = 0;
    std::uint8_t remoteSystemState = 0;
    std::uint8_t screen = 0xFFu;
    std::uint32_t sourceGeneration = 0u;
    std::uint32_t rngShadow = 0;
    std::uint32_t visualRngShadow = 0;
    std::array<std::uint8_t, det::kFpuStateSize> fpu{};
    std::array<std::uint8_t, trial::kAuxStateSize> shakeAux{};
    std::array<std::uint8_t, trial::kEffectDynamicStateSize> visualEffects{};
    std::vector<std::uint8_t> battle;
    bool occupied = false;
    // Per-region CRCs from this frame's last save; a re-save mismatch = incomplete snapshot.
    std::array<std::uint32_t, proto::kFrameHashRegions> regionHashes{};
    bool hashed = false;
};

struct InitSnapshot
{
    std::uint8_t localSide = 0;
    std::uint8_t remoteSide = 1;
    std::uint8_t inputDelay = 0;
    std::uint8_t configuredDelay = 0;
    std::uint32_t rngSeed = 0;
    std::uint32_t matchId = 0;
};

struct Status
{
    bool compiledIn = kCompiledIn;
    bool enabled = false;
    bool active = false;
    bool battleScreen = false;
    RollbackSubState subState = RollbackSubState::Other;
    TransportPhase phase = TransportPhase::Idle;

    int localSide = 0;
    int remoteSide = 1;
    int inputDelay = 0;
    int delayAdjust = 0;

    std::uint32_t currentFrame = 0;
    std::uint32_t simFrame = 0;
    std::uint32_t confirmedFrame = 0;
    std::uint32_t matchStartFrame = 0;

    std::size_t localInputCount = 0;
    std::size_t remoteInputCount = 0;
    std::uint32_t peerAckFrame = 0;
    std::size_t predictedInputCount = 0;
    std::size_t occupiedRingSlots = 0;

    std::uint32_t lastBatchFrames = 0;
    std::uint32_t lastReloadFrame = 0;
    std::uint32_t rollbacksTotal = 0;
    std::uint32_t mispredictsTotal = 0;
};

}
