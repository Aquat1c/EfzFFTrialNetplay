#include "lifecycle_log.h"

#include "caster_input.h"
#include "debug_log.h"
#include "mem_write_log.h"
#include "menu_nav.h"
#include "netplay_bootstrap.h"
#include "netplay_lockstep.h"
#include "netplay_navigation.h"
#include "netplay_sync.h"
#include "determinism.h"
#include "rollback.h"
#include "trial_addresses.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace thv::lifecycle
{
namespace
{

void LogVa(const char* eventClass, const char* subsystem, const char* fmt, va_list args)
{
    log::VEvent(eventClass, subsystem, fmt, args);
}

std::uint8_t ReadTrialScreenMode()
{
    const auto* const mode = static_cast<const std::uint8_t*>(trial::ResolveVa(trial::kVaBattleScreenMode));
    return mode != nullptr ? *mode : 0xFFu;
}

std::int32_t ReadTrialInt32(const std::uintptr_t va)
{
    const auto* const value = static_cast<const std::int32_t*>(trial::ResolveVa(va));
    return value != nullptr ? *value : -1;
}

}

void Step(const char* eventClass, const char* subsystem, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log::VEvent(eventClass, subsystem, fmt, args);
    va_end(args);
}

bool PhaseWarrantsCheckpoint(const char* to, const char* detail)
{
    auto mentions = [](const char* haystack, const char* needle) -> bool {
        return haystack != nullptr && needle != nullptr && std::strstr(haystack, needle) != nullptr;
    };

    return mentions(to, "battle")
        || mentions(to, "Battle")
        || mentions(to, "CharSelect")
        || mentions(to, "Loading")
        || mentions(to, "shutdown")
        || mentions(to, "battle_session")
        || mentions(detail, "handoff");
}

void Phase(const char* subsystem, const char* from, const char* to, const char* detail)
{
    if (detail != nullptr && detail[0] != '\0')
    {
        log::Event(log::EventClass::Phase, subsystem, "%s -> %s (%s)", from, to, detail);
    }
    else
    {
        log::Event(log::EventClass::Phase, subsystem, "%s -> %s", from, to);
    }

    if (PhaseWarrantsCheckpoint(to, detail))
    {
        char reason[192] = {};
        if (detail != nullptr && detail[0] != '\0')
        {
            std::snprintf(reason, sizeof(reason), "%s: %s -> %s (%s)", subsystem, from, to, detail);
        }
        else
        {
            std::snprintf(reason, sizeof(reason), "%s: %s -> %s", subsystem, from, to);
        }
        Checkpoint(reason);
    }
}

void Exit(const char* subsystem, const char* reason, const char* detail)
{
    if (detail != nullptr && detail[0] != '\0')
    {
        log::Error(log::EventClass::Exit, "%s: %s — %s", subsystem, reason, detail);
    }
    else
    {
        log::Error(log::EventClass::Exit, "%s: %s", subsystem, reason);
    }
    WriteModStateSnapshot(nullptr);
    log::FlushFile();
}

void Rollback(const char* event, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogVa(log::EventClass::Rollback, event != nullptr ? event : "evt", fmt, args);
    va_end(args);
}

void Sync(const char* subsystem, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogVa(log::EventClass::Sync, subsystem, fmt, args);
    va_end(args);
}

void Input(const char* subsystem, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogVa(log::EventClass::Input, subsystem, fmt, args);
    va_end(args);
}

void Desync(const char* subsystem, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogVa(log::EventClass::Desync, subsystem, fmt, args);
    va_end(args);
}

void InputPath(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogVa(log::EventClass::Input, "path", fmt, args);
    va_end(args);
}

void Netplay(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogVa(log::EventClass::Net, "netplay", fmt, args);
    va_end(args);
}

void Stall(const char* gate, const char* reason)
{
    static std::uint32_t s_lastStallCount = UINT32_MAX;
    const std::uint32_t stallCount = netplay::lockstep::GetStats().stallCount;
    if (stallCount == s_lastStallCount)
    {
        return;
    }

    s_lastStallCount = stallCount;
    const int memorial = rollback::MapMemorialScreenIndexFromGame();
    log::Event(
        log::EventClass::Stall,
        gate,
        "%s memorial=%d rollback=%d sim=%u local=%u remote=%u stalls=%u",
        reason,
        memorial,
        rollback::IsEnabled() ? 1 : 0,
        netplay::lockstep::GetSimFrame(),
        netplay::lockstep::GetLocalHistorySize(),
        netplay::lockstep::GetRemoteHistorySize(),
        stallCount);
}

void NavScreenChange(const menu::NavScreen prev, const menu::NavScreen next)
{
    if (prev == next)
    {
        return;
    }

    const int memorial = rollback::MapMemorialScreenIndexFromGame();
    log::Event(
        log::EventClass::Phase,
        "nav",
        "screen %s -> %s memorial=%d trial_mode=%u",
        menu::NavScreenName(prev),
        menu::NavScreenName(next),
        memorial,
        static_cast<unsigned>(ReadTrialScreenMode()));
}

void Init(const char* subsystem, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogVa(log::EventClass::Init, subsystem, fmt, args);
    va_end(args);
}

void Checkpoint(const char* reason)
{
    static DWORD s_lastMs = 0;
    const DWORD now = GetTickCount();
    if (s_lastMs != 0 && (now - s_lastMs) < 250)
    {
        return;
    }

    s_lastMs = now;
    log::Event(log::EventClass::Phase, "checkpoint", "%s", reason != nullptr ? reason : "?");
    memlog::LogGameHeadline(reason != nullptr ? reason : "checkpoint");
    WriteModStateSnapshot(nullptr);
}

void Fault(const char* subsystem, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char message[1024] = {};
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    log::Error(subsystem != nullptr ? subsystem : "fault", "%s", message);
    log::Event(log::EventClass::Fault, subsystem != nullptr ? subsystem : "fault", "%s", message);
}

void Heartbeat()
{
    if (!caster::IsNetplayActive())
    {
        return;
    }

    static DWORD s_lastMs = 0;
    const DWORD now = GetTickCount();
    if (s_lastMs != 0 && (now - s_lastMs) < 30000)
    {
        return;
    }

    s_lastMs = now;
    memlog::LogGameHeadline("heartbeat");
    const int memorial = rollback::MapMemorialScreenIndexFromGame();
    log::Event(
        log::EventClass::Net,
        "heartbeat",
        "nav=%s memorial=%d rollback=%d sim=%u local=%u remote=%u sync_rx=%u aborted=%d",
        netplay::nav::PhaseName(netplay::nav::GetPhase()),
        memorial,
        rollback::IsEnabled() ? 1 : 0,
        netplay::lockstep::GetSimFrame(),
        netplay::lockstep::GetLocalHistorySize(),
        netplay::lockstep::GetRemoteHistorySize(),
        netplay::GetRemoteSyncFrontier(),
        netplay::IsNetplayAborted() ? 1 : 0);
}

void DiOverlay(
    const char* path,
    const std::uint8_t localMask,
    const std::uint8_t remoteMask,
    const bool hasRemote,
    const int memorialOrScreen)
{
    static std::uint8_t s_lastLocal = 0xFFu;
    static std::uint8_t s_lastRemote = 0xFFu;
    static std::uint32_t s_frame = 0;

    ++s_frame;
    const bool changed = localMask != s_lastLocal || remoteMask != s_lastRemote;
    if (!changed && (s_frame % 300u) != 0u)
    {
        return;
    }

    s_lastLocal = localMask;
    s_lastRemote = remoteMask;
    log::Event(
        log::EventClass::Input,
        "di",
        "%s screen=%d local=0x%02X remote=0x%02X hasRemote=%d",
        path,
        memorialOrScreen,
        static_cast<unsigned>(localMask),
        static_cast<unsigned>(remoteMask),
        hasRemote ? 1 : 0);
}

void WriteModStateSnapshot(FILE* file)
{
    const bool toSession = file == nullptr;
    FILE* target = file;
    if (toSession)
    {
        const std::filesystem::path sessionDirectory = log::SessionDirectoryPath();
        if (sessionDirectory.empty())
        {
            return;
        }
        const std::filesystem::path path = sessionDirectory / L"state_snapshot.log";
        if (_wfopen_s(&target, path.c_str(), L"a") != 0 || target == nullptr)
        {
            return;
        }
    }

    const rollback::Status rb = rollback::GetStatus();
    const netplay::lockstep::Stats ls = netplay::lockstep::GetStats();
    const netplay::lockstep::HandoffStatus handoff = netplay::lockstep::EvaluateHandoff();
    const netplay::nav::FlowStats flow = netplay::nav::GetStats();
    const int memorial = rollback::MapMemorialScreenIndexFromGame();
    const std::uint8_t trialMode = ReadTrialScreenMode();
    const menu::NavScreen navScreen = menu::DetectNavScreen();
    const det::Status detStatus = det::GetStatus();

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    std::fprintf(target, "\n--- mod_state_snapshot %04u-%02u-%02u %02u:%02u:%02u ---\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::fprintf(target, "session_log=%s\n", log::SessionLogPath().c_str());
    std::fprintf(target, "memwrite_log=%s\\memwrite.log\n", log::SessionDirectory().c_str());
    std::fprintf(target, "trial_screen_mode=%u (%s)\n", static_cast<unsigned>(trialMode), netplay::nav::TrialScreenName(trialMode));
    std::fprintf(target, "nav_screen=%s memorial=%d\n", menu::NavScreenName(navScreen), memorial);
    std::fprintf(target, "nav_phase=%s packet_stage=%s\n", netplay::nav::PhaseName(netplay::nav::GetPhase()), netplay::nav::PacketStageName(netplay::nav::GetPacketStage()));
    std::fprintf(target, "netplay_aborted=%d reason=%s\n", netplay::IsNetplayAborted() ? 1 : 0, netplay::GetNetplayAbortReason());
    std::fprintf(target, "rollback_enabled=%d armed=%d active=%d substate=%d transport=%d\n", rb.enabled ? 1 : 0, rollback::IsArmedForNetplay() ? 1 : 0, rb.active ? 1 : 0, static_cast<int>(rb.subState), static_cast<int>(rb.phase));
    std::fprintf(target, "rollback_frame=%u sim=%u confirmed=%u rollbacks=%u last_reload=%u\n", rb.currentFrame, rb.simFrame, rb.confirmedFrame, rb.rollbacksTotal, rb.lastReloadFrame);
    std::fprintf(target, "rollback_inputs local=%zu remote=%zu predicted=%zu ring=%zu\n", rb.localInputCount, rb.remoteInputCount, rb.predictedInputCount, rb.occupiedRingSlots);
    std::fprintf(target, "caster_transport=%d local_side=P%d bridge=%d input_auth=%d\n", static_cast<int>(caster::GetTransportMode()), caster::GetLocalSide() + 1, caster::IsBridgeActive() ? 1 : 0, rollback::IsInputAuthorityActive() ? 1 : 0);
    std::fprintf(
        target,
        "wire_masks local=0x%02X remote=0x%02X has_remote=%d inject_frame=%u sim=%u\n",
        static_cast<unsigned>(caster::GetLocalWireMask()),
        static_cast<unsigned>(caster::GetRemoteWireMask()),
        caster::HasRemoteInput() ? 1 : 0,
        netplay::lockstep::ActiveWireFrameIndex(),
        ls.simFrame);
    std::fprintf(target, "lockstep sim=%u local_hist=%u remote_hist=%u wait_loop_delay=%u rollback_input_delay=%d stalls=%u packets=%u\n", ls.simFrame, ls.localFrames, ls.remoteFrames, ls.effectiveDelay, rb.inputDelay, ls.stallCount, ls.packetsApplied);
    std::fprintf(target, "handoff ready=%d block=%s need_remote=%d wait_iter=%d stalled=%d\n", handoff.ready ? 1 : 0, netplay::lockstep::HandoffBlockReasonName(handoff.blockReason), handoff.needRemote, handoff.waitIterations, handoff.waitStalled ? 1 : 0);
    std::fprintf(target, "packets seed_tx=%u seed_rx=%u launch_tx=%u launch_rx=%u sync_tx=%u sync_rx=%u sync_applied=%u screen_x=%u\n", flow.seedPacketsSent, flow.seedPacketsRecv, flow.launchGoSent, flow.launchGoRecv, flow.syncPacketsSent, flow.syncPacketsRecv, flow.syncFramesApplied, flow.screenTransitions);
    std::fprintf(target, "rng_seed_synced=%u\n", netplay::GetSyncedRngSeed());
    std::fprintf(target, "sync_transport=%d seed_synced=%d launch_go=%d rollback_req=%d rollback_neg=%d remote_sync_frame=%u\n", netplay::IsSyncTransportActive() ? 1 : 0, netplay::IsDeterminismSynced() ? 1 : 0, netplay::LaunchGoReceived() ? 1 : 0, netplay::IsRollbackRequired() ? 1 : 0, netplay::IsRollbackNegotiated() ? 1 : 0, netplay::GetRemoteSyncFrontier());
    std::fprintf(target, "determinism fpu_pin=%d cw=0x%04X observed=0x%04X mismatches=%u local_frame=%llu\n", detStatus.fpuPinningEnabled ? 1 : 0, static_cast<unsigned>(detStatus.capturedFpuCw), static_cast<unsigned>(detStatus.lastObservedFpuCw), detStatus.fpuMismatchCount, static_cast<unsigned long long>(detStatus.localFrame));
    std::fprintf(target, "rollback_last_main_tick_iter=%d\n", rollback::GetLastMainTickIterations());
    std::fprintf(target, "bootstrap_phase=%s session_active=%d\n", netplay::GetBootstrapPhaseName(), netplay::IsSessionActive() ? 1 : 0);
    std::fprintf(
        target,
        "trial_substate=%d anim=%d charsel_pick_p1=%d charsel_pick_p2=%d\n",
        ReadTrialInt32(trial::kVaScreenSubState),
        ReadTrialInt32(trial::kVaScreenAnimCounter),
        ReadTrialInt32(trial::kVaCharSelectP1Pick),
        ReadTrialInt32(trial::kVaCharSelectP2Pick));
    std::fprintf(
        target,
        "player_4c_p1=%d player_4c_p2=%d pace_busy=%d hold_flag=%d substeps=%d\n",
        ReadTrialInt32(trial::kVaVsHumanFlagP1),
        ReadTrialInt32(trial::kVaVsHumanFlagP2),
        ReadTrialInt32(trial::kVaFramePacingBusyFlag),
        ReadTrialInt32(trial::kVaSubstepHoldFlag),
        ReadTrialInt32(trial::kVaFrameSubsteps));
    std::fprintf(target, "--- end mod_state_snapshot ---\n");
    std::fflush(target);

    if (toSession)
    {
        std::fclose(target);
    }
}

}
