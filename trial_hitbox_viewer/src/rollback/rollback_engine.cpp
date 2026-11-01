#include "rollback_engine.h"
#include "spectator_relay.h"

#include "caster_input.h"
#include "control_config.h"
#include "control_swap.h"
#include "debug_log.h"
#include "desync_check.h"
#include "determinism.h"
#include "game_pacing.h"
#include "lifecycle_log.h"
#include "menu_nav.h"
#include "netplay_sync.h"
#include "render_gate.h"
#include "rollback.h"
#include "rollback_memory.h"
#include "savestate_digest.h"
#include "sdl_input.h"
#include "mem_write_log.h"
#include "netplay_delay.h"
#include "rng.h"
#include "test_harness.h"
#include "trial_addresses.h"
#include "trial_protocol.h"
#include "watchdog.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

namespace thv::rollback
{
namespace
{

static_assert(
    1 + static_cast<int>(proto::kMaxInputDelay)
        + static_cast<int>(proto::kMaxRollbackFrames) + 1
        < proto::kMaxQueueSlot,
    "every supported delay + rollback combination must fit the retransmit window");

std::uint64_t PerformanceNow()
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

std::uint64_t PerformanceElapsedUs(const std::uint64_t start)
{
    static const std::uint64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<std::uint64_t>(value.QuadPart > 0 ? value.QuadPart : 1);
    }();
    const std::uint64_t now = PerformanceNow();
    return now >= start ? ((now - start) * 1000000ull) / frequency : 0ull;
}

class ScopedPerformanceTimer
{
public:
    ScopedPerformanceTimer(
        std::uint64_t* calls,
        std::uint64_t* totalUs,
        std::uint64_t* maxUs)
        : calls_(calls), totalUs_(totalUs), maxUs_(maxUs), start_(PerformanceNow())
    {
    }

    ~ScopedPerformanceTimer()
    {
        const std::uint64_t elapsedUs = PerformanceElapsedUs(start_);
        if (calls_ != nullptr)
        {
            ++*calls_;
        }
        if (totalUs_ != nullptr)
        {
            *totalUs_ += elapsedUs;
        }
        if (maxUs_ != nullptr)
        {
            *maxUs_ = std::max(*maxUs_, elapsedUs);
        }
    }

private:
    std::uint64_t* calls_;
    std::uint64_t* totalUs_;
    std::uint64_t* maxUs_;
    std::uint64_t start_;
};

}

void Engine::Reset()
{
    session_.Reset();
    phase_ = TransportPhase::Idle;
    lastBatchFrames_ = 0;
    lastReloadFrame_ = 0;
    rollbacksTotal_ = 0;
    mispredictsTotal_ = 0;
    pendingRollbackFrame_ = kNoFrame;
    rollbackReplayTarget_ = 0;
    transitionBlockedFrame_ = kNoFrame;
    nextHashFrame_ = 0;
    nextSpectatorFrame_ = 0;
    gameplayEscTransitionActive_ = false;
    localEscDown_ = false;
    remoteEscDown_ = false;
    pendingLocalEsc_ = false;
    pendingRemoteEsc_ = false;
    pendingLocalEscFrame_ = kNoFrame;
    pendingRemoteEscFrame_ = kNoFrame;
    increaseDelayKeyDown_ = false;
    decreaseDelayKeyDown_ = false;
    perf_ = {};
    ResetRollbackAudioDedup();
}

void Engine::SetHostCallbacks(const HostCallbacks& callbacks)
{
    host_ = callbacks;
}

const Session& Engine::GetSession() const
{
    return session_;
}

Status Engine::BuildStatus() const
{
    Status status{};
    status.compiledIn = kCompiledIn;
    status.active = session_.IsTrialBattleScreen();
    status.battleScreen = session_.IsTrialBattleScreen();
    status.subState = session_.rollbackSubState;
    status.phase = phase_;
    status.localSide = session_.localSide;
    status.remoteSide = session_.remoteSide;
    status.inputDelay = session_.inputDelay;
    status.delayAdjust = session_.delayAdjust;
    status.currentFrame = session_.currentFrame;
    status.simFrame = session_.simFrame;
    status.confirmedFrame = session_.confirmedFrame;
    status.matchStartFrame = session_.matchStartFrame;
    status.localInputCount = session_.localInputs.size();
    status.remoteInputCount = session_.remoteInputs.size();
    status.peerAckFrame = session_.peerAckFrame;
    status.predictedInputCount = session_.predictedInputs.size();
    status.lastBatchFrames = lastBatchFrames_;
    status.lastReloadFrame = lastReloadFrame_;
    status.rollbacksTotal = rollbacksTotal_;
    status.mispredictsTotal = mispredictsTotal_;

    for (const auto& slot : session_.frameRing)
    {
        if (slot.occupied)
        {
            ++status.occupiedRingSlots;
        }
    }
    return status;
}

void Engine::SetInputDelay(int frames)
{
    if (frames < 0)
    {
        frames = 0;
    }
    if (frames > static_cast<int>(proto::kMaxInputDelay))
    {
        frames = static_cast<int>(proto::kMaxInputDelay);
    }
    session_.inputDelay = frames;
    session_.configuredDelay = frames;
    session_.initSnapshot.inputDelay = static_cast<std::uint8_t>(frames);
    session_.initSnapshot.configuredDelay = static_cast<std::uint8_t>(frames);
}

void Engine::ServiceLiveDelayHotkeys(const int memorialScreen)
{
    if (!caster::IsNetplayActive() || memorialScreen != 3)
    {
        increaseDelayKeyDown_ = false;
        decreaseDelayKeyDown_ = false;
        return;
    }

    const int bindingPlayer = caster::LocalBindingPlayerIndex();
    const auto down = [bindingPlayer](const trialnet::shortcuts::Action action) {
        return !controls::ShortcutConflictsWithFightBinding(action, bindingPlayer)
            && input::IsDownNow(controls::ShortcutKey(action));
    };
    const bool increaseDown = down(trialnet::shortcuts::Action::IncreaseInputDelay);
    const bool decreaseDown = down(trialnet::shortcuts::Action::DecreaseInputDelay);
    const bool increasePressed = increaseDown && !increaseDelayKeyDown_;
    const bool decreasePressed = decreaseDown && !decreaseDelayKeyDown_;
    increaseDelayKeyDown_ = increaseDown;
    decreaseDelayKeyDown_ = decreaseDown;

    const int scriptedDirection = harness::ConsumeScriptedLiveDelayDirection(
        session_.localSide);

    if (increasePressed && decreasePressed)
    {
        if (increasePressed)
        {
            log::Warn("rollback", "live delay shortcut conflict; change ignored");
        }
        return;
    }

    const int direction = increasePressed ? 1
        : decreasePressed ? -1
        : scriptedDirection;
    if (direction == 0)
    {
        return;
    }
    const int oldDelay = session_.inputDelay;
    const int newDelay = netplay::AdjustLiveInputDelay(
        oldDelay,
        direction,
        session_.pingMs,
        session_.delayAdjust,
        pacing::GetSessionTargetFps());
    if (newDelay == oldDelay)
    {
        lifecycle::Input(
            "live_delay",
            "unchanged delay=%d direction=%d min=%d ping=%u rollback=%d frame=%u",
            oldDelay,
            direction,
            netplay::MinimumLiveInputDelay(
                session_.pingMs,
                session_.delayAdjust,
                pacing::GetSessionTargetFps()),
            session_.pingMs,
            session_.delayAdjust,
            session_.currentFrame);
        log::Info(
            "rollback",
            "input delay unchanged at %d (direction=%d, minimum=%d, ping=%u ms, max rollback=%d)",
            oldDelay,
            direction,
            netplay::MinimumLiveInputDelay(
                session_.pingMs,
                session_.delayAdjust,
                pacing::GetSessionTargetFps()),
            session_.pingMs,
            session_.delayAdjust);
        return;
    }

    SetInputDelay(newDelay);
    lifecycle::Input(
        "live_delay",
        "changed old=%d new=%d direction=%d min=%d ping=%u rollback=%d frame=%u local=%zu remote=%zu",
        oldDelay,
        newDelay,
        direction,
        netplay::MinimumLiveInputDelay(
            session_.pingMs,
            session_.delayAdjust,
            pacing::GetSessionTargetFps()),
        session_.pingMs,
        session_.delayAdjust,
        session_.currentFrame,
        session_.localInputs.size(),
        session_.remoteInputs.size());
    log::Info(
        "rollback",
        "input delay changed locally %d -> %d (minimum=%d, ping=%u ms, max rollback=%d)",
        oldDelay,
        newDelay,
        netplay::MinimumLiveInputDelay(
            session_.pingMs,
            session_.delayAdjust,
            pacing::GetSessionTargetFps()),
        session_.pingMs,
        session_.delayAdjust);
}

void Engine::SetPingMs(const std::uint32_t pingMs)
{
    session_.pingMs = pingMs;
}

void Engine::SetPredictionWindow(int frames)
{
    if (frames < 0)
    {
        frames = 0;
    }
    if (frames > kMaxPredictionFrames)
    {
        frames = kMaxPredictionFrames;
    }

    const int delay = 1 + (session_.inputDelay > 0 ? session_.inputDelay : 0);
    const int wireHeadroom = proto::kMaxQueueSlot - 2 - delay;
    if (frames > wireHeadroom)
    {
        frames = wireHeadroom > 0 ? wireHeadroom : 0;
    }

    session_.predictionWindow = frames;
    if (session_.battleHandoffReady)
    {
        session_.delayAdjust = frames;
    }
    log::Info(
        "rollback",
        "prediction window=%d (inputDelay=%d ping=%u)",
        frames,
        session_.inputDelay,
        session_.pingMs);
}

void Engine::SetLocalSide(int side)
{
    if (side == 0 || side == 1)
    {
        session_.localSide = side;
        session_.remoteSide = side == 0 ? 1 : 0;
        session_.initSnapshot.localSide = static_cast<std::uint8_t>(side);
        session_.initSnapshot.remoteSide = static_cast<std::uint8_t>(session_.remoteSide);
    }
}

void Engine::SetRngSeed(const std::uint32_t seed)
{
    session_.initSnapshot.rngSeed = seed;
}

void Engine::PushLocalInput(std::uint8_t wireMask)
{
    if (!session_.AppendLocalInput(wireMask) && caster::IsNetplayActive())
    {
        netplay::AbortNetplaySession("local input history capacity was exhausted");
    }
}

void Engine::PushRemoteInput(std::uint8_t wireMask, std::uint8_t systemState)
{
    const std::uint8_t receivedSystemState = systemState;
    systemState = menu::FilterOnlineSystemState(systemState);
    if (receivedSystemState != systemState)
    {
        lifecycle::Input(
            "rollback_system",
            "masked remote system state raw=0x%02X allowed=0x%02X frame=%zu",
            static_cast<unsigned>(receivedSystemState),
            static_cast<unsigned>(systemState),
            session_.remoteInputs.size());
    }
    const std::size_t frame = session_.remoteInputs.size();
    const bool escDown = (systemState & menu::kSystemStateEsc) != 0u;
    if (escDown && !remoteEscDown_)
    {
        pendingRemoteEsc_ = true;
        pendingRemoteEscFrame_ = static_cast<std::uint32_t>(frame);
        lifecycle::Input(
            "rollback_system",
            "queued remote ESC edge frame=%zu current=%u",
            frame,
            session_.currentFrame);
    }
    remoteEscDown_ = escDown;
    if (!session_.AppendRemoteInput(wireMask, systemState))
    {
        if (caster::IsNetplayActive())
        {
            netplay::AbortNetplaySession("remote input history capacity was exhausted");
        }
        return;
    }

    if (caster::IsNetplayActive())
    {
        proto::PlayerInputRecord record{};
        record.playerAction = wireMask;
        record.systemState = systemState;
        caster::PushRemoteRecord(record);
    }

    // Keep the earliest mismatch: correcting it repairs every later snapshot during replay.
    if (frame < session_.currentFrame
        && session_.battleHandoffReady
        && session_.delayAdjust > 0)
    {
        const RollbackFrameSlot& slot = session_.FrameSlot(static_cast<std::uint32_t>(frame));
        if (!slot.occupied || slot.frame != frame)
        {
            lifecycle::Fault(
                "rollback",
                "late input frame=%zu has no exact snapshot (slot=%u occupied=%d cur=%u)",
                frame,
                slot.frame,
                slot.occupied ? 1 : 0,
                session_.currentFrame);
            log::Error(
                "rollback",
                "late input frame=%zu has no exact snapshot (slot=%u occupied=%d cur=%u)",
                frame,
                slot.frame,
                slot.occupied ? 1 : 0,
                session_.currentFrame);
            if (caster::IsNetplayActive())
            {
                netplay::AbortNetplaySession("rollback snapshot window was exceeded");
            }
        }
        else if (slot.remoteInput != wireMask || slot.remoteSystemState != systemState)
        {
            const std::uint32_t mismatchFrame = static_cast<std::uint32_t>(frame);
            pendingRollbackFrame_ = pendingRollbackFrame_ == kNoFrame
                ? mismatchFrame
                : std::min(pendingRollbackFrame_, mismatchFrame);
            ++mispredictsTotal_;
            log::Debug(
                "rollback",
                "mispredict frame %zu saved=0x%02X/%02X confirmed=0x%02X/%02X (cur=%u rewind=%u total=%u)",
                frame,
                slot.remoteInput,
                slot.remoteSystemState,
                wireMask,
                systemState,
                session_.currentFrame,
                pendingRollbackFrame_,
                mispredictsTotal_);
        }
    }
}

void Engine::ResetBattleRollbackState(const std::uint32_t startFrame)
{
    pendingRollbackFrame_ = kNoFrame;
    rollbackReplayTarget_ = 0;
    transitionBlockedFrame_ = kNoFrame;
    nextHashFrame_ = startFrame;
}

void Engine::ReseedMatchRng()
{
    // Round continuations keep this stream.
    if (!caster::IsNetplayActive() || session_.initSnapshot.rngSeed == 0u)
    {
        return;
    }

    const std::uint32_t generation = netplay::GetSyncGeneration();
    const std::uint32_t matchSeed =
        session_.initSnapshot.rngSeed ^ (0x9E3779B9u * (generation + 1u));
    det::ResetFpuForSimulation();
    det::rng::SetSeed(matchSeed);
    det::rng::VisualRestore(matchSeed ^ 0xA511E9B3u);

    log::Step(
        "rollback",
        "battle entry rng reseed 0x%08X (base=0x%08X generation=%u localMatchId=%u)",
        matchSeed,
        session_.initSnapshot.rngSeed,
        generation,
        session_.matchId);
}

void Engine::BeginMatchOnScreenEntry()
{
    if (session_.battleHandoffReady && session_.MapMemorialScreenIndex() == 3)
    {
        return;
    }

    const std::uint32_t frame = session_.currentFrame;
    session_.BeginMatchFromHandoff(frame);
    ResetBattleRollbackState(frame);
    ReseedMatchRng();
    if (!SeedBattleEntrySnapshot())
    {
        lifecycle::Rollback("autosave", "begin_match flush failed frame=%u", frame);
        log::Warn("rollback", "begin_match autosave flush failed frame=%u", frame);
    }
    log::Step(
        "rollback",
        "begin_match frame=%u local=%zu remote=%zu (delayAdjust=0 histories preserved)",
        frame,
        session_.localInputs.size(),
        session_.remoteInputs.size());
}

void Engine::ImportWireHistories(
    const std::vector<std::uint8_t>& local,
    const std::vector<std::uint8_t>& remote,
    const std::uint32_t simFrame)
{
    const std::size_t frontier = std::min(
        {local.size(), remote.size(), static_cast<std::size_t>(simFrame)});
    session_.localInputs.assign(local.begin(), local.begin() + static_cast<std::ptrdiff_t>(frontier));
    session_.remoteInputs.assign(remote.begin(), remote.begin() + static_cast<std::ptrdiff_t>(frontier));
    session_.localSystemInputs.assign(frontier, 0u);
    session_.remoteSystemInputs.assign(frontier, 0u);
    session_.frameScreens.assign(frontier, 0xFFu);
    session_.frameGenerations.assign(frontier, 0u);
    session_.frameRegionHashes.assign(frontier, {});
    session_.frameHashValid.assign(frontier, 0u);
    // Seed the ACK frontier from the barrier so the first tick is not flow-capped.
    session_.peerAckFrame = static_cast<std::uint32_t>(frontier);
    session_.BeginMatchFromHandoff(static_cast<std::uint32_t>(frontier));
    ResetBattleRollbackState(static_cast<std::uint32_t>(frontier));
    // Mark the loading owner so MainTick does not treat its first tick as a transition.
    session_.prevMemorialIndex = session_.MapMemorialScreenIndex();
    session_.memorialIndex = session_.prevMemorialIndex;
    session_.rollbackSubState = session_.MapTrialScreenSubState();
    session_.prevSubState = session_.rollbackSubState;
    nextSpectatorFrame_ = static_cast<std::uint32_t>(frontier);
    ReseedMatchRng();
    if (!SeedBattleEntrySnapshot())
    {
        log::Warn("rollback", "battle entry snapshot seed failed frontier=%zu", frontier);
    }
    log::Step(
        "rollback",
        "import wire histories frontier=%zu (raw local=%zu remote=%zu sim=%u) delay=%d ping=%u",
        frontier,
        local.size(),
        remote.size(),
        simFrame,
        session_.inputDelay,
        session_.pingMs);
}

void Engine::FillSyncDataPacket(proto::SyncDataPacket* out) const
{
    if (out == nullptr)
    {
        return;
    }

    out->controlWord = proto::kActionSyncData;
    out->currentFrame = static_cast<std::uint32_t>(session_.localInputs.size());
    out->ackFrame = static_cast<std::uint32_t>(session_.remoteInputs.size());
    out->confirmedHashFrame = UINT32_MAX;
    out->confirmedHashChain = 0;
    desync::GetLatestLocalChain(&out->confirmedHashFrame, &out->confirmedHashChain);
    out->actionData = {};

    const std::size_t count =
        std::min(session_.localInputs.size(), static_cast<std::size_t>(proto::kMaxQueueSlot));
    for (std::size_t i = 0; i < count; ++i)
    {
        proto::PlayerInputRecord record{};
        const std::size_t frame = session_.localInputs.size() - count + i;
        record.playerAction = session_.localInputs[frame];
        record.systemState = session_.LocalSystemStateAt(static_cast<std::uint32_t>(frame));
        out->actionData[i] = record;
    }
}

bool Engine::ApplySyncDataPacket(const proto::SyncDataPacket& packet)
{
    constexpr std::uint32_t kMaxRemoteLeadFrames = proto::kMaxQueueSlot;

    const std::uint32_t remoteEnd = packet.currentFrame;
    const std::uint32_t prevSize = static_cast<std::uint32_t>(session_.remoteInputs.size());
    const std::uint32_t localSize = static_cast<std::uint32_t>(session_.localInputs.size());
    const std::uint32_t boundedAck = std::min(packet.ackFrame, localSize);
    session_.peerAckFrame = std::max(
        session_.peerAckFrame,
        boundedAck);
    if (remoteEnd < prevSize)
    {
        return true;
    }

    if (remoteEnd == prevSize)
    {
        return true;
    }

    if (static_cast<std::uint64_t>(remoteEnd)
        > static_cast<std::uint64_t>(localSize) + static_cast<std::uint64_t>(kMaxRemoteLeadFrames))
    {
        lifecycle::Sync(
            "rollback",
            "reject remote lead remoteEnd=%u local=%d prev=%u",
            remoteEnd,
            static_cast<int>(localSize),
            prevSize);
        log::Warn(
            "rollback",
            "reject remote lead remoteEnd=%u local=%d",
            remoteEnd,
            static_cast<int>(localSize));
        return false;
    }

    const std::uint32_t windowCount =
        std::min(remoteEnd, static_cast<std::uint32_t>(proto::kMaxQueueSlot));
    const std::uint32_t windowStart = remoteEnd - windowCount;

    // Older inputs are never resent; padding them is a guaranteed desync, so reject.
    if (session_.remoteInputs.size() < windowStart)
    {
        lifecycle::Fault(
            "rollback",
            "unfillable remote input gap %zu -> %u (remoteEnd=%u) — rejecting, sync is lost",
            session_.remoteInputs.size(),
            windowStart,
            remoteEnd);
        log::Error(
            "rollback",
            "unfillable remote input gap %zu -> %u (remoteEnd=%u) — rejecting, sync is lost",
            session_.remoteInputs.size(),
            windowStart,
            remoteEnd);
        return false;
    }

    for (std::uint32_t frame = std::max(prevSize, windowStart); frame < remoteEnd; ++frame)
    {
        const std::size_t packetIndex = static_cast<std::size_t>(frame - windowStart);
        PushRemoteInput(
            packet.actionData[packetIndex].playerAction,
            packet.actionData[packetIndex].systemState);
    }

    return true;
}

bool Engine::RequireRemoteInput() const
{
    if (!caster::IsNetplayActive())
    {
        return false;
    }

    const int memorial = session_.MapMemorialScreenIndex();
    return memorial == 0 || memorial == 1 || memorial == 2 || memorial == 3
        || Session::IsPostMatchScreenFromGame();
}

bool Engine::IsMatchRollbackActive() const
{
    if (!session_.battleHandoffReady)
    {
        return false;
    }
    const auto* const mode = static_cast<const std::uint8_t*>(
        trial::ResolveVa(trial::kVaBattleScreenMode));
    if (mode == nullptr)
    {
        return false;
    }
    if (*mode == trial::kScreenBattle)
    {
        return true;
    }
    if (*mode != trial::kScreenLoading)
    {
        return false;
    }
    const auto* const subState = static_cast<const std::int32_t*>(
        trial::ResolveVa(trial::kVaScreenSubState));
    if (subState == nullptr)
    {
        return false;
    }
    return *subState == 2
        || *subState == trial::kRoundContinueP1SubState
        || *subState == trial::kRoundContinueP2SubState
        || *subState == trial::kRoundContinueDrawSubState;
}

bool Engine::IsPredictionScreen() const
{
    return IsMatchRollbackActive();
}

bool Engine::IsInputAuthorityActive() const
{
    if (!caster::IsNetplayActive())
    {
        return session_.IsTrialBattleScreen();
    }

    const int memorial = session_.MapMemorialScreenIndex();
    if (memorial != 0 && memorial != 1 && memorial != 2 && memorial != 3
        && !Session::IsPostMatchScreenFromGame())
    {
        return false;
    }

    const std::uint32_t frame = session_.currentFrame;
    // HasCapturedInputs: inject runs after SaveInputs has incremented confirmedFrame.
    const bool remoteRequiredForAuthority =
        RequireRemoteInput() && !(IsPredictionScreen() && session_.delayAdjust > 0);
    return session_.HasCapturedInputs(frame, remoteRequiredForAuthority);
}

bool Engine::IsDrivingCatchUp() const
{
    return drivingCatchUp_;
}

bool Engine::TryGetWireMaskForPlayerSide(int playerSide, std::uint8_t* outMask) const
{
    if (outMask == nullptr || !IsInputAuthorityActive())
    {
        return false;
    }

    const std::uint32_t frame = session_.WireInputFrame();
    const auto validateSavedInput = [&](const std::uint8_t mask) {
        const RollbackFrameSlot& slot = session_.FrameSlot(frame);
        if (!slot.occupied || slot.frame != frame)
        {
            return true;
        }
        const std::uint8_t expected = playerSide == session_.localSide
            ? slot.localInput
            : slot.remoteInput;
        if (mask == expected)
        {
            return true;
        }
        lifecycle::Fault(
            "rollback_input",
            "saved/injected mismatch frame=%u side=%d saved=%02X injected=%02X mode=%d",
            frame,
            playerSide,
            static_cast<unsigned>(expected),
            static_cast<unsigned>(mask),
            session_.MapMemorialScreenIndex());
        log::Critical(
            "rollback",
            "SAVED/INJECTED INPUT MISMATCH frame=%u side=%d saved=%02X injected=%02X",
            frame,
            playerSide,
            static_cast<unsigned>(expected),
            static_cast<unsigned>(mask));
        if (caster::IsNetplayActive())
        {
            netplay::AbortNetplaySession("saved rollback input differed from native injection");
        }
        return false;
    };

    if (playerSide == session_.localSide)
    {
        *outMask = session_.LocalInputAt(frame);
        return validateSavedInput(*outMask);
    }

    if (RequireRemoteInput() && playerSide == session_.remoteSide)
    {
        const bool predicting = session_.delayAdjust > 0 && IsPredictionScreen();
        *outMask = predicting ? session_.RemoteInputAtOrHeld(frame)
                              : session_.RemoteInputAt(frame);
        return validateSavedInput(*outMask);
    }

    return false;
}

std::uint8_t Engine::GetMergedSystemState() const
{
    if (!caster::IsNetplayActive())
    {
        return 0u;
    }

    // Do not gate on player-action authority: a mode flip can transiently stop injection.
    const std::uint32_t frame = session_.currentFrame;
    // ESC edge is retained until EFZ polls DirectInput; post-match skips some ticks.
    std::uint8_t merged = 0u;
    if ((pendingLocalEsc_ && frame >= pendingLocalEscFrame_)
        || (pendingRemoteEsc_ && frame >= pendingRemoteEscFrame_))
    {
        merged = static_cast<std::uint8_t>(merged | menu::kSystemStateEsc);
    }
    return merged;
}

bool Engine::ApplyConfirmedPracticeSystemState()
{
    // Never subtract the delay from the wire label: live changes repeat or skip actions.
    const std::uint32_t sourceFrame = session_.currentFrame;
    const std::uint8_t local = session_.LocalSystemStateAt(sourceFrame);
    const std::uint8_t remote = session_.RemoteSystemStateAt(sourceFrame);
    const std::uint8_t state = static_cast<std::uint8_t>((local | remote)
        & menu::kOnlinePracticeSystemMask);
    if (state == 0u)
    {
        return true;
    }

    if (!menu::ApplyTrialPracticeSystemState(state))
    {
        if (caster::IsNetplayActive())
        {
            netplay::AbortNetplaySession("practice system-state target was unavailable");
        }
        return false;
    }

    lifecycle::Input(
        "rollback_system",
        "apply confirmed practice state=0x%02X source=%u current=%u",
        static_cast<unsigned>(state),
        sourceFrame,
        session_.currentFrame);
    return true;
}

void Engine::AcknowledgeSystemStateApplied(const std::uint8_t state)
{
    if ((state & menu::kSystemStateEsc) == 0u)
    {
        return;
    }

    const bool appliedLocal = pendingLocalEsc_ && session_.currentFrame >= pendingLocalEscFrame_;
    const bool appliedRemote = pendingRemoteEsc_ && session_.currentFrame >= pendingRemoteEscFrame_;
    if (appliedLocal)
    {
        pendingLocalEsc_ = false;
        pendingLocalEscFrame_ = kNoFrame;
    }
    if (appliedRemote)
    {
        pendingRemoteEsc_ = false;
        pendingRemoteEscFrame_ = kNoFrame;
    }
    if (appliedLocal || appliedRemote)
    {
        lifecycle::Input(
            "rollback_system",
            "consumed ESC edge current=%u local=%d remote=%d",
            session_.currentFrame,
            appliedLocal ? 1 : 0,
            appliedRemote ? 1 : 0);
    }
}

bool Engine::CaptureAndAppendLocalInput()
{
    std::uint8_t mask = caster::SampleSyncWireMaskForCapture();
    std::uint8_t systemState = menu::SampleLocalSystemState();
    systemState = static_cast<std::uint8_t>(
        systemState
        | harness::ScriptedSystemState(
            session_.currentFrame,
            static_cast<std::uint32_t>(session_.localInputs.size()),
            session_.matchStartFrame,
            session_.localSide));
    systemState = menu::FilterOnlineSystemState(systemState);

    const std::uint32_t captureFrame =
        static_cast<std::uint32_t>(session_.localInputs.size());
    const bool escDown = (systemState & menu::kSystemStateEsc) != 0u;
    if (escDown && !localEscDown_)
    {
        pendingLocalEsc_ = true;
        pendingLocalEscFrame_ = captureFrame;
        lifecycle::Input(
            "rollback_system",
            "queued local ESC edge frame=%u current=%u",
            captureFrame,
            session_.currentFrame);
    }
    localEscDown_ = escDown;

    if (harness::ScriptedInputEnabled() && caster::IsNetplayActive())
    {
        const int memorial = session_.MapMemorialScreenIndex();
        if (memorial == 1)
        {
            mask = harness::ScriptedCharSelectMask(session_.localInputs.size());
        }
        else if (memorial == 3 || (memorial == 2 && session_.battleHandoffReady))
        {
            constexpr std::uint32_t kIntroMovementProbeFrames = 120u;
            if (captureFrame >= session_.matchStartFrame
                && captureFrame - session_.matchStartFrame < kIntroMovementProbeFrames)
            {
                mask = session_.localSide == 0
                    ? static_cast<std::uint8_t>(1u << 3u)
                    : static_cast<std::uint8_t>(1u << 2u);
            }
            else
            {
                mask = harness::ScriptedLocalMask(captureFrame, session_.localSide);
            }
        }
        else
        {
            // Neutral actions only: an A pulse can dismiss the winner screen before it is shown.
            mask = 0u;
        }
    }

    return AppendLocalInputWithProvenance(mask, systemState);
}

bool Engine::AppendRepeatedLocalInput()
{
    if (session_.localInputs.empty()
        || session_.localSystemInputs.size() != session_.localInputs.size())
    {
        lifecycle::Fault(
            "rollback",
            "remote-ahead repeat unavailable local=%zu system=%zu",
            session_.localInputs.size(),
            session_.localSystemInputs.size());
        if (caster::IsNetplayActive())
        {
            netplay::AbortNetplaySession("local repeat input history was unavailable");
        }
        return false;
    }

    const std::uint8_t wireMask = session_.localInputs.back();
    const std::uint8_t systemState = session_.localSystemInputs.back();
    log::Debug(
        "rollback",
        "remote ahead repeat local frame=%zu action=0x%02X system=0x%02X",
        session_.localInputs.size(),
        static_cast<unsigned>(wireMask),
        static_cast<unsigned>(systemState));
    return AppendLocalInputWithProvenance(wireMask, systemState);
}

bool Engine::AppendLocalInputWithProvenance(
    const std::uint8_t wireMask,
    const std::uint8_t systemState)
{
    const std::uint32_t captureFrame =
        static_cast<std::uint32_t>(session_.localInputs.size());
    if (!session_.AppendLocalInput(wireMask, systemState))
    {
        if (caster::IsNetplayActive())
        {
            netplay::AbortNetplaySession("local input history capacity was exhausted");
        }
        return false;
    }
    // Stamp provenance at the pre-frame boundary, not here.
    if (session_.frameScreens.size() == captureFrame
        && session_.frameGenerations.size() == captureFrame)
    {
        session_.frameScreens.push_back(0xFFu);
        session_.frameGenerations.push_back(0u);
        session_.frameRegionHashes.emplace_back();
        session_.frameHashValid.push_back(0u);
    }
    else if (captureFrame < session_.frameScreens.size()
        && captureFrame < session_.frameGenerations.size())
    {
        session_.frameScreens[captureFrame] = 0xFFu;
        session_.frameGenerations[captureFrame] = 0u;
        session_.frameHashValid[captureFrame] = 0u;
    }
    else
    {
        netplay::AbortNetplaySession("local input provenance history was misaligned");
        return false;
    }
    return true;
}

bool Engine::StampCurrentFrameProvenance()
{
    const std::uint32_t frame = session_.currentFrame;
    if (frame >= session_.frameScreens.size()
        || frame >= session_.frameGenerations.size())
    {
        log::Error(
            "spectator",
            "simulation provenance unavailable frame=%u screens=%zu generations=%zu local=%zu",
            frame,
            session_.frameScreens.size(),
            session_.frameGenerations.size(),
            session_.localInputs.size());
        if (caster::IsNetplayActive())
        {
            netplay::AbortNetplaySession("simulation provenance history was unavailable");
        }
        return false;
    }

    const auto* const screenPtr = static_cast<const std::uint8_t*>(
        trial::ResolveVa(trial::kVaBattleScreenMode));
    session_.frameScreens[frame] = screenPtr != nullptr ? *screenPtr : 0xFFu;
    session_.frameGenerations[frame] = netplay::GetSyncGeneration();
    return true;
}

void Engine::EnsureInputsCapturedForFrame(bool requireRemoteInput)
{
    const std::uint32_t frame = session_.currentFrame;

    if (session_.localInputs.size() == frame)
    {
        if (!CaptureAndAppendLocalInput())
        {
            return;
        }
    }
    else if (session_.localInputs.size() < frame)
    {
        lifecycle::Sync(
            "rollback",
            "local input gap size=%zu frame=%u",
            session_.localInputs.size(),
            frame);
        log::Warn(
            "rollback",
            "local input gap size=%zu frame=%u",
            session_.localInputs.size(),
            frame);
    }

    if (!requireRemoteInput)
    {
        return;
    }

    if (session_.remoteInputs.size() < frame)
    {
        log::Debug(
            "rollback",
            "remote input pending size=%zu frame=%u",
            session_.remoteInputs.size(),
            frame);
    }
}

bool Engine::SaveInputs()
{
    ScopedPerformanceTimer perfTimer(
        &perf_.saveCalls,
        &perf_.saveTotalUs,
        &perf_.saveMaxUs);
    if (!IsMatchRollbackActive())
    {
        return false;
    }

    const bool requireRemote = RequireRemoteInput();
    const std::uint32_t frame = session_.currentFrame;
    EnsureInputsCapturedForFrame(requireRemote);

    if (static_cast<std::size_t>(frame) >= session_.localInputs.size())
    {
        return false;
    }
    if (!StampCurrentFrameProvenance())
    {
        return false;
    }
    const bool remotePredicted =
        requireRemote && static_cast<std::size_t>(frame) >= session_.remoteInputs.size();
    if (remotePredicted
        && !(session_.delayAdjust > 0 && IsPredictionScreen()))
    {
        return false;
    }

    const std::uint8_t local = session_.LocalInputAt(frame);
    const std::uint8_t remote = requireRemote ? session_.RemoteInputAtOrHeld(frame) : 0u;
    const std::uint8_t localSystem = session_.LocalSystemStateAt(frame);
    const std::uint8_t remoteSystem = requireRemote ? session_.RemoteSystemStateAt(frame) : 0u;
    const auto maskForSide = [&](const int playerSide) -> std::uint8_t {
        if (playerSide == session_.localSide)
        {
            return local;
        }
        return playerSide == session_.remoteSide ? remote : 0u;
    };
    const std::uint8_t p1Mask = maskForSide(0);
    const std::uint8_t p2Mask = maskForSide(1);

    RollbackFrameSlot& slot = session_.FrameSlot(frame);

    const bool prevHashed = slot.hashed && slot.frame == frame && slot.occupied;
    const std::array<std::uint32_t, proto::kFrameHashRegions> prevHashes = slot.regionHashes;
    const std::uint8_t prevLocal = slot.localInput;
    const std::uint8_t prevRemote = slot.remoteInput;
    const std::uint8_t prevLocalSystem = slot.localSystemState;
    const std::uint8_t prevRemoteSystem = slot.remoteSystemState;
    // SaveInputs replaces slot.battle below; keep the old bytes for a post-failure dump.
    const std::vector<std::uint8_t> prevBattle = prevHashed ? slot.battle : std::vector<std::uint8_t>{};

    slot.frame = frame;
    slot.simFrame = session_.simFrame;
    slot.localInput = local;
    slot.remoteInput = remote;
    slot.localSystemState = localSystem;
    slot.remoteSystemState = remoteSystem;
    const auto* const rawScreen = static_cast<const std::uint8_t*>(
        trial::ResolveVa(trial::kVaBattleScreenMode));
    slot.screen = rawScreen != nullptr ? *rawScreen : 0xFFu;
    slot.sourceGeneration = netplay::GetSyncGeneration();
    det::ResetFpuForSimulation();
    memory::CaptureDeterminism(slot.fpu.data(), &slot.rngShadow);
    memory::CaptureShakeAux(slot.shakeAux.data());
    memory::CaptureVisualEffects(slot.visualEffects.data());
    slot.visualRngShadow = det::rng::VisualSnapshot();
    if (!memory::CaptureBattle(&slot.battle))
    {
        lifecycle::Rollback("save_fail", "battle capture failed frame=%u", frame);
        log::Warn("rollback", "save frame %u — battle capture failed", frame);
        return false;
    }
    slot.occupied = true;

    char saveCtx[48] = {};
    std::snprintf(saveCtx, sizeof(saveCtx), "rollback_save f=%u", frame);
    memlog::LogBlobHash(saveCtx, "slot_battle", slot.battle.data(), slot.battle.size());

    std::uint32_t regionHashes[proto::kFrameHashRegions] = {};
    if (slot.battle.size() >= trial::kDynamicBattleSize)
    {
        std::size_t hashOff = 0;
        for (int r = 0; r < trial::kDynamicRegionCount; ++r)
        {
            const std::size_t sz = trial::kDynamicBattleRegions[r].size;
            regionHashes[r] = savestate_digest::Crc32(slot.battle.data() + hashOff, sz);
            hashOff += sz;
        }

        std::array<std::uint8_t, sizeof(std::uint16_t) + sizeof(std::uint32_t)> determinismBytes{};
        std::memcpy(determinismBytes.data(), slot.fpu.data(), sizeof(std::uint16_t));
        std::memcpy(
            determinismBytes.data() + sizeof(std::uint16_t),
            &slot.rngShadow,
            sizeof(slot.rngShadow));
        regionHashes[trial::kDynamicRegionCount] = savestate_digest::Crc32(
            determinismBytes.data(), determinismBytes.size());
        regionHashes[trial::kDynamicRegionCount + 1] =
            savestate_digest::Crc32(slot.shakeAux.data(), slot.shakeAux.size());

        if (prevHashed && prevLocal == local && prevRemote == remote
            && prevLocalSystem == localSystem && prevRemoteSystem == remoteSystem)
        {
            for (int r = 0; r < proto::kFrameHashRegions; ++r)
            {
                if (prevHashes[r] != regionHashes[r])
                {
                    lifecycle::Rollback(
                        "resim_diverge",
                        "frame=%u region=%s in=0x%02X/0x%02X %08X->%08X",
                        frame,
                        desync::RegionName(r),
                        local,
                        remote,
                        prevHashes[r],
                        regionHashes[r]);
                    log::Warn(
                        "rollback",
                        "RESIM NON-DETERMINISTIC frame=%u region=%s (same in L=0x%02X R=0x%02X) "
                        "%08X -> %08X — snapshot is missing state this region reads",
                        frame,
                        desync::RegionName(r),
                        local,
                        remote,
                        prevHashes[r],
                        regionHashes[r]);
                    if (prevBattle.size() == slot.battle.size())
                    {
                        std::size_t regionOffset = 0;
                        for (int prior = 0; prior < r && prior < trial::kDynamicRegionCount; ++prior)
                        {
                            regionOffset += trial::kDynamicBattleRegions[prior].size;
                        }
                        const std::size_t regionSize = r < trial::kDynamicRegionCount
                            ? trial::kDynamicBattleRegions[r].size
                            : 0u;
                        unsigned logged = 0;
                        for (std::size_t offset = 0; offset < regionSize && logged < 32u; ++offset)
                        {
                            const std::uint8_t before = prevBattle[regionOffset + offset];
                            const std::uint8_t after = slot.battle[regionOffset + offset];
                            if (before == after)
                            {
                                continue;
                            }
                            log::Warn(
                                "rollback",
                                "RESIM BYTE frame=%u va=0x%08X old=%02X new=%02X",
                                frame,
                                static_cast<unsigned>(trial::kDynamicBattleRegions[r].va + offset),
                                before,
                                after);
                            ++logged;
                        }
                    }
                    desync::NoteResimDivergence();
                    desync::RequestDump(frame, 1u << r);
                }
            }
        }
        for (int r = 0; r < proto::kFrameHashRegions; ++r)
        {
            slot.regionHashes[r] = regionHashes[r];
        }
        slot.hashed = true;
        if (session_.frameRegionHashes.size() <= frame)
        {
            session_.frameRegionHashes.resize(static_cast<std::size_t>(frame) + 1u);
            session_.frameHashValid.resize(static_cast<std::size_t>(frame) + 1u, 0u);
        }
        session_.frameRegionHashes[frame] = slot.regionHashes;
        session_.frameHashValid[frame] = 1u;

        log::Debug(
            "rollback",
            "save f=%u sim=%u rh=[%08X %08X %08X %08X %08X det=%08X aux=%08X] rng=%08X fpu=%04X in[L=0x%02X R=0x%02X] p[%02X %02X]",
            frame,
            slot.simFrame,
            regionHashes[0],
            regionHashes[1],
            regionHashes[2],
            regionHashes[3],
            regionHashes[4],
            regionHashes[5],
            regionHashes[6],
            slot.rngShadow,
            static_cast<unsigned>(det::GetCapturedFpuCw()),
            local,
            remote,
            p1Mask,
            p2Mask);
    }
    memlog::LogBlobHash(saveCtx, "slot_rng", &slot.rngShadow, sizeof(slot.rngShadow));

    log::Debug(
        "rollback",
        "save frame %u local=0x%02X remote=0x%02X predicted=%d inject P1=0x%02X P2=0x%02X",
        frame,
        local,
        remote,
        remotePredicted ? 1 : 0,
        p1Mask,
        p2Mask);
    return true;
}

bool Engine::SeedBattleEntrySnapshot()
{
    if (!session_.IsTrialBattleScreen() && !rollback::IsBattleIntroBoundaryPending())
    {
        return false;
    }

    const std::uint32_t frame = session_.currentFrame;
    const bool requireRemote = RequireRemoteInput();

    // At handoff currentFrame == history.size(); read the last committed input.
    if (session_.localInputs.empty())
    {
        lifecycle::Rollback("seed_fail", "empty local history frame=%u", frame);
        log::Warn("rollback", "seed battle entry — empty local history frame=%u", frame);
        return false;
    }
    if (requireRemote && session_.remoteInputs.empty())
    {
        lifecycle::Rollback("seed_fail", "empty remote history frame=%u", frame);
        log::Warn("rollback", "seed battle entry — empty remote history frame=%u", frame);
        return false;
    }

    const auto historyInputIndex = [](const std::uint32_t simFrame, const std::size_t historySize) -> std::uint32_t
    {
        if (historySize == 0u)
        {
            return 0u;
        }
        return simFrame < historySize ? simFrame : static_cast<std::uint32_t>(historySize - 1u);
    };

    const std::uint32_t localIdx = historyInputIndex(frame, session_.localInputs.size());
    const std::uint32_t remoteIdx = historyInputIndex(frame, session_.remoteInputs.size());

    RollbackFrameSlot& slot = session_.FrameSlot(frame);
    slot.frame = frame;
    slot.simFrame = session_.simFrame;
    slot.localInput = session_.LocalInputAt(localIdx);
    slot.remoteInput = requireRemote ? session_.RemoteInputAt(remoteIdx) : 0u;
    slot.localSystemState = session_.LocalSystemStateAt(localIdx);
    slot.remoteSystemState = requireRemote ? session_.RemoteSystemStateAt(remoteIdx) : 0u;
    const auto* const rawScreen = static_cast<const std::uint8_t*>(
        trial::ResolveVa(trial::kVaBattleScreenMode));
    slot.screen = rawScreen != nullptr ? *rawScreen : 0xFFu;
    slot.sourceGeneration = netplay::GetSyncGeneration();
    slot.hashed = false;
    slot.regionHashes.fill(0u);
    det::ResetFpuForSimulation();
    memory::CaptureDeterminism(slot.fpu.data(), &slot.rngShadow);

    // Canonicalize shake at the battle boundary; keep the adjacent charge-edge flags.
    std::array<std::uint8_t, trial::kAuxStateSize> entryAux{};
    memory::CaptureShakeAux(entryAux.data());
    const auto shakeEnd = entryAux.begin() + trial::kShakeStateSize;
    if (std::any_of(entryAux.begin(), shakeEnd, [](std::uint8_t value) { return value != 0u; }))
    {
        std::uint64_t magnitudeBits = 0u;
        std::uint32_t phase0 = 0u;
        std::uint32_t phase1 = 0u;
        std::memcpy(&magnitudeBits, entryAux.data(), sizeof(magnitudeBits));
        std::memcpy(&phase0, entryAux.data() + 8u, sizeof(phase0));
        std::memcpy(&phase1, entryAux.data() + 12u, sizeof(phase1));
        lifecycle::Rollback(
            "entry_shake_reset",
            "frame=%u magnitude=%08X%08X phase=%u/%u -> zero",
            frame,
            static_cast<unsigned>(magnitudeBits >> 32u),
            static_cast<unsigned>(magnitudeBits),
            phase0,
            phase1);
        std::fill(entryAux.begin(), shakeEnd, std::uint8_t{0});
        memory::RestoreShakeAux(entryAux.data());
    }
    memory::CaptureShakeAux(slot.shakeAux.data());
    memory::CaptureVisualEffects(slot.visualEffects.data());
    slot.visualRngShadow = det::rng::VisualSnapshot();
    if (!memory::CaptureBattle(&slot.battle))
    {
        lifecycle::Rollback("seed_fail", "battle capture failed frame=%u", frame);
        log::Warn("rollback", "seed battle entry snapshot — battle capture failed frame=%u", frame);
        return false;
    }

    slot.occupied = true;

    char seedCtx[56] = {};
    std::snprintf(seedCtx, sizeof(seedCtx), "rollback_seed f=%u", frame);
    memlog::LogBlobHash(seedCtx, "slot_battle", slot.battle.data(), slot.battle.size());
    memlog::LogBlobHash(seedCtx, "slot_rng", &slot.rngShadow, sizeof(slot.rngShadow));

    log::Info(
        "rollback",
        "seed battle entry snapshot frame=%u local=0x%02X remote=0x%02X sim=%u",
        frame,
        slot.localInput,
        slot.remoteInput,
        session_.simFrame);
    return true;
}

bool Engine::ApplyPendingPostMatchEsc()
{
    if (!Session::IsPostMatchScreenFromGame()
        || (GetMergedSystemState() & menu::kSystemStateEsc) == 0u)
    {
        return false;
    }

    auto* const subState = static_cast<std::uint32_t*>(
        trial::ResolveVa(trial::kVaScreenSubState));
    auto* const animCounter = static_cast<std::int32_t*>(
        trial::ResolveVa(trial::kVaScreenAnimCounter));
    if (subState == nullptr || animCounter == nullptr)
    {
        return false;
    }

    const std::uint32_t oldSubState = *subState;
    const std::int32_t oldAnimCounter = *animCounter;

    // Matches the native tick: ESC selects case 4 on post-match substates but the fade.
    if (oldSubState != 4u)
    {
        *subState = 4u;
        *animCounter = 0;
        memlog::LogScalarWrite(
            "rollback_post_esc",
            trial::kVaScreenSubState,
            "screen_substate",
            oldSubState,
            4u,
            4);
        memlog::LogScalarWrite(
            "rollback_post_esc",
            trial::kVaScreenAnimCounter,
            "screen_anim",
            static_cast<std::uint32_t>(oldAnimCounter),
            0u,
            4);
    }

    AcknowledgeSystemStateApplied(menu::kSystemStateEsc);
    lifecycle::Input(
        "rollback_system",
        "apply post ESC frame=%u sub=%u anim=%d -> char-select fade",
        session_.currentFrame,
        static_cast<unsigned>(oldSubState),
        oldAnimCounter);
    return true;
}

bool Engine::ApplyPendingGameplayEsc()
{
    if (session_.MapMemorialScreenIndex() != 3
        || (GetMergedSystemState() & menu::kSystemStateEsc) == 0u)
    {
        return false;
    }

    auto* const subState = static_cast<std::uint32_t*>(
        trial::ResolveVa(trial::kVaScreenSubState));
    auto* const animCounter = static_cast<std::int32_t*>(
        trial::ResolveVa(trial::kVaScreenAnimCounter));
    if (subState == nullptr || animCounter == nullptr)
    {
        if (caster::IsNetplayActive())
        {
            netplay::AbortNetplaySession("gameplay ESC lifecycle target was unavailable");
        }
        return false;
    }

    const std::uint32_t oldSubState = *subState;
    const std::int32_t oldAnimCounter = *animCounter;
    AcknowledgeSystemStateApplied(menu::kSystemStateEsc);

    // Raw DI would consume the ESC edge on different frames on each peer.
    if (oldSubState != 0u)
    {
        lifecycle::Input(
            "rollback_system",
            "consume gameplay ESC outside active substate frame=%u sub=%u anim=%d",
            session_.currentFrame,
            static_cast<unsigned>(oldSubState),
            oldAnimCounter);
        return false;
    }

    *subState = 2u;
    *animCounter = 0;
    gameplayEscTransitionActive_ = true;
    memlog::LogScalarWrite(
        "rollback_gameplay_esc",
        trial::kVaScreenSubState,
        "screen_substate",
        oldSubState,
        2u,
        4);
    memlog::LogScalarWrite(
        "rollback_gameplay_esc",
        trial::kVaScreenAnimCounter,
        "screen_anim",
        static_cast<std::uint32_t>(oldAnimCounter),
        0u,
        4);
    lifecycle::Input(
        "rollback_system",
        "apply gameplay ESC frame=%u sub=%u anim=%d -> char-select fade",
        session_.currentFrame,
        static_cast<unsigned>(oldSubState),
        oldAnimCounter);
    return true;
}

bool Engine::AdvanceOne()
{
    const std::uint32_t frame = session_.currentFrame;
    const bool introFallthroughFrame = IsBattleIntroBoundaryPending();
    const std::uint32_t introOffset = frame >= session_.matchStartFrame
        ? frame - session_.matchStartFrame : UINT32_MAX;
    const bool introTraceFrame = session_.battleHandoffReady
        && (introOffset == 0u || introOffset == 1u || introOffset == 2u
            || introOffset == 5u || introOffset == 10u || introOffset == 20u
            || introOffset == 40u || introOffset == 60u || introOffset == 90u);
    if (frame == session_.syncEnqueueFrame
        && frame < session_.localInputs.size()
        && frame < session_.remoteInputs.size())
    {
        ++session_.syncEnqueueFrame;
    }

    const auto readPositionX = [](const std::uintptr_t playerVa) -> double
    {
        const auto* const value = static_cast<const double*>(
            trial::ResolveVa(playerVa + trial::kOffPositionX));
        return value != nullptr ? *value : 0.0;
    };
    const double p1XBefore = introTraceFrame
        ? readPositionX(trial::kVaP1Player) : 0.0;
    const double p2XBefore = introTraceFrame
        ? readPositionX(trial::kVaP2Player) : 0.0;
    const std::uint8_t p1Mask = introTraceFrame
        ? session_.WireMaskForPlayerSide(frame, 0) : 0u;
    const std::uint8_t p2Mask = introTraceFrame
        ? session_.WireMaskForPlayerSide(frame, 1) : 0u;

    const int memorial = session_.MapMemorialScreenIndex();
    const HWND window = lastGameWindow_ != nullptr ? lastGameWindow_ : host_.gameWindow;

    using ScreenTickFn = int(__cdecl*)(HWND);
    ScreenTickFn tick = nullptr;
    switch (memorial)
    {
    case 0:
    {
        const auto* const mode = static_cast<const std::uint8_t*>(
            trial::ResolveVa(trial::kVaBattleScreenMode));
        if (mode != nullptr && *mode == trial::kScreenMainMenu)
        {
            tick = host_.advanceMenuTick;
        }
        else if (mode != nullptr && *mode == trial::kScreenTitle)
        {
            tick = host_.advanceTitleTick;
        }
        break;
    }
    case 1:
        tick = host_.advanceCharSelectTick;
        break;
    case 2:
        tick = host_.advanceLoadingTick;
        break;
    case 3:
        tick = host_.advanceBattleTick;
        break;
    default:
        if (Session::IsPostMatchScreenFromGame())
        {
            tick = host_.advanceLoadingTick;
            break;
        }
        return false;
    }

    if (tick == nullptr || window == nullptr)
    {
        return false;
    }
    if (!StampCurrentFrameProvenance())
    {
        return false;
    }

    // A late edge consumed here stops peers splitting between title and select.
    if (memorial < 0 && Session::IsPostMatchScreenFromGame())
    {
        ApplyPendingPostMatchEsc();
    }

    // Re-check before each tick: counter 10 must not load BGxx.DAT from an unsynced stage.
    if (memorial == 1 && caster::IsNetplayActive() && session_.initSnapshot.rngSeed != 0u)
    {
        const auto* const anim = static_cast<const std::int32_t*>(
            trial::ResolveVa(trial::kVaScreenAnimCounter));
        auto* const stage = static_cast<std::uint8_t*>(
            trial::ResolveVa(trial::kVaCharSelectStage));
        if (anim != nullptr && stage != nullptr
            && *anim >= trial::kNativeTransitionNearComplete)
        {
            std::uint8_t syncedStage =
                static_cast<std::uint8_t>(session_.initSnapshot.rngSeed % 4u);
            syncedStage = syncedStage == 2u ? 0u : (syncedStage == 1u ? 3u : syncedStage);
            *stage = syncedStage;
        }
    }

    if (memorial == 3 && caster::IsNetplayActive())
    {
        harness::MaybeForceLifecycleProgress(session_.currentFrame, session_.matchStartFrame);
    }

    // Do not run practice mutations or the battle handler after starting the fade.
    if (memorial == 3 && ApplyPendingGameplayEsc())
    {
        ++session_.simFrame;
        ++session_.currentFrame;
        return true;
    }

    if (memorial == 3 && !ApplyConfirmedPracticeSystemState())
    {
        return false;
    }

    {
        watchdog::ScopedPhase phase("native_battle_tick");
        tick(window);
    }
    if (introTraceFrame)
    {
        const auto* const mode = static_cast<const std::uint8_t*>(
            trial::ResolveVa(trial::kVaBattleScreenMode));
        const auto* const sub = static_cast<const std::uint32_t*>(
            trial::ResolveVa(trial::kVaScreenSubState));
        const auto* const p1 = static_cast<const std::uint8_t*>(
            trial::ResolveVa(trial::kVaP1Player));
        const auto* const p2 = static_cast<const std::uint8_t*>(
            trial::ResolveVa(trial::kVaP2Player));
        const auto readShort = [](const std::uint8_t* const player, const std::size_t offset) -> int
        {
            return player != nullptr
                ? static_cast<int>(*reinterpret_cast<const std::int16_t*>(player + offset))
                : -1;
        };
        const auto readInt = [](const std::uint8_t* const player, const std::size_t offset) -> int
        {
            return player != nullptr
                ? *reinterpret_cast<const std::int32_t*>(player + offset)
                : -1;
        };
        lifecycle::Input(
            "battle_intro",
            "intro frame=%u offset=%u fallthrough=%d masks[P1=%02X P2=%02X] x[P1=%.3f->%.3f P2=%.3f->%.3f] move[P1=%d P2=%d] cpu[P1=%d P2=%d] rawMode=%u sub=%u",
            frame,
            introOffset,
            introFallthroughFrame ? 1 : 0,
            static_cast<unsigned>(p1Mask),
            static_cast<unsigned>(p2Mask),
            p1XBefore,
            readPositionX(trial::kVaP1Player),
            p2XBefore,
            readPositionX(trial::kVaP2Player),
            readShort(p1, trial::kOffMoveId),
            readShort(p2, trial::kOffMoveId),
            readInt(p1, trial::kOffCpuControlFlag),
            readInt(p2, trial::kOffCpuControlFlag),
            mode != nullptr ? static_cast<unsigned>(*mode) : 0xFFu,
            sub != nullptr ? static_cast<unsigned>(*sub) : UINT32_MAX);
        log::Step(
            "input",
            "battle intro frame=%u offset=%u fallthrough=%d masks[P1=%02X P2=%02X] x[P1=%.3f->%.3f P2=%.3f->%.3f]",
            frame,
            introOffset,
            introFallthroughFrame ? 1 : 0,
            static_cast<unsigned>(p1Mask),
            static_cast<unsigned>(p2Mask),
            p1XBefore,
            readPositionX(trial::kVaP1Player),
            p2XBefore,
            readPositionX(trial::kVaP2Player));
    }
    ++session_.simFrame;
    ++session_.currentFrame;
    return true;
}

void Engine::HandleMemorialTransition(const int newMemorial, const int oldMemorial)
{
    // Post-match lockstep must not inherit hidden rendering or rollback sound suppression.
    SetPresentSuppressed(false);
    SetRollbackAudioFrame(0u, false);
    drivingCatchUp_ = false;
    phase_ = TransportPhase::Idle;
    lastBatchFrames_ = 0;
    const auto* const rawSubStatePtr = static_cast<const std::int32_t*>(
        trial::ResolveVa(trial::kVaScreenSubState));
    const int rawSubState = rawSubStatePtr != nullptr ? *rawSubStatePtr : -1;
    const bool roundContinues = oldMemorial == 3 && newMemorial < 0
        && session_.battleHandoffReady
        && (rawSubState == trial::kRoundContinueP1SubState
            || rawSubState == trial::kRoundContinueP2SubState
            || rawSubState == trial::kRoundContinueDrawSubState);

    if (roundContinues)
    {
        // Same match: do not reset ring, histories, RNG, hashes or recorder cursors.
        session_.prevMemorialIndex = newMemorial;
        lifecycle::Phase("rollback", "battle", "round_post", "confirmed round continuation");
        lifecycle::Checkpoint("battle -> round post (match timeline preserved)");
        log::Step(
            "rollback",
            "round continuation battle -> post sub=%d frame=%u matchStart=%u generation=%u",
            rawSubState,
            session_.currentFrame,
            session_.matchStartFrame,
            netplay::GetSyncGeneration());
        return;
    }

    if (oldMemorial == 3 && newMemorial != 3)
    {
        gameplayEscTransitionActive_ = false;
        // Quarantine the predicted suffix so buffered attacks cannot confirm characters.
        if (caster::IsNetplayActive())
        {
            if (!session_.TrimForFrontendEpoch(session_.currentFrame))
            {
                netplay::AbortNetplaySession("confirmed battle-exit frontier was unavailable");
            }
            else
            {
                netplay::AdvanceSyncTimeline();
            }
        }
    }

    if (newMemorial == 2 && !session_.battleHandoffReady
        && IsBattleIntroBoundaryPending())
    {
        session_.OnMemorialTransition(newMemorial, oldMemorial);
        session_.BeginMatchFromHandoff(session_.currentFrame);
        ResetBattleRollbackState(session_.currentFrame);
        ReseedMatchRng();
        if (!SeedBattleEntrySnapshot())
        {
            netplay::AbortNetplaySession("new-match entry snapshot seed failed");
        }
        session_.prevMemorialIndex = newMemorial;
        lifecycle::Phase("rollback", "char_select", "match_intro", "new match boundary");
        lifecycle::Checkpoint("new match intro frame 0");
        log::Step(
            "rollback",
            "new match begins at first intro simulation frame=%u matchId=%u generation=%u",
            session_.currentFrame,
            session_.matchId,
            netplay::GetSyncGeneration());
        return;
    }

    if (newMemorial == 2 && session_.battleHandoffReady)
    {
        // No state, cursor, generation, RNG, recorder, or ring reset is allowed here.
        session_.prevMemorialIndex = newMemorial;
        lifecycle::Phase("rollback", "round_post", "round_intro", "same match");
        log::Step(
            "rollback",
            "round intro continues match frame=%u matchStart=%u matchId=%u generation=%u",
            session_.currentFrame,
            session_.matchStartFrame,
            session_.matchId,
            netplay::GetSyncGeneration());
        return;
    }

    if (newMemorial == 3)
    {
        const bool matchAlreadyActive = session_.battleHandoffReady;
        if (matchAlreadyActive)
        {
        }
        else
        {
            lifecycle::Fault(
                "rollback",
                "battle entered without active match boundary frame=%u oldMemorial=%d",
                session_.currentFrame,
                oldMemorial);
            BeginMatchOnScreenEntry();
        }
        session_.prevMemorialIndex = newMemorial;

        lifecycle::Phase("rollback", "round_intro", "battle", "resume match rollback");
        lifecycle::Checkpoint("memorial transition -> battle");
        log::Step(
            "rollback",
            "memorial transition %d -> 3 (matchActive=%d frame=%u matchStart=%u)",
            oldMemorial,
            matchAlreadyActive ? 1 : 0,
            session_.currentFrame,
            session_.matchStartFrame);
        return;
    }

    if (newMemorial == 1 && oldMemorial != 1)
    {
        // Do not split on post-match -> loading between rounds.
        spectator::GameStartInfo start{};
        start.sourceGeneration = netplay::GetSyncGeneration();
        start.sourceFrame = session_.currentFrame;
        start.rngSeed = session_.initSnapshot.rngSeed;
        start.frontendRngState = det::rng::Snapshot();
        start.frontendVisualRngState = det::rng::VisualSnapshot();
        start.screen = trial::kScreenCharSelect;
        if (!spectator::CaptureGameStartVisualMetadata(&start))
        {
            log::Error(
                "spectator",
                "cannot capture rollback character-select visual metadata source=%u:%u",
                start.sourceGeneration,
                start.sourceFrame);
        }
        else
        {
            spectator::BeginGame(start);
            nextSpectatorFrame_ = session_.currentFrame;
        }
    }

    session_.OnMemorialTransition(newMemorial, oldMemorial);
    ResetBattleRollbackState(session_.currentFrame);
    session_.prevMemorialIndex = newMemorial;

    auto memorialName = [](const int memorial) -> const char* {
        switch (memorial)
        {
        case 0:
            return "menu/title";
        case 1:
            return "char_select";
        case 2:
            return "loading";
        case 3:
            return "battle";
        default:
            return "other";
        }
    };

    lifecycle::Phase(
        "rollback",
        memorialName(oldMemorial),
        memorialName(newMemorial),
        "memorial transition");
    lifecycle::Rollback(
        "memorial",
        "transition %d -> %d (%s) transport reset ring cleared",
        oldMemorial,
        newMemorial,
        memorialName(newMemorial));
    log::Step(
        "rollback",
        "memorial transition %d -> %d (%s) — transport reset, ring cleared",
        oldMemorial,
        newMemorial,
        memorialName(newMemorial));
}

bool Engine::ReloadSavedState(const std::uint32_t restoreFrame)
{
    const std::uint32_t replayTarget = session_.currentFrame;
    MergeDelayMaps(restoreFrame, replayTarget);

    const RollbackFrameSlot& slot = session_.FrameSlot(restoreFrame);
    if (!slot.occupied || slot.frame != restoreFrame)
    {
        lifecycle::Fault(
            "rollback",
            "exact reload unavailable restore=%u target=%u slot=%u occupied=%d",
            restoreFrame,
            replayTarget,
            slot.frame,
            slot.occupied ? 1 : 0);
        log::Error(
            "rollback",
            "exact reload unavailable restore=%u target=%u slot=%u occupied=%d",
            restoreFrame,
            replayTarget,
            slot.frame,
            slot.occupied ? 1 : 0);
        if (caster::IsNetplayActive())
        {
            netplay::AbortNetplaySession("exact rollback snapshot was unavailable");
        }
        return false;
    }

    phase_ = TransportPhase::Reloading;
    ApplyReloadedState(slot);
    lastReloadFrame_ = restoreFrame;
    ++rollbacksTotal_;

    // Do not log every correction: that load itself creates visible stalls.
    if (rollbacksTotal_ <= 4u || (rollbacksTotal_ % 256u) == 0u)
    {
        lifecycle::Rollback(
            "reload",
            "exact %u -> replay target %u total=%u",
            restoreFrame,
            replayTarget,
            rollbacksTotal_);
    }
    log::Debug("rollback", "reload exact %u for replay target %u", restoreFrame, replayTarget);
    phase_ = TransportPhase::Idle;
    return true;
}

void Engine::ApplyReloadedState(const RollbackFrameSlot& slot)
{
    char restoreCtx[80] = {};
    std::snprintf(
        restoreCtx,
        sizeof(restoreCtx),
        "rollback_reload f=%u sim=%u ring=%u",
        slot.frame,
        slot.simFrame,
        lastReloadFrame_);

    memlog::LogGameHeadline("before_engine_reload");
    if (!slot.battle.empty())
    {
        memlog::LogBlobHash(restoreCtx, "slot_payload", slot.battle.data(), slot.battle.size());
        memory::RestoreBattle(slot.battle.data(), slot.battle.size(), restoreCtx);
    }
    memory::RestoreDeterminism(slot.fpu.data(), slot.rngShadow);
    memory::RestoreShakeAux(slot.shakeAux.data());
    memory::RestoreVisualEffects(slot.visualEffects.data());
    det::rng::VisualRestore(slot.visualRngShadow);
    session_.simFrame = slot.simFrame;
    session_.currentFrame = slot.frame;
    memlog::LogGameHeadline("after_engine_reload");
}

void Engine::MergeDelayMaps(std::uint32_t restoreFrame, std::uint32_t targetFrame)
{
    session_.delayMapP0.clear();
    session_.delayMapP1.clear();

    if (targetFrame < restoreFrame)
    {
        return;
    }

    session_.delayMapP0.reserve(static_cast<std::size_t>(targetFrame - restoreFrame + 1u));
    session_.delayMapP1.reserve(static_cast<std::size_t>(targetFrame - restoreFrame + 1u));

    for (std::uint32_t frame = restoreFrame; frame <= targetFrame; ++frame)
    {
        session_.delayMapP0.push_back(static_cast<std::uint16_t>(session_.LocalInputAt(frame)));
        session_.delayMapP1.push_back(
            static_cast<std::uint16_t>(session_.RemoteInputAtOrHeld(frame)));
    }
}

bool Engine::ServicePendingRollback()
{
    if (pendingRollbackFrame_ == kNoFrame)
    {
        return true;
    }

    const std::uint32_t restoreFrame = pendingRollbackFrame_;
    pendingRollbackFrame_ = kNoFrame;
    if (restoreFrame >= session_.currentFrame)
    {
        return true;
    }

    const std::uint32_t replayTarget = session_.currentFrame;
    if (transitionBlockedFrame_ != kNoFrame && restoreFrame < transitionBlockedFrame_)
    {
        // The earlier correction owns causality: clear the KO hold and replay through it.
        log::Debug(
            "rollback",
            "earlier correction %u supersedes speculative transition hold %u",
            restoreFrame,
            transitionBlockedFrame_);
        transitionBlockedFrame_ = kNoFrame;
    }
    if (!ReloadSavedState(restoreFrame))
    {
        return false;
    }

    // Corrected ancestry: suppress the identical-input self-check until overwritten.
    for (std::uint32_t frame = restoreFrame;; ++frame)
    {
        RollbackFrameSlot& stale = session_.FrameSlot(frame);
        if (stale.occupied && stale.frame == frame)
        {
            stale.hashed = false;
        }
        if (frame < session_.frameHashValid.size())
        {
            session_.frameHashValid[frame] = 0u;
        }
        if (frame == replayTarget)
        {
            break;
        }
    }

    rollbackReplayTarget_ = std::max(rollbackReplayTarget_, replayTarget);
    if (rollbacksTotal_ <= 4u || (rollbacksTotal_ % 256u) == 0u)
    {
        lifecycle::Rollback(
            "rewind",
            "mispredict restore=%u replayTarget=%u span=%u total=%u",
            restoreFrame,
            replayTarget,
            replayTarget - restoreFrame,
            rollbacksTotal_);
    }
    return true;
}

void Engine::ServiceConfirmedFrames()
{
    ScopedPerformanceTimer perfTimer(
        &perf_.hashCalls,
        &perf_.hashTotalUs,
        &perf_.hashMaxUs);
    if (pendingRollbackFrame_ != kNoFrame
        || (rollbackReplayTarget_ != 0u && session_.currentFrame < rollbackReplayTarget_))
    {
        return;
    }

    std::size_t frontier = std::min(
        session_.localInputs.size(), static_cast<std::size_t>(session_.currentFrame));
    if (caster::IsNetplayActive())
    {
        frontier = std::min(frontier, session_.remoteInputs.size());
    }

    while (static_cast<std::size_t>(nextHashFrame_) < frontier)
    {
        if (nextHashFrame_ >= session_.frameHashValid.size()
            || nextHashFrame_ >= session_.frameRegionHashes.size()
            || session_.frameHashValid[nextHashFrame_] == 0u)
        {
            lifecycle::Fault(
                "rollback",
                "confirmed frame %u missing durable corrected hash (frontier=%zu cur=%u validSize=%zu hashSize=%zu)",
                nextHashFrame_,
                frontier,
                session_.currentFrame,
                session_.frameHashValid.size(),
                session_.frameRegionHashes.size());
            log::Error(
                "rollback",
                "confirmed frame %u missing durable corrected hash (frontier=%zu cur=%u validSize=%zu hashSize=%zu)",
                nextHashFrame_,
                frontier,
                session_.currentFrame,
                session_.frameHashValid.size(),
                session_.frameRegionHashes.size());
            if (caster::IsNetplayActive())
            {
                netplay::AbortNetplaySession("corrected confirmed state was unavailable");
            }
            return;
        }

        desync::RecordLocal(
            nextHashFrame_, session_.frameRegionHashes[nextHashFrame_].data(), proto::kFrameHashRegions);
        if (caster::IsNetplayActive())
        {
            netplay::SendFrameHash(
                nextHashFrame_,
                session_.frameRegionHashes[nextHashFrame_].data(),
                proto::kFrameHashRegions);
        }
        ++nextHashFrame_;
    }

    if (caster::IsNetplayActive() && spectator::IsRecordingActive())
    {
        while (static_cast<std::size_t>(nextSpectatorFrame_) < frontier)
        {
            const std::uint32_t frame = nextSpectatorFrame_;
            if (frame >= session_.localInputs.size()
                || frame >= session_.remoteInputs.size()
                || frame >= session_.localSystemInputs.size()
                || frame >= session_.remoteSystemInputs.size()
                || frame >= session_.frameScreens.size()
                || frame >= session_.frameGenerations.size())
            {
                log::Error(
                    "spectator",
                    "corrected recorder history unavailable frame=%u frontier=%zu inputs=%zu/%zu provenance=%zu/%zu",
                    frame,
                    frontier,
                    session_.localInputs.size(),
                    session_.remoteInputs.size(),
                    session_.frameScreens.size(),
                    session_.frameGenerations.size());
                break;
            }
            const auto maskForSide = [&](const int side) -> std::uint8_t
            {
                return side == session_.localSide
                    ? session_.LocalInputAt(frame)
                    : session_.RemoteInputAt(frame);
            };
            const bool hasHashes = frame < session_.frameHashValid.size()
                && frame < session_.frameRegionHashes.size()
                && session_.frameHashValid[frame] != 0u;
            const auto systemForSide = [&](const int side) -> std::uint8_t
            {
                return spectator::ConfirmedSystemStateForCanonicalSideAt(
                    session_.localSystemInputs,
                    session_.remoteSystemInputs,
                    session_.localSide,
                    side,
                    frame);
            };
            if (!spectator::PublishConfirmedFrame(
                    session_.frameGenerations[frame],
                    frame,
                    session_.frameScreens[frame],
                    spectator::PackInputWord(maskForSide(0), systemForSide(0)),
                    spectator::PackInputWord(maskForSide(1), systemForSide(1)),
                    hasHashes ? session_.frameRegionHashes[frame].data() : nullptr,
                    hasHashes ? proto::kFrameHashRegions : 0u))
            {
                break;
            }
            ++nextSpectatorFrame_;
        }
    }

    // A corrected state frontier, not the number of remote inputs received.
    session_.confirmedFrame = nextHashFrame_;
}

void Engine::ServiceConfirmedRecorderFrames()
{
    if (!caster::IsNetplayActive())
    {
        return;
    }

    const std::size_t frontier = std::min(
        {session_.localInputs.size(),
         session_.remoteInputs.size(),
         session_.localSystemInputs.size(),
         session_.remoteSystemInputs.size(),
         static_cast<std::size_t>(session_.currentFrame)});
    while (static_cast<std::size_t>(nextSpectatorFrame_) < frontier)
    {
        const std::uint32_t frame = nextSpectatorFrame_;
        if (frame >= session_.frameScreens.size()
            || frame >= session_.frameGenerations.size())
        {
            log::Error(
                "spectator",
                "frontend recorder provenance unavailable frame=%u screens=%zu generations=%zu frontier=%zu",
                frame,
                session_.frameScreens.size(),
                session_.frameGenerations.size(),
                frontier);
            break;
        }
        const auto actionForSide = [&](const int side) -> std::uint8_t
        {
            return side == session_.localSide
                ? session_.LocalInputAt(frame)
                : session_.RemoteInputAt(frame);
        };
        const auto systemForSide = [&](const int side) -> std::uint8_t
        {
            return spectator::ConfirmedSystemStateForCanonicalSideAt(
                session_.localSystemInputs,
                session_.remoteSystemInputs,
                session_.localSide,
                side,
                frame);
        };

        if (!spectator::PublishConfirmedFrame(
                session_.frameGenerations[frame],
                frame,
                session_.frameScreens[frame],
                spectator::PackInputWord(actionForSide(0), systemForSide(0)),
                spectator::PackInputWord(actionForSide(1), systemForSide(1)),
                nullptr,
                0u))
        {
            // Stop so a recorder discontinuity is not hidden behind later rejected frames.
            break;
        }
        ++nextSpectatorFrame_;
    }
}

int Engine::WaitRemote()
{
    phase_ = TransportPhase::WaitingRemote;

    // delay = 1 + configured frames; the 1 is mandatory or the confirmed floor wedges.
    const int delay = 1 + (session_.inputDelay > 0 ? session_.inputDelay : 0);

    // Build the lead here each tick, not only while advancing.
    const std::size_t leadTarget =
        static_cast<std::size_t>(session_.currentFrame) + static_cast<std::size_t>(delay) + 1u;
    // Cap at peer ACK + retransmit window; the opposite input frontier is not an ACK.
    std::size_t leadCap = leadTarget + 16u;
    if (RequireRemoteInput())
    {
        const std::size_t flowCap =
            static_cast<std::size_t>(session_.peerAckFrame)
            + static_cast<std::size_t>(proto::kMaxQueueSlot);
        leadCap = flowCap < leadCap ? flowCap : leadCap;
    }
    while (session_.localInputs.size() < leadTarget && session_.localInputs.size() < leadCap)
    {
        if (!CaptureAndAppendLocalInput())
        {
            return 0;
        }
    }

    const int localSize = static_cast<int>(session_.localInputs.size());
    const int remoteSize = static_cast<int>(session_.remoteInputs.size());
    int safeFrame = localSize - delay - 1;

    // Lifecycle keys change screens outside the snapshot: wait for the peer's ACK.
    if (RequireRemoteInput() && !session_.localSystemInputs.empty()
        && safeFrame >= static_cast<int>(session_.currentFrame))
    {
        const std::uint32_t end = std::min(
            static_cast<std::uint32_t>(safeFrame),
            static_cast<std::uint32_t>(session_.localSystemInputs.size() - 1u));
        for (std::uint32_t frame = session_.currentFrame; frame <= end; ++frame)
        {
            const bool escRising =
                (session_.LocalSystemStateAt(frame) & menu::kSystemStateEsc) != 0u
                && (frame == 0u
                    || (session_.LocalSystemStateAt(frame - 1u) & menu::kSystemStateEsc) == 0u);
            if (escRising
                && session_.peerAckFrame <= frame)
            {
                safeFrame = static_cast<int>(frame) - 1;
                static std::uint32_t s_systemAckStalls = 0;
                if ((++s_systemAckStalls % 30u) == 1u)
                {
                    lifecycle::Stall("rollback", "system_input_wait_peer_ack");
                    lifecycle::Input(
                        "rollback_system",
                        "hold local system frame=%u peerAck=%u current=%u",
                        frame,
                        session_.peerAckFrame,
                        session_.currentFrame);
                }
                break;
            }
        }
    }

    if (safeFrame >= 0 && safeFrame < remoteSize)
    {
        const int iterations = safeFrame + 1 - static_cast<int>(session_.currentFrame);
        if (iterations <= 0)
        {
            return 0;
        }

        log::Debug(
            "rollback",
            "wait_remote iterations=%d delay=%d local=%d remote=%d safe=%d frame=%u",
            iterations,
            delay,
            localSize,
            remoteSize,
            safeFrame,
            session_.currentFrame);
        return iterations;
    }

    log::Debug(
        "rollback",
        "pause too few remote inputs ping=%u delay=%d local=%d remote=%d safe=%d",
        session_.pingMs,
        delay,
        localSize,
        remoteSize,
        safeFrame);
    return 0;
}

int Engine::AdjustPrediction()
{
    phase_ = TransportPhase::Predicting;

    if (transitionBlockedFrame_ != kNoFrame)
    {
        if (session_.currentFrame != transitionBlockedFrame_)
        {
            lifecycle::Fault(
                "rollback",
                "transition hold moved unexpectedly block=%u cur=%u",
                transitionBlockedFrame_,
                session_.currentFrame);
            if (caster::IsNetplayActive())
            {
                netplay::AbortNetplaySession("speculative transition hold lost alignment");
            }
            return 0;
        }
        if (session_.remoteInputs.size() <= transitionBlockedFrame_)
        {
            log::Debug(
                "rollback",
                "hold speculative transition frame=%u remote=%zu",
                transitionBlockedFrame_,
                session_.remoteInputs.size());
            return 0;
        }
        log::Info(
            "rollback",
            "release speculative transition frame=%u with confirmed remote input",
            transitionBlockedFrame_);
        transitionBlockedFrame_ = kNoFrame;
    }

    if (session_.delayAdjust == 0)
    {
        return WaitRemote();
    }

    if (gameplayEscTransitionActive_)
    {
        return WaitRemote();
    }

    // Same effective delay as WaitRemote, so both paths agree on which frame is safe.
    const int delay = 1 + (session_.inputDelay > 0 ? session_.inputDelay : 0);

    const int remoteSize = static_cast<int>(session_.remoteInputs.size());
    // Compute the lead before capture; after it, both peers self-sustain an extra frame.
    const int leadBeforeCapture =
        static_cast<int>(session_.localInputs.size()) - remoteSize;
    const bool replayingCorrection =
        rollbackReplayTarget_ != 0u && session_.currentFrame < rollbackReplayTarget_;
    {
        const int localSize = static_cast<int>(session_.localInputs.size());
        const int lead = localSize - remoteSize;
        // Do not pause on queue lead: correction ticks would regain state without advancing.
        const int budget = delay + session_.delayAdjust;
        if (lead > budget)
        {
            log::Debug(
                "rollback",
                "input queue lead above prediction budget local=%d remote=%d lead=%d budget=%d",
                localSize,
                remoteSize,
                lead,
                budget);
        }
    }

    // A restore moves currentFrame back; the display tick must still capture new input.
    const std::uint32_t liveCaptureBase = replayingCorrection
        ? std::max(session_.currentFrame, rollbackReplayTarget_)
        : session_.currentFrame;
    const std::size_t leadTarget =
        static_cast<std::size_t>(liveCaptureBase) + static_cast<std::size_t>(delay) + 1u;
    const std::size_t flowCap =
        static_cast<std::size_t>(session_.peerAckFrame)
        + static_cast<std::size_t>(proto::kMaxQueueSlot);
    while (session_.localInputs.size() < leadTarget && session_.localInputs.size() < flowCap)
    {
        if (!CaptureAndAppendLocalInput())
        {
            return 0;
        }
    }

    const int localSize = static_cast<int>(session_.localInputs.size());
    const int current = static_cast<int>(session_.currentFrame);

    if (current + delay <= localSize || current <= static_cast<int>(session_.matchStartFrame))
    {
        if (netplay::ShouldRepeatLocalInputForRemoteAhead(
                leadBeforeCapture,
                session_.pingMs,
                pacing::GetSessionTargetFps())
            && session_.localInputs.size() < flowCap)
        {
            // Re-enqueue the prior word; do not poll input twice in one logical tick.
            if (!AppendRepeatedLocalInput())
            {
                return 0;
            }
        }

        constexpr int kMaxBatchPerTick = 32;
        int iterations =
            static_cast<int>(session_.localInputs.size()) - current - delay;
        const int predictionRoom =
            remoteSize + session_.delayAdjust - static_cast<int>(session_.currentFrame);
        iterations = std::min(iterations, std::max(0, predictionRoom));
        if (replayingCorrection)
        {
            const int replayFrames = static_cast<int>(rollbackReplayTarget_)
                - static_cast<int>(session_.currentFrame);
            iterations = std::max(iterations, replayFrames);
        }

        // ESC changes screen mode outside the snapshot: wait for the peer's ACK.
        if (RequireRemoteInput() && iterations > 0 && !session_.localSystemInputs.empty())
        {
            const std::uint32_t end = std::min(
                session_.currentFrame + static_cast<std::uint32_t>(iterations) - 1u,
                static_cast<std::uint32_t>(session_.localSystemInputs.size() - 1u));
            for (std::uint32_t frame = session_.currentFrame; frame <= end; ++frame)
            {
                const bool escRising =
                    (session_.LocalSystemStateAt(frame) & menu::kSystemStateEsc) != 0u
                    && (frame == 0u
                        || (session_.LocalSystemStateAt(frame - 1u)
                            & menu::kSystemStateEsc) == 0u);
                // ACK proves the peer has the byte; the remote frontier proves no prediction. Need both.
                if (escRising
                    && (session_.peerAckFrame <= frame
                        || session_.remoteInputs.size() <= frame))
                {
                    iterations = static_cast<int>(frame - session_.currentFrame);
                    static std::uint32_t s_predictionSystemAckStalls = 0;
                    if ((++s_predictionSystemAckStalls % 30u) == 1u)
                    {
                        lifecycle::Stall("rollback", "prediction_system_input_wait_peer_ack");
                        lifecycle::Input(
                            "rollback_system",
                            "hold predicted local system frame=%u peerAck=%u remote=%zu current=%u",
                            frame,
                            session_.peerAckFrame,
                            session_.remoteInputs.size(),
                            session_.currentFrame);
                    }
                    break;
                }
            }
        }
        return std::clamp(iterations, 0, kMaxBatchPerTick);
    }

    log::Debug(
        "rollback",
        "pause too little delay local=%d current=%d delay=%d",
        localSize,
        current,
        delay);
    return 0;
}

int Engine::AdvanceSimple(int frameBudget)
{
    int advanced = 0;
    const std::uint32_t pacingAnchor = pacing::CaptureNativeFrameWaitAnchor();
    for (int i = 0; i < frameBudget; ++i)
    {
        const bool hidden = i + 1 < frameBudget;
        SetPresentSuppressed(hidden);
        SetRollbackAudioFrame(session_.currentFrame, true);
        if (hidden)
        {
            pacing::BypassNextNativeFrameWait();
        }
        else if (frameBudget > 1)
        {
            pacing::RestoreNextNativeFrameWaitAnchor(pacingAnchor);
        }
        const int prevMemorial = session_.MapMemorialScreenIndex();
        if (prevMemorial != 0)
        {
            break;
        }

        if (session_.currentFrame >= session_.localInputs.size())
        {
            if (!CaptureAndAppendLocalInput())
            {
                break;
            }
        }

        bool nativeAdvanced = false;
        {
            pacing::ScopedNativeFpsAccountingIsolation fpsAccounting(hidden);
            nativeAdvanced = AdvanceOne();
        }
        if (!nativeAdvanced)
        {
            break;
        }
        ++advanced;

        if (session_.MapMemorialScreenIndex() != prevMemorial)
        {
            break;
        }
    }
    SetPresentSuppressed(false);
    SetRollbackAudioFrame(0u, false);
    return advanced;
}

int Engine::AdvanceBatch(int frameBudget)
{
    int advanced = 0;
    const std::uint32_t pacingAnchor = pacing::CaptureNativeFrameWaitAnchor();
    for (int i = 0; i < frameBudget; ++i)
    {
        const bool hidden = i + 1 < frameBudget;
        SetPresentSuppressed(hidden);
        SetRollbackAudioFrame(session_.currentFrame, true);
        if (hidden)
        {
            pacing::BypassNextNativeFrameWait();
        }
        else if (frameBudget > 1)
        {
            pacing::RestoreNextNativeFrameWaitAnchor(pacingAnchor);
        }
        const int prevMemorial = session_.MapMemorialScreenIndex();
        if (prevMemorial != 1 && prevMemorial != 2)
        {
            break;
        }

        // Unsigned wrap at size 0 skips the capture.
        if (session_.currentFrame >= session_.localInputs.size() - 1u)
        {
            if (!CaptureAndAppendLocalInput())
            {
                break;
            }
        }

        if (IsMatchRollbackActive() && !SaveInputs())
        {
            lifecycle::Fault(
                "rollback",
                "confirmed match-transition snapshot unavailable frame=%u",
                session_.currentFrame);
            if (caster::IsNetplayActive())
            {
                netplay::AbortNetplaySession("confirmed match-transition snapshot failed");
            }
            break;
        }

        bool nativeAdvanced = false;
        {
            pacing::ScopedNativeFpsAccountingIsolation fpsAccounting(hidden);
            nativeAdvanced = AdvanceOne();
        }
        if (!nativeAdvanced)
        {
            break;
        }
        ++advanced;

        if (session_.MapMemorialScreenIndex() != prevMemorial)
        {
            break;
        }
    }
    SetPresentSuppressed(false);
    SetRollbackAudioFrame(0u, false);
    return advanced;
}

int Engine::AdvanceUntilInput(int frameBudget)
{
    int advanced = 0;
    const std::uint32_t pacingAnchor = pacing::CaptureNativeFrameWaitAnchor();
    while (advanced < frameBudget)
    {
        const bool hidden = advanced + 1 < frameBudget;
        SetPresentSuppressed(hidden);
        SetRollbackAudioFrame(session_.currentFrame, true);
        if (hidden)
        {
            pacing::BypassNextNativeFrameWait();
        }
        else if (frameBudget > 1)
        {
            pacing::RestoreNextNativeFrameWaitAnchor(pacingAnchor);
        }
        const int prevMemorial = session_.MapMemorialScreenIndex();

        if (session_.currentFrame >= session_.localInputs.size())
        {
            if (!CaptureAndAppendLocalInput())
            {
                break;
            }
        }

        if (session_.currentFrame >= session_.remoteInputs.size())
        {
            break;
        }

        if (IsMatchRollbackActive() && !SaveInputs())
        {
            lifecycle::Fault(
                "rollback",
                "confirmed match-transition snapshot unavailable frame=%u",
                session_.currentFrame);
            if (caster::IsNetplayActive())
            {
                netplay::AbortNetplaySession("confirmed match-transition snapshot failed");
            }
            break;
        }

        bool nativeAdvanced = false;
        {
            pacing::ScopedNativeFpsAccountingIsolation fpsAccounting(hidden);
            nativeAdvanced = AdvanceOne();
        }
        if (!nativeAdvanced)
        {
            break;
        }
        ++advanced;

        if (session_.MapMemorialScreenIndex() != prevMemorial)
        {
            break;
        }
    }
    SetPresentSuppressed(false);
    SetRollbackAudioFrame(0u, false);
    return advanced;
}

void Engine::BatchRender(int frameCount)
{
    if (!IsPredictionScreen())
    {
        return;
    }

    if (frameCount <= 0)
    {
        // Do not restore on zero-work polls: replaying the same window turns ping into slow-mo.
        phase_ = TransportPhase::Idle;
        return;
    }

    phase_ = TransportPhase::BatchCatchUp;
    lastBatchFrames_ = static_cast<std::uint32_t>(frameCount);
    perf_.maxBatch = std::max(perf_.maxBatch, frameCount);
    drivingCatchUp_ = true;
    const std::uint32_t pacingAnchor = pacing::CaptureNativeFrameWaitAnchor();

    {
        const bool rr = RequireRemoteInput();
        const std::size_t frontierNow = rr
            ? std::min(session_.localInputs.size(), session_.remoteInputs.size())
            : session_.localInputs.size();
        log::Debug(
            "rollback",
            "batch n=%d cur=%u replayTarget=%u confirmed=%u frontier=%zu local=%zu remote=%zu predDepth=%d adj=%d",
            frameCount,
            session_.currentFrame,
            rollbackReplayTarget_,
            session_.confirmedFrame,
            frontierNow,
            session_.localInputs.size(),
            session_.remoteInputs.size(),
            static_cast<int>(session_.currentFrame) + frameCount - static_cast<int>(frontierNow),
            session_.delayAdjust);
    }

    const auto exitBatch = [&]() {
        SetPresentSuppressed(false);
        SetRollbackAudioFrame(0u, false);
        drivingCatchUp_ = false;
        phase_ = TransportPhase::Idle;
    };

    struct TransitionGuard
    {
        std::uint8_t mode = 0;
        std::uint32_t subState = 0;
        std::int32_t animCounter = 0;
        std::uint8_t winCount = 0;
        std::uint8_t winnerSide = 0;
    };
    auto* const modePtr = static_cast<std::uint8_t*>(trial::ResolveVa(trial::kVaBattleScreenMode));
    auto* const subPtr = static_cast<std::uint32_t*>(trial::ResolveVa(trial::kVaScreenSubState));
    auto* const animPtr = static_cast<std::int32_t*>(trial::ResolveVa(trial::kVaScreenAnimCounter));
    auto* const winPtr = static_cast<std::uint8_t*>(trial::ResolveVa(trial::kVaWinCount));
    auto* const winnerSidePtr = static_cast<std::uint8_t*>(trial::ResolveVa(trial::kVaCharSelectSide));
    const bool guardResolvable =
        modePtr != nullptr && subPtr != nullptr && animPtr != nullptr && winPtr != nullptr
        && winnerSidePtr != nullptr;

    for (int i = 0; i < frameCount; ++i)
    {
        if (!IsPredictionScreen())
        {
            exitBatch();
            return;
        }

        const std::uint32_t frame = session_.currentFrame;
        const bool requireRemote = RequireRemoteInput();
        if (session_.HasFrameInputs(frame, requireRemote))
        {
            session_.delayMapP0.clear();
        }

        const std::size_t confirmedFrontier = requireRemote
            ? std::min(session_.localInputs.size(), session_.remoteInputs.size())
            : session_.localInputs.size();
        const bool predictedFrame = static_cast<std::size_t>(frame) >= confirmedFrontier;

        TransitionGuard guard{};
        if (predictedFrame && guardResolvable)
        {
            guard.mode = *modePtr;
            guard.subState = *subPtr;
            guard.animCounter = *animPtr;
            guard.winCount = *winPtr;
            guard.winnerSide = *winnerSidePtr;
        }

        SetPresentSuppressed(i != frameCount - 1);
        SetRollbackAudioFrame(frame, true);
        if (i != frameCount - 1)
        {
            pacing::BypassNextNativeFrameWait();
        }
        else if (frameCount > 1)
        {
            pacing::RestoreNextNativeFrameWaitAnchor(pacingAnchor);
        }

        if (!SaveInputs())
        {
            lifecycle::Fault(
                "rollback",
                "could not save pre-frame snapshot frame=%u local=%zu remote=%zu",
                frame,
                session_.localInputs.size(),
                session_.remoteInputs.size());
            if (caster::IsNetplayActive())
            {
                netplay::AbortNetplaySession("pre-frame rollback snapshot failed");
            }
            exitBatch();
            return;
        }
        if (caster::IsNetplayActive() && desync::ResimDivergences() != 0u)
        {
            ServiceDesyncDump();
            netplay::AbortNetplaySession("rollback re-simulation became non-deterministic");
            exitBatch();
            return;
        }
        const int prevMemorial = session_.MapMemorialScreenIndex();
        const bool hiddenFrame = i != frameCount - 1;
        const std::uint64_t nativeStart = PerformanceNow();
        bool advanced = false;
        {
            pacing::ScopedNativeFpsAccountingIsolation fpsAccounting(hiddenFrame);
            advanced = AdvanceOne();
        }
        const std::uint64_t nativeUs = PerformanceElapsedUs(nativeStart);
        ++perf_.nativeCalls;
        perf_.nativeTotalUs += nativeUs;
        perf_.nativeMaxUs = std::max(perf_.nativeMaxUs, nativeUs);
        if (hiddenFrame)
        {
            ++perf_.hiddenNativeCalls;
            perf_.hiddenNativeUs += nativeUs;
            if (nativeUs >= 4000u)
            {
                ++perf_.slowHiddenCalls;
            }
        }
        else
        {
            ++perf_.visibleNativeCalls;
            perf_.visibleNativeUs += nativeUs;
        }
        if (predictedFrame)
        {
            ++perf_.predictedFrames;
        }
        else
        {
            ++perf_.confirmedFrames;
        }

        if (!advanced)
        {
            lifecycle::Fault("rollback", "native match advance failed frame=%u", frame);
            if (caster::IsNetplayActive())
            {
                netplay::AbortNetplaySession("native battle tick could not advance");
            }
            exitBatch();
            return;
        }

        const std::uint32_t advancedFrame = session_.currentFrame;
        if (advancedFrame > session_.peakAdvanceFrame)
        {
            session_.peakAdvanceFrame = advancedFrame;
            session_.delayMapP1.clear();
        }

        if (session_.MapMemorialScreenIndex() != prevMemorial)
        {
            if (predictedFrame && guardResolvable)
            {
                ++perf_.transitionVetoes;
                // No native-handler transition may run speculatively, including loading substates.
                *modePtr = guard.mode;
                *subPtr = guard.subState;
                *animPtr = guard.animCounter;
                *winPtr = guard.winCount;
                *winnerSidePtr = guard.winnerSide;
                lifecycle::Rollback(
                    "spec_transition_veto",
                    "predicted frame %u ended match — exact rewind and hold",
                    frame);
                log::Warn(
                    "rollback",
                    "speculative match exit on predicted frame %u VETOED (exact hold)",
                    frame);
                if (ReloadSavedState(frame))
                {
                    transitionBlockedFrame_ = frame;
                    rollbackReplayTarget_ = 0;
                }
            }
            exitBatch();
            return;
        }

        if (i == frameCount - 1)
        {
            log::Debug("rollback", "render frame %u", session_.currentFrame);
        }
    }

    if (rollbackReplayTarget_ != 0u && session_.currentFrame >= rollbackReplayTarget_)
    {
        rollbackReplayTarget_ = 0;
    }
    exitBatch();
}

void Engine::ServiceDesyncDump()
{
    std::uint32_t frame = 0;
    std::uint32_t mask = 0;
    if (!desync::TakeDumpRequest(&frame, &mask))
    {
        return;
    }

    const RollbackFrameSlot& slot = session_.FrameSlot(frame);
    if (!slot.occupied || slot.frame != frame || slot.battle.size() < trial::kDynamicBattleSize)
    {
        log::Warn(
            "desync",
            "byte-dump f=%u skipped — ring slot unavailable (occupied=%d slotFrame=%u)",
            frame,
            slot.occupied ? 1 : 0,
            slot.frame);
        return;
    }

    static const char* const kNames[] = {"p1_core", "p1_anim", "p2_block", "p2_anim", "globals"};
    std::size_t off = 0;
    for (int r = 0; r < trial::kDynamicRegionCount; ++r)
    {
        const std::size_t sz = trial::kDynamicBattleRegions[r].size;
        if ((mask & (1u << r)) != 0u)
        {
            const auto* const data = slot.battle.data() + off;
            const std::uint32_t va = static_cast<std::uint32_t>(trial::kDynamicBattleRegions[r].va);
            log::Critical("desync", "=== BYTE DUMP f=%u region=%s va=0x%08X size=%zu ===", frame, kNames[r], va, sz);

            for (std::size_t c = 0; c < sz; c += 256u)
            {
                const std::size_t clen = (c + 256u <= sz) ? 256u : (sz - c);
                log::Critical("desync", "  chunk va=0x%08X crc=0x%08X", static_cast<unsigned>(va + c),
                    savestate_digest::Crc32(data + c, clen));
            }

            for (std::size_t i = 0; i < sz; i += 16u)
            {
                char line[96];
                int p = std::snprintf(line, sizeof(line), "  +0x%04zX:", i);
                for (std::size_t j = 0; j < 16u && i + j < sz && p > 0 && static_cast<std::size_t>(p) < sizeof(line) - 4u; ++j)
                {
                    p += std::snprintf(line + p, sizeof(line) - static_cast<std::size_t>(p), " %02X", data[i + j]);
                }
                log::Critical("desync", "%s", line);
            }
            log::Critical("desync", "=== END BYTE DUMP f=%u region=%s ===", frame, kNames[r]);
        }
        off += sz;
    }
    if ((mask & (1u << trial::kDynamicRegionCount)) != 0u)
    {
        log::Critical(
            "desync",
            "DETERMINISM DUMP f=%u fpuCrc=%08X rng=%08X fpuCW=%02X%02X",
            frame,
            savestate_digest::Crc32(slot.fpu.data(), slot.fpu.size()),
            slot.rngShadow,
            static_cast<unsigned>(slot.fpu[1]),
            static_cast<unsigned>(slot.fpu[0]));
    }
    if ((mask & (1u << (trial::kDynamicRegionCount + 1))) != 0u)
    {
        char auxHex[3 * trial::kAuxStateSize + 1] = {};
        std::size_t pos = 0;
        for (const std::uint8_t value : slot.shakeAux)
        {
            const int written = std::snprintf(
                auxHex + pos,
                sizeof(auxHex) - pos,
                "%s%02X",
                pos != 0u ? " " : "",
                static_cast<unsigned>(value));
            if (written <= 0)
            {
                break;
            }
            pos += static_cast<std::size_t>(written);
        }
        log::Critical("desync", "SIM AUX DUMP f=%u bytes=%s", frame, auxHex);
    }
    log::FlushFile();
}

void Engine::EmitPerformanceSample(const std::uint64_t mainTickUs, const int iterations)
{
    const std::uint32_t nowMs = static_cast<std::uint32_t>(GetTickCount());
    if (perf_.startMs == 0u)
    {
        perf_.startMs = nowMs;
    }
    ++perf_.mainCalls;
    perf_.mainTotalUs += mainTickUs;
    perf_.mainMaxUs = std::max(perf_.mainMaxUs, mainTickUs);
    if (iterations <= 0)
    {
        ++perf_.zeroWorkCalls;
    }

    const std::uint32_t elapsedMs = nowMs - perf_.startMs;
    if (elapsedMs < 1000u)
    {
        return;
    }

    const auto average = [](const std::uint64_t total, const std::uint64_t count) {
        return count != 0u ? total / count : 0u;
    };
    const double simHz = static_cast<double>(perf_.nativeCalls) * 1000.0
        / static_cast<double>(elapsedMs);
    const double visibleHz = static_cast<double>(perf_.visibleNativeCalls) * 1000.0
        / static_cast<double>(elapsedMs);
    const double outerHz = static_cast<double>(perf_.mainCalls) * 1000.0
        / static_cast<double>(elapsedMs);
    const int localLead = static_cast<int>(session_.localInputs.size())
        - static_cast<int>(session_.currentFrame);
    const int remoteLead = static_cast<int>(session_.remoteInputs.size())
        - static_cast<int>(session_.currentFrame);
    const std::size_t confirmedFrontier = std::min(
        session_.localInputs.size(), session_.remoteInputs.size());
    const int predictionDepth = static_cast<int>(session_.currentFrame)
        - static_cast<int>(confirmedFrontier);
    const auto* const mode = static_cast<const std::uint8_t*>(
        trial::ResolveVa(trial::kVaBattleScreenMode));
    const auto* const subState = static_cast<const std::int32_t*>(
        trial::ResolveVa(trial::kVaScreenSubState));
    const auto* const counter = static_cast<const std::int32_t*>(
        trial::ResolveVa(trial::kVaScreenAnimCounter));

    log::Info(
        "rollback.perf",
        "PERF dt=%ums outer=%.1fHz sim=%.1fHz visible=%.1fHz zero=%llu/%llu "
        "main[avg=%lluus max=%lluus] save[n=%llu avg=%lluus max=%lluus] "
        "native[n=%llu avg=%lluus max=%lluus hidden=%llu/%lluus slow=%llu visible=%llu/%lluus] "
        "hash[n=%llu avg=%lluus max=%lluus] flow[f=%u L=%+d R=%+d pred=%+d pending=%u] "
        "batch[max=%d pred=%llu conf=%llu veto=%llu] raw[%u/%d/%d]",
        elapsedMs,
        outerHz,
        simHz,
        visibleHz,
        static_cast<unsigned long long>(perf_.zeroWorkCalls),
        static_cast<unsigned long long>(perf_.mainCalls),
        static_cast<unsigned long long>(average(perf_.mainTotalUs, perf_.mainCalls)),
        static_cast<unsigned long long>(perf_.mainMaxUs),
        static_cast<unsigned long long>(perf_.saveCalls),
        static_cast<unsigned long long>(average(perf_.saveTotalUs, perf_.saveCalls)),
        static_cast<unsigned long long>(perf_.saveMaxUs),
        static_cast<unsigned long long>(perf_.nativeCalls),
        static_cast<unsigned long long>(average(perf_.nativeTotalUs, perf_.nativeCalls)),
        static_cast<unsigned long long>(perf_.nativeMaxUs),
        static_cast<unsigned long long>(perf_.hiddenNativeCalls),
        static_cast<unsigned long long>(average(perf_.hiddenNativeUs, perf_.hiddenNativeCalls)),
        static_cast<unsigned long long>(perf_.slowHiddenCalls),
        static_cast<unsigned long long>(perf_.visibleNativeCalls),
        static_cast<unsigned long long>(average(perf_.visibleNativeUs, perf_.visibleNativeCalls)),
        static_cast<unsigned long long>(perf_.hashCalls),
        static_cast<unsigned long long>(average(perf_.hashTotalUs, perf_.hashCalls)),
        static_cast<unsigned long long>(perf_.hashMaxUs),
        session_.currentFrame,
        localLead,
        remoteLead,
        predictionDepth,
        pendingRollbackFrame_,
        perf_.maxBatch,
        static_cast<unsigned long long>(perf_.predictedFrames),
        static_cast<unsigned long long>(perf_.confirmedFrames),
        static_cast<unsigned long long>(perf_.transitionVetoes),
        mode != nullptr ? static_cast<unsigned>(*mode) : 0xFFu,
        subState != nullptr ? *subState : -1,
        counter != nullptr ? *counter : -1);

    perf_ = {};
    perf_.startMs = nowMs;
}

void Engine::MainTick()
{
    const std::uint64_t mainTickStart = PerformanceNow();
    ServiceDesyncDump();

    // Never continue an online match after determinism has been disproved.
    if (caster::IsNetplayActive()
        && (desync::Mismatches() != 0u || desync::ResimDivergences() != 0u))
    {
        lastMainTickIterations_ = 0;
        netplay::AbortNetplaySession("confirmed deterministic state divergence");
        return;
    }

    const int memorial = session_.MapMemorialScreenIndex();
    const auto* const modePtr =
        static_cast<const std::uint8_t*>(trial::ResolveVa(trial::kVaBattleScreenMode));
    const std::uint8_t trialMode = modePtr != nullptr ? *modePtr : 0xFFu;

    session_.memorialIndex = memorial;
    session_.rollbackSubState = session_.MapTrialScreenSubState();

    if (memorial != session_.prevMemorialIndex)
    {
        HandleMemorialTransition(memorial, session_.prevMemorialIndex);

        const char* dispatch = "until_input";
        switch (memorial)
        {
        case 0:
            dispatch = "advance_simple";
            break;
        case 1:
            dispatch = "batch";
            break;
        case 2:
            dispatch = "batch";
            break;
        case 3:
            dispatch = "render";
            break;
        default:
            dispatch = "until_input";
            break;
        }

        log::Step(
            "rollback",
            "memorial index %d (trial mode=%u) dispatch=%s matchRollback=%d",
            memorial,
            static_cast<unsigned>(trialMode),
            dispatch,
            IsMatchRollbackActive() ? 1 : 0);

        if (trialMode == trial::kScreenLoading && memorial == 2)
        {
            const auto* const subPtr =
                static_cast<const std::uint32_t*>(trial::ResolveVa(trial::kVaScreenSubState));
            const auto* const animPtr =
                static_cast<const std::int32_t*>(trial::ResolveVa(trial::kVaScreenAnimCounter));
            log::Info(
                "rollback",
                "loading intro CCDBC0=%u CCDBBC=%d frame=%u local=%zu remote=%zu memorial=%d",
                subPtr != nullptr ? *subPtr : 0u,
                animPtr != nullptr ? *animPtr : -1,
                session_.currentFrame,
                session_.localInputs.size(),
                session_.remoteInputs.size(),
                memorial);
        }
    }

    ServiceLiveDelayHotkeys(memorial);

    int iterations = 0;
    const bool matchRollbackActive = IsMatchRollbackActive();
    const bool predictionActive = IsPredictionScreen();
    if (predictionActive)
    {
        if (ServicePendingRollback())
        {
            iterations = AdjustPrediction();
            BatchRender(iterations);
            if (caster::IsNetplayActive()
                && (desync::Mismatches() != 0u || desync::ResimDivergences() != 0u))
            {
                netplay::AbortNetplaySession("confirmed deterministic state divergence");
            }
        }
    }
    else
    {
    switch (memorial)
    {
    case 0:
        iterations = AdvanceSimple(WaitRemote());
        break;
    case 1:
        iterations = AdvanceBatch(WaitRemote());
        break;
    case 2:
        iterations = AdvanceBatch(WaitRemote());
        break;
    case 3:
    {
        if (!ServicePendingRollback())
        {
            iterations = 0;
            break;
        }
        iterations = AdjustPrediction();
        BatchRender(iterations);
        if (caster::IsNetplayActive()
            && (desync::Mismatches() != 0u || desync::ResimDivergences() != 0u))
        {
            netplay::AbortNetplaySession("confirmed deterministic state divergence");
        }
        break;
    }
    default:
        iterations = AdvanceUntilInput(WaitRemote());
        break;
    }
    }

    if (matchRollbackActive)
    {
        ServiceConfirmedFrames();
    }
    else
    {
        ServiceConfirmedRecorderFrames();
    }

    lastMainTickIterations_ = iterations;
    session_.prevSubState = session_.rollbackSubState;

    EmitPerformanceSample(PerformanceElapsedUs(mainTickStart), iterations);

    log::Debug("rollback", "iterations: %d memorial=%d", iterations, memorial);
}

void Engine::SetGameWindow(const HWND gameWindow)
{
    if (gameWindow != nullptr)
    {
        lastGameWindow_ = gameWindow;
    }
}

void Engine::OnBattleFrameBoundary(HWND gameWindow)
{
    SetGameWindow(gameWindow);

    if (drivingCatchUp_)
    {
        return;
    }

    const int memorial = session_.MapMemorialScreenIndex();
    if (memorial != 2 && memorial != 3)
    {
        return;
    }

    if (!IsInputAuthorityActive())
    {
        EnsureInputsCapturedForFrame(RequireRemoteInput());
        return;
    }

    if (memorial == 3)
    {
        SaveInputs();
    }
}

int Engine::GetLastMainTickIterations() const
{
    return lastMainTickIterations_;
}

}
