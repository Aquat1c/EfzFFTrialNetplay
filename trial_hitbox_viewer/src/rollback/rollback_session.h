#pragma once

#include "rollback_config.h"
#include "rollback_types.h"

#include <vector>

namespace thv::rollback
{

class Session
{
public:
    void Reset();
    void BeginMatch(std::uint32_t matchStartFrame);
    void BeginMatchFromHandoff(std::uint32_t matchStartFrame);
    void OnMemorialTransition(int newMemorial, int oldMemorial);
    bool TrimForFrontendEpoch(std::uint32_t exitFrame);

    int MapMemorialScreenIndex() const;
    static int MapMemorialScreenIndexFromGame();
    static bool IsPostMatchScreenFromGame();
    RollbackSubState MapTrialScreenSubState() const;
    bool IsTrialBattleScreen() const;

    int SlotIndex(std::uint32_t frame) const;
    RollbackFrameSlot& FrameSlot(std::uint32_t frame);
    const RollbackFrameSlot& FrameSlot(std::uint32_t frame) const;

    std::uint8_t LocalInputAt(std::uint32_t frame) const;
    std::uint8_t RemoteInputAt(std::uint32_t frame) const;
    std::uint8_t LocalSystemStateAt(std::uint32_t frame) const;
    std::uint8_t RemoteSystemStateAt(std::uint32_t frame) const;
    std::uint8_t RemoteInputAtOrHeld(std::uint32_t frame) const;
    // False means the history bound was reached; callers must fail closed.
    bool AppendLocalInput(std::uint8_t wireMask, std::uint8_t systemState = 0);
    bool AppendRemoteInput(std::uint8_t wireMask, std::uint8_t systemState = 0);
    void AppendPredictedInput(std::uint8_t wireMask);

    // Reads history at currentFrame; delay is enforced before inputs enter history.
    std::uint32_t SimInputFrame() const;
    std::uint32_t EffectiveInputFrame(std::uint32_t simFrameNumber) const;
    std::uint8_t WireMaskForPlayerSide(std::uint32_t frame, int playerSide) const;

    std::uint32_t ConfirmedFrameFloor() const;
    bool HasFrameInputs(std::uint32_t frame, bool requireRemoteInput = true) const;
    bool HasCapturedInputs(std::uint32_t frame, bool requireRemoteInput = true) const;
    std::uint32_t WireInputFrame() const;

    int localSide = 0;
    int remoteSide = 1;
    int inputDelay = 0;
    int delayAdjust = 0;
    int configuredDelay = 0;
    int predictionWindow = 0;

    std::uint32_t currentFrame = 0;
    std::uint32_t simFrame = 0;
    std::uint32_t confirmedFrame = 0;
    std::uint32_t syncEnqueueFrame = 0;
    std::uint32_t matchStartFrame = 0;
    std::uint32_t matchId = 0;
    std::uint32_t pingMs = 0;
    std::uint32_t peakAdvanceFrame = 0;
    std::uint32_t peerAckFrame = 0;

    int prevMemorialIndex = -1;
    int memorialIndex = -1;
    RollbackSubState prevSubState = RollbackSubState::Other;
    RollbackSubState rollbackSubState = RollbackSubState::Other;

    std::vector<std::uint8_t> localInputs;
    std::vector<std::uint8_t> remoteInputs;
    std::vector<std::uint8_t> localSystemInputs;
    std::vector<std::uint8_t> remoteSystemInputs;
    // Publication lags capture, so the live screen/generation is not valid later.
    std::vector<std::uint8_t> frameScreens;
    std::vector<std::uint32_t> frameGenerations;
    // The ring can wrap during a correction burst; publication must not depend on its slots.
    std::vector<std::array<std::uint32_t, proto::kFrameHashRegions>> frameRegionHashes;
    std::vector<std::uint8_t> frameHashValid;
    std::vector<std::uint8_t> predictedInputs;

    std::vector<std::uint16_t> delayMapP0;
    std::vector<std::uint16_t> delayMapP1;

    InitSnapshot initSnapshot{};
    std::array<RollbackFrameSlot, kFrameRingSize> frameRing{};

    bool battleHandoffReady = false;
};

}
