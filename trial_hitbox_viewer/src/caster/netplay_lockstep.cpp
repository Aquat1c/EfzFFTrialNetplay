#include "netplay_lockstep.h"

#include "caster_input.h"
#include "debug_log.h"
#include "game_pacing.h"
#include "lifecycle_log.h"
#include "menu_nav.h"
#include "netplay_delay.h"
#include "netplay_sync.h"
#include "spectator_relay.h"
#include "trial_addresses.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <vector>

namespace thv::netplay::lockstep
{

// Recursive leaf lock over all histories; never hold across another module's mutex.
std::recursive_mutex g_historyMutex;

namespace
{

constexpr int kRing = proto::kMaxQueueSlot;
// A conforming sender can never be more than one complete retransmit window ahead.
constexpr int kMaxRemoteLeadFrames = proto::kMaxQueueSlot;

std::vector<proto::PlayerInputRecord> g_localHistory{};
std::vector<proto::PlayerInputRecord> g_remoteHistory{};
// Captured at consumption, not publication, so backlog keeps its own screen stamp.
std::vector<std::uint8_t> g_consumedScreens{};
std::vector<std::uint32_t> g_consumedGenerations{};
std::uint32_t g_simFrame = 0;
std::uint32_t g_pingMs = 0;
std::uint32_t g_peerAckFrame = 0;
constexpr std::uint32_t kNoSpectatorFrame = std::numeric_limits<std::uint32_t>::max();
std::uint32_t g_nextSpectatorFrame = kNoSpectatorFrame;
Stats g_stats{};

std::atomic<std::uint8_t> g_localSystem{0};
std::atomic<std::uint8_t> g_remoteSystem{0};

void TrimHistoriesTo(std::uint32_t frameCount)
{
    if (g_localHistory.size() > frameCount)
    {
        g_localHistory.resize(frameCount);
    }
    if (g_remoteHistory.size() > frameCount)
    {
        g_remoteHistory.resize(frameCount);
    }
    if (g_consumedScreens.size() > frameCount)
    {
        g_consumedScreens.resize(frameCount);
    }
    if (g_consumedGenerations.size() > frameCount)
    {
        g_consumedGenerations.resize(frameCount);
    }
    if (g_simFrame > frameCount)
    {
        g_simFrame = frameCount;
    }
    if (g_peerAckFrame > frameCount)
    {
        g_peerAckFrame = frameCount;
    }
    g_stats.localFrames = static_cast<std::uint32_t>(g_localHistory.size());
    g_stats.remoteFrames = static_cast<std::uint32_t>(g_remoteHistory.size());
    g_stats.simFrame = g_simFrame;
}

int WaitLoopDelayFrames()
{
    return static_cast<int>(
        PingHalfPeriodCeilFrames(g_pingMs, pacing::GetSessionTargetFps()));
}

int WaitLoopWarmupTarget()
{
    return WaitLoopDelayFrames() + 1;
}

void ServiceConfirmedSpectatorFramesLocked()
{
    if (g_nextSpectatorFrame == kNoSpectatorFrame || !spectator::IsRecordingActive())
    {
        return;
    }

    const std::size_t frontier = std::min(
        {g_localHistory.size(),
         g_remoteHistory.size(),
         static_cast<std::size_t>(g_simFrame),
         g_consumedScreens.size(),
         g_consumedGenerations.size()});

    while (static_cast<std::size_t>(g_nextSpectatorFrame) < frontier)
    {
        const proto::PlayerInputRecord& local =
            g_localHistory[static_cast<std::size_t>(g_nextSpectatorFrame)];
        const proto::PlayerInputRecord& remote =
            g_remoteHistory[static_cast<std::size_t>(g_nextSpectatorFrame)];
        const int localSide = caster::GetLocalSide();
        const proto::PlayerInputRecord& p1 = localSide == 0 ? local : remote;
        const proto::PlayerInputRecord& p2 = localSide == 0 ? remote : local;
        if (!spectator::PublishConfirmedFrame(
                g_consumedGenerations[static_cast<std::size_t>(g_nextSpectatorFrame)],
                g_nextSpectatorFrame,
                g_consumedScreens[static_cast<std::size_t>(g_nextSpectatorFrame)],
                spectator::PackInputWord(p1.playerAction, p1.systemState),
                spectator::PackInputWord(p2.playerAction, p2.systemState),
                nullptr,
                0u))
        {
            break;
        }
        ++g_nextSpectatorFrame;
    }
}

}

std::uint32_t ActiveWireFrameIndex()
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    if (g_simFrame == 0u)
    {
        return 0u;
    }

    return g_simFrame - 1u;
}

void Reset()
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    g_localHistory.clear();
    g_remoteHistory.clear();
    g_consumedScreens.clear();
    g_consumedGenerations.clear();
    g_simFrame = 0;
    g_peerAckFrame = 0;
    g_nextSpectatorFrame = kNoSpectatorFrame;
    g_stats = {};
    g_localSystem.store(0);
    g_remoteSystem.store(0);
    lifecycle::Sync("lockstep", "reset");
    log::Step("lockstep", "reset");
}

void ResetFrontendEpoch(const char* reason)
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    const std::uint32_t aligned =
        static_cast<std::uint32_t>(std::min({g_localHistory.size(), g_remoteHistory.size(), static_cast<std::size_t>(g_simFrame)}));
    TrimHistoriesTo(aligned);
    lifecycle::Sync(
        "lockstep",
        "frontend epoch reset (%s) aligned=%u local=%zu remote=%zu sim=%u",
        reason != nullptr ? reason : "?",
        aligned,
        g_localHistory.size(),
        g_remoteHistory.size(),
        g_simFrame);
    log::Step(
        "lockstep",
        "frontend epoch reset (%s) aligned=%u local=%zu remote=%zu sim=%u",
        reason != nullptr ? reason : "?",
        aligned,
        g_localHistory.size(),
        g_remoteHistory.size(),
        g_simFrame);
}

void OnScreenTransition(const std::uint8_t prevMode, const std::uint8_t nextMode)
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    if (prevMode == nextMode)
    {
        return;
    }

    const bool leavingBattle = prevMode == trial::kScreenBattle && nextMode != trial::kScreenBattle;
    const bool charSelectEdge =
        (prevMode == trial::kScreenCharSelect && nextMode == trial::kScreenLoading)
        || (nextMode == trial::kScreenCharSelect
            && (prevMode == trial::kScreenLoading || prevMode == trial::kScreenBattle));
    const bool enteringBattle =
        prevMode != trial::kScreenBattle && nextMode == trial::kScreenBattle;

    if (leavingBattle)
    {
        ResetFrontendEpoch("battle_exit");
        return;
    }

    if (charSelectEdge)
    {
        ResetFrontendEpoch(nextMode == trial::kScreenCharSelect ? "charsel_entry" : "charsel_exit");
        return;
    }

    if (enteringBattle)
    {
        lifecycle::Netplay("screen -> battle AlignForBattleHandoff");
        log::Step(
            "lockstep",
            "screen -> battle (begin_match follows synchronized loading handoff)");
        AlignForBattleHandoff();
    }
}

bool HasWireInputsForFrame(const std::uint32_t frame)
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    return frame < g_localHistory.size() && frame < g_remoteHistory.size();
}

void SetPingMs(const std::uint32_t pingMs)
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    g_pingMs = pingMs;
    log::Info(
        "lockstep",
        "ping=%u ms pingFrames=%u effectiveDelay=%u target_fps=%u",
        pingMs,
        PingHalfPeriodCeilFrames(pingMs, pacing::GetSessionTargetFps()),
        PingHalfPeriodCeilFrames(pingMs, pacing::GetSessionTargetFps()),
        static_cast<unsigned>(pacing::GetSessionTargetFps()));
}

std::uint32_t GetPingMs()
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    return g_pingMs;
}

std::uint32_t GetWaitLoopDelayFrames()
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    return PingHalfPeriodCeilFrames(g_pingMs, pacing::GetSessionTargetFps());
}

std::uint32_t GetLocalHistorySize()
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    return static_cast<std::uint32_t>(g_localHistory.size());
}

std::uint32_t GetRemoteHistorySize()
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    return static_cast<std::uint32_t>(g_remoteHistory.size());
}

std::uint32_t GetPeerAckFrame()
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    return g_peerAckFrame;
}

void OnLocalInput(const proto::PlayerInputRecord& record)
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    proto::PlayerInputRecord filtered = record;
    filtered.systemState = menu::FilterOnlineSystemState(filtered.systemState);
    g_localHistory.push_back(filtered);
    ++g_stats.localFrames;
    g_localSystem.store(filtered.systemState);
}

void FillOutgoingPacket(proto::SyncDataPacket* out)
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    if (out == nullptr)
    {
        return;
    }

    out->controlWord = proto::kActionSyncData;
    const std::uint32_t historySize = static_cast<std::uint32_t>(g_localHistory.size());
    out->currentFrame = historySize;
    out->ackFrame = static_cast<std::uint32_t>(g_remoteHistory.size());
    out->actionData = {};

    if (historySize == 0u)
    {
        return;
    }

    // SyncData layout: frontier + trailing window (oldest first).
    const std::uint32_t count = std::min(historySize, static_cast<std::uint32_t>(kRing));
    const std::uint32_t windowStart = historySize - count;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        out->actionData[i] = g_localHistory[static_cast<std::size_t>(windowStart + i)];
    }
}

bool ApplyIncomingPacket(const proto::SyncDataPacket& packet)
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    const std::uint32_t remoteEnd = packet.currentFrame;
    const std::uint32_t prevSize = static_cast<std::uint32_t>(g_remoteHistory.size());
    const std::uint32_t localSize = static_cast<std::uint32_t>(g_localHistory.size());
    // An in-flight ACK may exceed the trimmed local frontier: clamp it.
    const std::uint32_t boundedAck = std::min(packet.ackFrame, localSize);
    g_peerAckFrame = std::max(
        g_peerAckFrame,
        boundedAck);
    g_stats.peerAckFrame = g_peerAckFrame;

    if (remoteEnd == 0u)
    {
        return true;
    }

    if (remoteEnd <= prevSize)
    {
        return true;
    }

    if (static_cast<std::uint64_t>(remoteEnd)
        > static_cast<std::uint64_t>(localSize) + static_cast<std::uint64_t>(kMaxRemoteLeadFrames))
    {
        lifecycle::Sync(
            "lockstep",
            "reject remote lead remoteEnd=%u local=%d maxLead=%d",
            remoteEnd,
            static_cast<int>(localSize),
            kMaxRemoteLeadFrames);
        log::Warn(
            "lockstep",
            "reject remote lead remoteEnd=%u local=%d maxLead=%d",
            remoteEnd,
            static_cast<int>(localSize),
            kMaxRemoteLeadFrames);
        return false;
    }

    const std::uint32_t windowCount = std::min(remoteEnd, static_cast<std::uint32_t>(kRing));
    const std::uint32_t windowStart = remoteEnd - windowCount;

    g_remoteHistory.reserve(static_cast<std::size_t>(remoteEnd));
    // Never pad an unfillable gap: padding is a silent desync.
    if (g_remoteHistory.size() < windowStart)
    {
        const std::size_t gapCount = windowStart - g_remoteHistory.size();
        lifecycle::Fault(
            "lockstep",
            "unfillable remote history gap %zu -> %u (%zu lost frames) — rejecting, sync is lost",
            g_remoteHistory.size(),
            windowStart,
            gapCount);
        log::Error(
            "lockstep",
            "unfillable remote history gap %zu -> %u (%zu lost frames) — rejecting, sync is lost",
            g_remoteHistory.size(),
            windowStart,
            gapCount);
        return false;
    }

    for (std::uint32_t frame = std::max(prevSize, windowStart); frame < remoteEnd; ++frame)
    {
        const std::size_t packetIndex = static_cast<std::size_t>(frame - windowStart);
        if (packetIndex >= static_cast<std::size_t>(kRing))
        {
            return false;
        }

        proto::PlayerInputRecord incoming = packet.actionData[packetIndex];
        const std::uint8_t receivedSystemState = incoming.systemState;
        incoming.systemState = menu::FilterOnlineSystemState(incoming.systemState);
        if (receivedSystemState != incoming.systemState)
        {
            lifecycle::Input(
                "lockstep_system",
                "masked remote system state raw=0x%02X allowed=0x%02X frame=%u",
                static_cast<unsigned>(receivedSystemState),
                static_cast<unsigned>(incoming.systemState),
                frame);
        }
        if (frame < static_cast<std::uint32_t>(g_remoteHistory.size()))
        {
            const proto::PlayerInputRecord& existing = g_remoteHistory[static_cast<std::size_t>(frame)];
            if (existing.playerAction != incoming.playerAction || existing.systemState != incoming.systemState)
            {
                lifecycle::Sync(
                    "lockstep",
                    "remote history mismatch frame=%u prev=0x%02X incoming=0x%02X",
                    frame,
                    static_cast<unsigned>(existing.playerAction),
                    static_cast<unsigned>(incoming.playerAction));
                log::Warn(
                    "lockstep",
                    "remote history mismatch frame=%u prev=0x%02X incoming=0x%02X",
                    frame,
                    static_cast<unsigned>(existing.playerAction),
                    static_cast<unsigned>(incoming.playerAction));
            }
            continue;
        }

        g_remoteHistory.push_back(incoming);
        g_remoteSystem.store(incoming.systemState);
    }

    ++g_stats.packetsApplied;
    g_stats.remoteFrames = static_cast<std::uint32_t>(g_remoteHistory.size());
    if (g_stats.packetsApplied <= 4u || (g_stats.packetsApplied % 256u) == 0u
        || remoteEnd - prevSize > 1u)
    {
        lifecycle::Sync(
            "lockstep",
            "applied remote batch +%u remoteSize=%u localSize=%u sim=%u delay=%d",
            remoteEnd - prevSize,
            g_stats.remoteFrames,
            static_cast<std::uint32_t>(g_localHistory.size()),
            g_simFrame,
            WaitLoopDelayFrames());
    }
    log::Debug(
        "lockstep",
        "applied remote batch frames=%u remoteSize=%u localSize=%u sim=%u delay=%d",
        remoteEnd - prevSize,
        g_stats.remoteFrames,
        static_cast<std::uint32_t>(g_localHistory.size()),
        g_simFrame,
        WaitLoopDelayFrames());
    return true;
}

int SimIterationsAfterCapture(const int preLocalSize)
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    return preLocalSize - WaitLoopDelayFrames() + 1 - static_cast<int>(g_simFrame);
}

int WaitRemoteIterations()
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    const int delay = WaitLoopDelayFrames();
    g_stats.effectiveDelay = static_cast<std::uint32_t>(delay);

    const int localSize = static_cast<int>(g_localHistory.size());
    const int remoteSize = static_cast<int>(g_remoteHistory.size());

    const std::uint64_t flowCap =
        static_cast<std::uint64_t>(g_peerAckFrame)
        + static_cast<std::uint64_t>(proto::kMaxQueueSlot);
    if (static_cast<std::uint64_t>(g_localHistory.size()) >= flowCap)
    {
        ++g_stats.stallCount;
        static std::uint32_t s_flowStallLog = 0;
        if ((++s_flowStallLog % 60u) == 1u)
        {
            lifecycle::Stall("lockstep", "peer_ack_window_full");
            lifecycle::Netplay(
                "pause ACK window full local=%d peerAck=%u window=%d remote=%d sim=%u",
                localSize,
                g_peerAckFrame,
                proto::kMaxQueueSlot,
                remoteSize,
                g_simFrame);
        }
        return 0;
    }

    if (localSize < delay + 1)
    {
        log::Debug(
            "lockstep",
            "warmup local=%d remote=%d sim=%u waitLoopDelay=%d ping=%u",
            localSize,
            remoteSize,
            g_simFrame,
            delay,
            g_pingMs);
        return 1;
    }

    const int needRemote = localSize - delay - 1;
    if (needRemote < remoteSize)
    {
        const int postIterations = SimIterationsAfterCapture(localSize);
        log::Debug(
            "lockstep",
            "capture ok postIter=%d waitLoopDelay=%d local=%d remote=%d sim=%u needRemote=%d",
            postIterations,
            delay,
            localSize,
            remoteSize,
            g_simFrame,
            needRemote);
        return 1;
    }

    ++g_stats.stallCount;
    static std::uint32_t s_stallLog = 0;
    if ((++s_stallLog % 60u) == 1u)
    {
        lifecycle::Stall("lockstep", "wait_remote");
        lifecycle::Netplay(
            "pause too few remote inputs ping=%u delay=%d local=%d remote=%d sim=%u needRemote=%d",
            g_pingMs,
            delay,
            localSize,
            remoteSize,
            g_simFrame,
            needRemote);
    }
    log::Debug(
        "lockstep",
        "pause too few remote inputs ping=%u waitLoopDelay=%d local=%d remote=%d sim=%u needRemote=%d",
        g_pingMs,
        delay,
        localSize,
        remoteSize,
        g_simFrame,
        needRemote);
    return 0;
}

bool IsWaitRemoteStalled()
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    return WaitRemoteIterations() <= 0;
}

const char* HandoffBlockReasonName(const HandoffBlockReason reason)
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    switch (reason)
    {
    case HandoffBlockReason::None:
        return "none";
    case HandoffBlockReason::EmptyHistory:
        return "empty_history";
    case HandoffBlockReason::MissingFrameInputs:
        return "missing_frame_inputs";
    case HandoffBlockReason::WaitRemoteStalled:
        return "wait_remote_stalled";
    case HandoffBlockReason::AsymmetricHistory:
        return "asymmetric_history";
    default:
        return "?";
    }
}

HandoffStatus EvaluateHandoff()
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    HandoffStatus status{};
    status.simFrame = g_simFrame;
    status.localSize = g_localHistory.size();
    status.remoteSize = g_remoteHistory.size();
    status.effectiveDelay = WaitLoopDelayFrames();
    status.waitIterations = WaitRemoteIterations();
    status.waitStalled = status.waitIterations <= 0;
    status.hasFrameInputs = HasWireInputsForFrame(g_simFrame);

    if (status.localSize >= static_cast<std::size_t>(status.effectiveDelay + 1))
    {
        status.needRemote =
            static_cast<int>(status.localSize) - status.effectiveDelay - 1;
    }

    if (status.localSize == 0u)
    {
        status.blockReason = HandoffBlockReason::EmptyHistory;
        return status;
    }
    // At the handoff simFrame may equal history length: require coverage only.
    if (status.simFrame > status.localSize || status.simFrame > status.remoteSize)
    {
        status.blockReason = HandoffBlockReason::MissingFrameInputs;
        return status;
    }
    if (status.waitStalled)
    {
        status.blockReason = HandoffBlockReason::WaitRemoteStalled;
        return status;
    }
    if (status.localSize != status.remoteSize)
    {
        status.blockReason = HandoffBlockReason::AsymmetricHistory;
        return status;
    }
    if (status.simFrame != status.localSize)
    {
        status.blockReason = HandoffBlockReason::AsymmetricHistory;
        return status;
    }

    status.ready = true;
    status.blockReason = HandoffBlockReason::None;
    return status;
}

void LogHandoffTransition(const HandoffStatus& status)
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    struct Snapshot
    {
        HandoffBlockReason reason = HandoffBlockReason::None;
        std::uint32_t simFrame = 0;
        std::size_t localSize = 0;
        std::size_t remoteSize = 0;
        bool ready = false;
    };

    static Snapshot last{};

    const bool changed = status.ready != last.ready
        || status.blockReason != last.reason
        || status.simFrame != last.simFrame
        || status.localSize != last.localSize
        || status.remoteSize != last.remoteSize;

    if (!changed)
    {
        return;
    }

    last.ready = status.ready;
    last.reason = status.blockReason;
    last.simFrame = status.simFrame;
    last.localSize = status.localSize;
    last.remoteSize = status.remoteSize;

    if (status.ready)
    {
        lifecycle::Netplay(
            "handoff READY sim=%u local=%zu remote=%zu delay=%d",
            status.simFrame,
            status.localSize,
            status.remoteSize,
            status.effectiveDelay);
        log::Step(
            "lockstep",
            "handoff READY sim=%u local=%zu remote=%zu delay=%d (frame inputs available; remote-wait budget satisfied)",
            status.simFrame,
            status.localSize,
            status.remoteSize,
            status.effectiveDelay);
        return;
    }

    lifecycle::Netplay(
        "handoff BLOCKED reason=%s sim=%u local=%zu remote=%zu waitIter=%d needRemote=%d",
        HandoffBlockReasonName(status.blockReason),
        status.simFrame,
        status.localSize,
        status.remoteSize,
        status.waitIterations,
        status.needRemote);
    log::Step(
        "lockstep",
        "handoff BLOCKED reason=%s sim=%u local=%zu remote=%zu hasFrame=%d waitIter=%d needRemote=%d delay=%d",
        HandoffBlockReasonName(status.blockReason),
        status.simFrame,
        status.localSize,
        status.remoteSize,
        status.hasFrameInputs ? 1 : 0,
        status.waitIterations,
        status.needRemote,
        status.effectiveDelay);
}

bool IsHandoffReady()
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    return EvaluateHandoff().ready;
}

void TrimForBattleEntry(const std::uint32_t frame)
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    if (g_localHistory.size() <= frame && g_remoteHistory.size() <= frame && g_simFrame <= frame)
    {
        return;
    }

    // Numbering must match, or every desync compare is misaligned.
    lifecycle::Netplay(
        "battle entry trim local=%zu remote=%zu sim=%u -> %u (peer entry earlier)",
        g_localHistory.size(),
        g_remoteHistory.size(),
        g_simFrame,
        frame);
    log::Step(
        "lockstep",
        "battle entry trim local=%zu remote=%zu sim=%u -> %u (peer entry earlier)",
        g_localHistory.size(),
        g_remoteHistory.size(),
        g_simFrame,
        frame);
    TrimHistoriesTo(frame);
}

void AlignForBattleHandoff()
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    const std::uint32_t aligned = static_cast<std::uint32_t>(std::min(
        {g_localHistory.size(), g_remoteHistory.size(), static_cast<std::size_t>(g_simFrame)}));
    if (aligned < g_localHistory.size() || aligned < g_remoteHistory.size() || aligned < g_simFrame)
    {
        TrimHistoriesTo(aligned);
    }
    lifecycle::Sync(
        "lockstep",
        "battle handoff align=%u local=%zu remote=%zu sim=%u",
        aligned,
        g_localHistory.size(),
        g_remoteHistory.size(),
        g_simFrame);
    log::Step(
        "lockstep",
        "battle handoff align=%u local=%zu remote=%zu sim=%u",
        aligned,
        g_localHistory.size(),
        g_remoteHistory.size(),
        g_simFrame);
    LogHandoffTransition(EvaluateHandoff());
}

bool GetRemoteInput(proto::PlayerInputRecord* out)
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    if (out == nullptr)
    {
        return false;
    }

    const std::uint32_t frame = ActiveWireFrameIndex();
    if (frame >= g_remoteHistory.size())
    {
        return false;
    }

    *out = g_remoteHistory[static_cast<std::size_t>(frame)];
    const int lead =
        static_cast<int>(g_remoteHistory.size()) - static_cast<int>(g_localHistory.size());
    g_stats.lastRemoteLead = lead > 0 ? static_cast<std::uint32_t>(lead) : 0u;
    return true;
}

bool GetLocalInput(proto::PlayerInputRecord* out)
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    if (out == nullptr)
    {
        return false;
    }

    const std::uint32_t frame = ActiveWireFrameIndex();
    if (frame >= g_localHistory.size())
    {
        return false;
    }

    *out = g_localHistory[static_cast<std::size_t>(frame)];
    return true;
}

std::uint8_t GetMergedSystemState()
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    return menu::FilterOnlineSystemState(
        static_cast<std::uint8_t>(g_localSystem.load() | g_remoteSystem.load()));
}

void ExportWireHistories(std::vector<std::uint8_t>* localOut, std::vector<std::uint8_t>* remoteOut)
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    if (localOut == nullptr || remoteOut == nullptr)
    {
        return;
    }

    ServiceConfirmedSpectatorFramesLocked();

    localOut->clear();
    remoteOut->clear();
    localOut->reserve(g_localHistory.size());
    remoteOut->reserve(g_remoteHistory.size());

    for (const proto::PlayerInputRecord& record : g_localHistory)
    {
        localOut->push_back(record.playerAction);
    }
    for (const proto::PlayerInputRecord& record : g_remoteHistory)
    {
        remoteOut->push_back(record.playerAction);
    }

    log::Step(
        "lockstep",
        "export wire histories local=%zu remote=%zu sim=%u",
        localOut->size(),
        remoteOut->size(),
        g_simFrame);
}

void NotifySimAdvanced()
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    const auto* const screenPtr = static_cast<const std::uint8_t*>(
        trial::ResolveVa(trial::kVaBattleScreenMode));
    const std::uint8_t screen = screenPtr != nullptr ? *screenPtr : 0xFFu;
    if (g_consumedScreens.size() == g_simFrame
        && g_consumedGenerations.size() == g_simFrame)
    {
        g_consumedScreens.push_back(screen);
        g_consumedGenerations.push_back(netplay::GetSyncGeneration());
    }
    else if (g_simFrame < g_consumedScreens.size()
        && g_simFrame < g_consumedGenerations.size())
    {
        g_consumedScreens[g_simFrame] = screen;
        g_consumedGenerations[g_simFrame] = netplay::GetSyncGeneration();
    }
    else
    {
        log::Error(
            "spectator",
            "lockstep provenance gap sim=%u screens=%zu generations=%zu",
            g_simFrame,
            g_consumedScreens.size(),
            g_consumedGenerations.size());
        return;
    }
    ++g_simFrame;
    g_stats.simFrame = g_simFrame;
    // Do not publish here: drain the common frontier so no frame is skipped.
    ServiceConfirmedSpectatorFramesLocked();
}

void BeginSpectatorGame(const std::uint32_t sourceFrame)
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    g_nextSpectatorFrame = sourceFrame;
    ServiceConfirmedSpectatorFramesLocked();
}

bool IsWaitLoopWarmup()
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    return static_cast<int>(g_localHistory.size()) < WaitLoopWarmupTarget();
}

std::uint32_t GetSimFrame()
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    return g_simFrame;
}

Stats GetStats()
{
    std::lock_guard<std::recursive_mutex> lk(g_historyMutex);
    g_stats.simFrame = g_simFrame;
    return g_stats;
}

}
