#include "rollback_session.h"

#include "debug_log.h"
#include "lifecycle_log.h"
#include "trial_addresses.h"

#include <algorithm>

namespace thv::rollback
{

void Session::Reset()
{
    localSide = 0;
    remoteSide = 1;
    inputDelay = 0;
    delayAdjust = 0;
    configuredDelay = 0;
    predictionWindow = 0;

    currentFrame = 0;
    simFrame = 0;
    confirmedFrame = 0;
    syncEnqueueFrame = 0;
    matchStartFrame = 0;
    matchId = 0;
    peakAdvanceFrame = 0;
    peerAckFrame = 0;

    prevMemorialIndex = -1;
    memorialIndex = -1;
    prevSubState = RollbackSubState::Other;
    rollbackSubState = RollbackSubState::Other;

    localInputs.clear();
    remoteInputs.clear();
    localSystemInputs.clear();
    remoteSystemInputs.clear();
    frameScreens.clear();
    frameGenerations.clear();
    frameRegionHashes.clear();
    frameHashValid.clear();
    predictedInputs.clear();
    delayMapP0.clear();
    delayMapP1.clear();

    initSnapshot = {};
    for (auto& slot : frameRing)
    {
        slot = {};
    }
    battleHandoffReady = false;
    lifecycle::Rollback("session_reset", "Session::Reset");
    log::Step("rollback", "Session::Reset");
}

void Session::BeginMatch(std::uint32_t startFrame)
{
    matchStartFrame = startFrame;
    currentFrame = startFrame;
    simFrame = startFrame;
    confirmedFrame = startFrame;
    syncEnqueueFrame = startFrame;
    localInputs.clear();
    remoteInputs.clear();
    localSystemInputs.clear();
    remoteSystemInputs.clear();
    frameScreens.clear();
    frameGenerations.clear();
    frameRegionHashes.clear();
    frameHashValid.clear();
    predictedInputs.clear();
    delayMapP0.clear();
    delayMapP1.clear();
    battleHandoffReady = false;
    for (auto& slot : frameRing)
    {
        slot = {};
    }
    lifecycle::Rollback("begin_match", "Session::BeginMatch startFrame=%u", startFrame);
    log::Step("rollback", "Session::BeginMatch startFrame=%u", startFrame);
}

void Session::BeginMatchFromHandoff(std::uint32_t startFrame)
{
    matchStartFrame = startFrame;
    currentFrame = startFrame;
    simFrame = startFrame;
    confirmedFrame = startFrame;
    syncEnqueueFrame = startFrame;
    delayAdjust = predictionWindow;
    predictedInputs.clear();
    delayMapP0.clear();
    delayMapP1.clear();
    battleHandoffReady = true;
    for (auto& slot : frameRing)
    {
        slot = {};
    }
    lifecycle::Rollback(
        "begin_match_handoff",
        "Session::BeginMatchFromHandoff startFrame=%u local=%zu remote=%zu delayAdjust=%d",
        startFrame,
        localInputs.size(),
        remoteInputs.size(),
        delayAdjust);
    log::Step(
        "rollback",
        "Session::BeginMatchFromHandoff startFrame=%u local=%zu remote=%zu delayAdjust=%d",
        startFrame,
        localInputs.size(),
        remoteInputs.size(),
        delayAdjust);
}

void Session::OnMemorialTransition(const int newMemorial, const int oldMemorial)
{
    (void)oldMemorial;

    simFrame = 0;
    delayAdjust = 0;
    battleHandoffReady = false;
    peakAdvanceFrame = 0;
    predictedInputs.clear();
    delayMapP0.clear();
    delayMapP1.clear();

    for (auto& slot : frameRing)
    {
        slot = {};
    }

    if (newMemorial == 2)
    {
        ++matchId;
    }

    lifecycle::Rollback(
        "memorial_transition",
        "Session::OnMemorialTransition %d -> %d frame=%u local=%zu remote=%zu matchId=%u transient=clear",
        oldMemorial,
        newMemorial,
        currentFrame,
        localInputs.size(),
        remoteInputs.size(),
        matchId);
    log::Step(
        "rollback",
        "Session::OnMemorialTransition %d -> %d frame=%u local=%zu remote=%zu matchId=%u transient=clear",
        oldMemorial,
        newMemorial,
        currentFrame,
        localInputs.size(),
        remoteInputs.size(),
        matchId);
}

bool Session::TrimForFrontendEpoch(const std::uint32_t exitFrame)
{
    // Battle exit only on a fully confirmed frame; never pad fabricated input.
    if (localInputs.size() < exitFrame || remoteInputs.size() < exitFrame
        || localSystemInputs.size() < exitFrame || remoteSystemInputs.size() < exitFrame)
    {
        lifecycle::Fault(
            "rollback",
            "frontend epoch trim unavailable frame=%u local=%zu/%zu remote=%zu/%zu",
            exitFrame,
            localInputs.size(),
            localSystemInputs.size(),
            remoteInputs.size(),
            remoteSystemInputs.size());
        log::Error(
            "rollback",
            "frontend epoch trim unavailable frame=%u local=%zu/%zu remote=%zu/%zu",
            exitFrame,
            localInputs.size(),
            localSystemInputs.size(),
            remoteInputs.size(),
            remoteSystemInputs.size());
        return false;
    }

    const std::size_t oldLocal = localInputs.size();
    const std::size_t oldRemote = remoteInputs.size();
    localInputs.resize(exitFrame);
    remoteInputs.resize(exitFrame);
    localSystemInputs.resize(exitFrame);
    remoteSystemInputs.resize(exitFrame);
    if (frameScreens.size() > exitFrame)
    {
        frameScreens.resize(exitFrame);
    }
    if (frameGenerations.size() > exitFrame)
    {
        frameGenerations.resize(exitFrame);
    }
    if (frameRegionHashes.size() > exitFrame)
    {
        frameRegionHashes.resize(exitFrame);
    }
    if (frameHashValid.size() > exitFrame)
    {
        frameHashValid.resize(exitFrame);
    }
    peerAckFrame = std::min(peerAckFrame, exitFrame);
    confirmedFrame = exitFrame;
    syncEnqueueFrame = exitFrame;
    predictedInputs.clear();
    delayMapP0.clear();
    delayMapP1.clear();

    lifecycle::Sync(
        "rollback",
        "frontend epoch trim frame=%u local=%zu->%zu remote=%zu->%zu ack=%u",
        exitFrame,
        oldLocal,
        localInputs.size(),
        oldRemote,
        remoteInputs.size(),
        peerAckFrame);
    log::Step(
        "rollback",
        "frontend epoch trim frame=%u local=%zu->%zu remote=%zu->%zu ack=%u",
        exitFrame,
        oldLocal,
        localInputs.size(),
        oldRemote,
        remoteInputs.size(),
        peerAckFrame);
    return true;
}

namespace
{

bool IsPostMatchLoadingScreen()
{
    const auto* const mode = static_cast<const std::uint8_t*>(trial::ResolveVa(trial::kVaBattleScreenMode));
    if (mode == nullptr || *mode != trial::kScreenLoading)
    {
        return false;
    }

    const auto* const p1Move = static_cast<const std::int16_t*>(trial::ResolveVa(trial::kVaP1MoveId));
    const auto* const p2Move = static_cast<const std::int16_t*>(trial::ResolveVa(trial::kVaP2Player + trial::kOffMoveId));
    if (p1Move != nullptr
        && (*p1Move == trial::kWinScreenMoveIdA || *p1Move == trial::kWinScreenMoveIdB))
    {
        return true;
    }
    if (p2Move != nullptr
        && (*p2Move == trial::kWinScreenMoveIdA || *p2Move == trial::kWinScreenMoveIdB))
    {
        return true;
    }

    const auto* const sub = static_cast<const int*>(trial::ResolveVa(trial::kVaScreenSubState));
    return sub != nullptr && *sub >= trial::kPostMatchSubStateMin && *sub <= trial::kPostMatchSubStateMax;
}

}

int Session::MapMemorialScreenIndex() const
{
    return MapMemorialScreenIndexFromGame();
}

bool Session::IsPostMatchScreenFromGame()
{
    return IsPostMatchLoadingScreen();
}

int Session::MapMemorialScreenIndexFromGame()
{
    const auto* const mode = static_cast<const std::uint8_t*>(trial::ResolveVa(trial::kVaBattleScreenMode));
    if (mode == nullptr)
    {
        return -1;
    }

    switch (*mode)
    {
    case trial::kScreenTitle:
    case trial::kScreenMainMenu:
        return 0;
    case trial::kScreenCharSelect:
        return 1;
    case trial::kScreenLoading:
        return IsPostMatchLoadingScreen() ? -1 : 2;
    case trial::kScreenBattle:
        return 3;
    default:
        return -1;
    }
}

RollbackSubState Session::MapTrialScreenSubState() const
{
    const int memorial = MapMemorialScreenIndex();
    if (memorial >= 0 && memorial <= 3)
    {
        return static_cast<RollbackSubState>(memorial);
    }
    return RollbackSubState::Other;
}

bool Session::IsTrialBattleScreen() const
{
    return MapTrialScreenSubState() == RollbackSubState::Battle;
}

int Session::SlotIndex(std::uint32_t frame) const
{
    return static_cast<int>(frame % static_cast<std::uint32_t>(kFrameRingSize));
}

RollbackFrameSlot& Session::FrameSlot(std::uint32_t frame)
{
    return frameRing[static_cast<std::size_t>(SlotIndex(frame))];
}

const RollbackFrameSlot& Session::FrameSlot(std::uint32_t frame) const
{
    return frameRing[static_cast<std::size_t>(SlotIndex(frame))];
}

std::uint8_t Session::LocalInputAt(std::uint32_t frame) const
{
    return frame < localInputs.size() ? localInputs[frame] : 0u;
}

std::uint8_t Session::RemoteInputAt(std::uint32_t frame) const
{
    return frame < remoteInputs.size() ? remoteInputs[frame] : 0u;
}

std::uint8_t Session::LocalSystemStateAt(std::uint32_t frame) const
{
    return frame < localSystemInputs.size() ? localSystemInputs[frame] : 0u;
}

std::uint8_t Session::RemoteSystemStateAt(std::uint32_t frame) const
{
    return frame < remoteSystemInputs.size() ? remoteSystemInputs[frame] : 0u;
}

std::uint8_t Session::RemoteInputAtOrHeld(std::uint32_t frame) const
{
    if (frame < remoteInputs.size())
    {
        return remoteInputs[frame];
    }
    return remoteInputs.empty() ? 0u : remoteInputs.back();
}

bool Session::AppendLocalInput(std::uint8_t wireMask, std::uint8_t systemState)
{
    if (localInputs.size() >= static_cast<std::size_t>(kMaxInputHistory))
    {
        static bool s_warned = false;
        if (!s_warned)
        {
            s_warned = true;
            lifecycle::Fault("rollback", "local input history hit kMaxInputHistory=%d — sim will wedge", kMaxInputHistory);
            log::Error("rollback", "local input history hit kMaxInputHistory=%d — sim will wedge", kMaxInputHistory);
        }
        return false;
    }
    const std::size_t frame = localInputs.size();
    localInputs.push_back(wireMask);
    localSystemInputs.push_back(systemState);
    if (systemState != 0u)
    {
        lifecycle::Input(
            "rollback_system",
            "local append frame=%zu sim=%u state=0x%02X memorial=%d",
            frame,
            currentFrame,
            static_cast<unsigned>(systemState),
            MapMemorialScreenIndex());
    }
    return true;
}

bool Session::AppendRemoteInput(std::uint8_t wireMask, std::uint8_t systemState)
{
    if (remoteInputs.size() >= static_cast<std::size_t>(kMaxInputHistory))
    {
        static bool s_warned = false;
        if (!s_warned)
        {
            s_warned = true;
            lifecycle::Fault("rollback", "remote input history hit kMaxInputHistory=%d — sim will wedge", kMaxInputHistory);
            log::Error("rollback", "remote input history hit kMaxInputHistory=%d — sim will wedge", kMaxInputHistory);
        }
        return false;
    }
    const std::size_t frame = remoteInputs.size();
    remoteInputs.push_back(wireMask);
    remoteSystemInputs.push_back(systemState);
    if (systemState != 0u)
    {
        lifecycle::Input(
            "rollback_system",
            "remote append frame=%zu sim=%u state=0x%02X memorial=%d",
            frame,
            currentFrame,
            static_cast<unsigned>(systemState),
            MapMemorialScreenIndex());
    }
    return true;
}

void Session::AppendPredictedInput(std::uint8_t wireMask)
{
    if (predictedInputs.size() >= static_cast<std::size_t>(kMaxInputHistory))
    {
        return;
    }
    predictedInputs.push_back(wireMask);
}

std::uint32_t Session::SimInputFrame() const
{
    return currentFrame;
}

std::uint32_t Session::EffectiveInputFrame(std::uint32_t simFrameNumber) const
{
    const int delay = inputDelay + delayAdjust;
    if (delay <= 0)
    {
        return simFrameNumber;
    }

    const auto delayU = static_cast<std::uint32_t>(delay);
    return simFrameNumber > delayU ? simFrameNumber - delayU : matchStartFrame;
}

std::uint8_t Session::WireMaskForPlayerSide(std::uint32_t frame, int playerSide) const
{
    const std::uint8_t local = LocalInputAt(frame);
    const std::uint8_t remote = RemoteInputAt(frame);
    if (playerSide == localSide)
    {
        return local;
    }
    if (playerSide == remoteSide)
    {
        return remote;
    }
    return 0u;
}

std::uint32_t Session::ConfirmedFrameFloor() const
{
    const std::uint32_t bound = confirmedFrame > 0u ? confirmedFrame - 1u : 0u;
    return matchStartFrame > bound ? matchStartFrame : bound;
}

bool Session::HasFrameInputs(const std::uint32_t frame, const bool requireRemoteInput) const
{
    if (frame != confirmedFrame || frame >= localInputs.size())
    {
        return false;
    }

    if (requireRemoteInput && frame >= remoteInputs.size())
    {
        return false;
    }

    return true;
}

bool Session::HasCapturedInputs(const std::uint32_t frame, const bool requireRemoteInput) const
{
    if (frame >= localInputs.size())
    {
        return false;
    }

    if (requireRemoteInput && frame >= remoteInputs.size())
    {
        return false;
    }

    return true;
}

std::uint32_t Session::WireInputFrame() const
{
    if (currentFrame < localInputs.size())
    {
        return currentFrame;
    }

    if (!localInputs.empty())
    {
        return static_cast<std::uint32_t>(localInputs.size() - 1u);
    }

    return 0u;
}

}
