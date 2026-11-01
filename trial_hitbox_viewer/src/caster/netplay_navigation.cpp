#include "netplay_navigation.h"

#include "debug_log.h"
#include "lifecycle_log.h"
#include "netplay_lockstep.h"
#include "netplay_sync.h"
#include "rng.h"
#include "rollback.h"
#include "spectator_relay.h"
#include "trial_addresses.h"

#include <atomic>

namespace thv::netplay::nav
{
namespace
{

std::atomic<Phase> g_phase{Phase::Idle};
std::atomic<PacketStage> g_packetStage{PacketStage::HandshakeDone};
FlowStats g_stats{};

}

const char* PhaseName(const Phase phase)
{
    switch (phase)
    {
    case Phase::Idle:
        return "Idle";
    case Phase::AwaitDeterminism:
        return "AwaitDeterminism";
    case Phase::AwaitCasterLaunchGo:
        return "AwaitCasterLaunchGo";
    case Phase::AwaitTitle:
        return "AwaitTitle";
    case Phase::EnterVsHuman:
        return "EnterVsHuman";
    case Phase::CharSelectLockstep:
        return "CharSelectLockstep";
    case Phase::LoadingSync:
        return "LoadingSync";
    case Phase::BattleLockstep:
        return "BattleLockstep";
    case Phase::PostMatch:
        return "PostMatch";
    case Phase::Active:
        return "Active";
    default:
        return "?";
    }
}

const char* PacketStageName(const PacketStage stage)
{
    switch (stage)
    {
    case PacketStage::HandshakeDone:
        return "HandshakeDone";
    case PacketStage::SeedExchange:
        return "SeedExchange(4)";
    case PacketStage::LaunchGo:
        return "LaunchGo(7)";
    case PacketStage::NavTitle:
        return "NavTitle";
    case PacketStage::NavCharSelect:
        return "NavCharSelect+SyncData(5)";
    case PacketStage::NavLoading:
        return "NavLoading";
    case PacketStage::BattleSyncData:
        return "BattleSyncData(5)";
    case PacketStage::Quit:
        return "Quit(6)";
    default:
        return "?";
    }
}

const char* TrialScreenName(const std::uint8_t mode)
{
    switch (mode)
    {
    case 0u:
        return "main_menu";
    case 1u:
        return "battle";
    case 2u:
        return "char_select";
    case 3u:
        return "loading";
    case 4u:
        return "title";
    default:
        return "unknown";
    }
}

void Reset()
{
    g_phase.store(Phase::Idle);
    g_packetStage.store(PacketStage::HandshakeDone);
    g_stats = {};
    log::Step("nav", "navigation reset");
}

void SetPhase(const Phase phase)
{
    const Phase prev = g_phase.exchange(phase);
    if (prev != phase)
    {
        lifecycle::Phase("nav", PhaseName(prev), PhaseName(phase));
    }
}

Phase GetPhase()
{
    return g_phase.load();
}

void SetPacketStage(const PacketStage stage)
{
    const PacketStage prev = g_packetStage.exchange(stage);
    if (prev != stage)
    {
        lifecycle::Netplay("packet stage %s -> %s", PacketStageName(prev), PacketStageName(stage));
    }
}

PacketStage GetPacketStage()
{
    return g_packetStage.load();
}

void OnTrialScreenMode(const std::uint8_t prev, const std::uint8_t next)
{
    if (prev == next)
    {
        return;
    }

    ++g_stats.screenTransitions;
    lifecycle::Netplay(
        "screen %u(%s) -> %u(%s)",
        static_cast<unsigned>(prev),
        TrialScreenName(prev),
        static_cast<unsigned>(next),
        TrialScreenName(next));

    lockstep::OnScreenTransition(prev, next);

    if (!rollback::IsEnabled()
        && prev != trial::kScreenCharSelect && next == trial::kScreenCharSelect)
    {
        spectator::GameStartInfo start{};
        start.sourceGeneration = GetSyncGeneration();
        start.sourceFrame = lockstep::GetSimFrame();
        start.rngSeed = GetSyncedRngSeed();
        start.frontendRngState = det::rng::Snapshot();
        start.frontendVisualRngState = det::rng::VisualSnapshot();
        start.screen = trial::kScreenCharSelect;
        if (!spectator::CaptureGameStartVisualMetadata(&start))
        {
            log::Error(
                "spectator",
                "cannot capture character-select visual metadata source=%u:%u",
                start.sourceGeneration,
                start.sourceFrame);
        }
        else if (spectator::BeginGame(start))
        {
            lockstep::BeginSpectatorGame(start.sourceFrame);
        }
    }

    switch (next)
    {
    case 4u:
        SetPacketStage(PacketStage::NavTitle);
        break;
    case 2u:
        SetPacketStage(PacketStage::NavCharSelect);
        if (GetPhase() == Phase::Active || GetPhase() == Phase::EnterVsHuman)
        {
            SetPhase(Phase::CharSelectLockstep);
        }
        break;
    case 3u:
        SetPacketStage(PacketStage::NavLoading);
        SetPhase(prev == 1u ? Phase::PostMatch : Phase::LoadingSync);
        break;
    case 1u:
        SetPacketStage(PacketStage::BattleSyncData);
        SetPhase(Phase::BattleLockstep);
        lifecycle::Phase("nav", TrialScreenName(prev), "battle", "lockstep handoff");
        if (rollback::IsArmedForNetplay())
        {
            lifecycle::Netplay("StartNetplaySession @ battle screen");
            rollback::StartNetplaySession();
        }
        // Not here: racing MainTick could skip the round 2+ entry barrier.
        break;
    default:
        break;
    }
}

FlowStats GetStats()
{
    return g_stats;
}

void NoteSeedSent()
{
    ++g_stats.seedPacketsSent;
}

void NoteSeedRecv()
{
    ++g_stats.seedPacketsRecv;
}

void NoteLaunchGoSent()
{
    ++g_stats.launchGoSent;
}

void NoteLaunchGoRecv()
{
    ++g_stats.launchGoRecv;
}

void NoteSyncSent()
{
    ++g_stats.syncPacketsSent;
}

void NoteSyncRecv()
{
    ++g_stats.syncPacketsRecv;
}

void NoteSyncApplied()
{
    ++g_stats.syncFramesApplied;
}

}
