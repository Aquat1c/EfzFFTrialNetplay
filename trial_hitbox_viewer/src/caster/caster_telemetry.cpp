#include "caster_telemetry.h"

#include "caster_input.h"
#include "control_swap.h"
#include "debug_log.h"
#include "determinism.h"
#include "framestep.h"
#include "lifecycle_log.h"
#include "rng.h"
#include "savestate.h"
#include "savestate_profiler.h"
#include "sdl_input.h"
#include "trial_input_codec.h"
#include "trial_addresses.h"

#include <atomic>
#include <cstdio>

namespace thv::telemetry
{
namespace
{

constexpr int kHeartbeatFrames = 60;

std::atomic<bool> g_initialized{false};
std::atomic<bool> g_verboseInput{false};
std::atomic<int> g_lastScreenMode{-1};
std::atomic<std::uint32_t> g_lastScreenSubState{0xFFFFFFFFu};

const char* CpuFlagLabel(int cpuFlag)
{
    return cpuFlag == 0 ? "human" : "cpu";
}

void LogPlayerSnapshot(const char* prefix, const PlayerInputSnapshot& player)
{
    log::Info(
        "input",
        "%s P%d char=%u cpu=%s(%d) face=%d mask=0x%02X raw[ce06..ce0b]=%02X %02X %02X %02X %02X %02X "
        "histIdx=%u packed=0x%04X",
        prefix,
        player.side + 1,
        static_cast<unsigned>(player.charId),
        CpuFlagLabel(player.cpuFlag),
        player.cpuFlag,
        static_cast<int>(player.facing),
        static_cast<unsigned>(player.wireMask),
        static_cast<unsigned>(player.horiz),
        static_cast<unsigned>(player.vert),
        static_cast<unsigned>(player.buttonA),
        static_cast<unsigned>(player.buttonB),
        static_cast<unsigned>(player.buttonC),
        static_cast<unsigned>(player.buttonD),
        static_cast<unsigned>(player.historyIndex),
        static_cast<unsigned>(player.lastPackedWord));
}

void LogRuntimeSnapshot(const RuntimeSnapshot& snap, const char* reason)
{
    log::Info("dump", "=== %s ===", reason);
    log::Info(
        "screen",
        "mode=%u (%s) subState=%u battleActive=%d diActive=%d",
        static_cast<unsigned>(snap.screenMode),
        ScreenModeName(snap.screenMode),
        snap.screenSubState,
        snap.battleActive ? 1 : 0,
        snap.diActive ? 1 : 0);
    log::Info(
        "session",
        "humanSide=P%d framestepPaused=%d localFrame=%llu saveSlot=%d saves=%u loads=%u",
        snap.humanSide + 1,
        snap.framestepPaused ? 1 : 0,
        static_cast<unsigned long long>(snap.localFrame),
        snap.saveSlot,
        snap.saveCount,
        snap.loadCount);
    log::Info(
        "det",
        "fpuPinned cw=0x%04X last=0x%04X mismatches=%u",
        static_cast<unsigned>(snap.fpuCapturedCw),
        static_cast<unsigned>(snap.fpuLastObservedCw),
        snap.fpuMismatchCount);
    log::Info(
        "rng",
        "shadow=0x%08X lastSeed=0x%08X srandCalls=%u randCalls=%u iatRand=%d iatSrand=%d staticRand=%d staticSrand=%d",
        snap.rngShadow,
        snap.rngLastSeed,
        snap.rngSrandCalls,
        snap.rngRandCalls,
        snap.rngIatRand ? 1 : 0,
        snap.rngIatSrand ? 1 : 0,
        snap.rngStaticRand ? 1 : 0,
        snap.rngStaticSrand ? 1 : 0);
    LogPlayerSnapshot("player", snap.p1);
    LogPlayerSnapshot("player", snap.p2);
    log::Info(
        "hooks",
        "battleTick=EFZ+0x%05X inputPoll=EFZ+0x%05X srandSite=EFZ+0x%05X",
        static_cast<unsigned>(trial::VaToRva(trial::kVaBattleTick)),
        static_cast<unsigned>(trial::VaToRva(trial::kVaPollPlayerInput)),
        0x5C76Bu);
    log::Info("dump", "=== end ===");
}

void MaybeLogScreenTransition()
{
    const auto* const modePtr = static_cast<const std::uint8_t*>(trial::ResolveVa(trial::kVaBattleScreenMode));
    const auto* const subPtr = static_cast<const std::uint32_t*>(trial::ResolveVa(trial::kVaScreenSubState));
    const auto* const animPtr = static_cast<const std::int32_t*>(trial::ResolveVa(trial::kVaScreenAnimCounter));
    if (modePtr == nullptr)
    {
        return;
    }

    const int mode = *modePtr;
    const std::uint32_t subState = subPtr != nullptr ? *subPtr : 0u;
    const int animCounter = animPtr != nullptr ? *animPtr : -1;
    const int lastMode = g_lastScreenMode.exchange(mode);
    const std::uint32_t lastSub = g_lastScreenSubState.exchange(subState);

    if (mode != lastMode || subState != lastSub)
    {
        if (mode == static_cast<int>(trial::kScreenLoading))
        {
            log::Info(
                "screen",
                "transition mode %d->%d (%s) CCDBC0 0x%08X->0x%08X CCDBBC=%d",
                lastMode,
                mode,
                ScreenModeName(static_cast<std::uint8_t>(mode)),
                lastSub,
                subState,
                animCounter);
        }
        else
        {
            log::Info(
                "screen",
                "transition mode %d->%d (%s) subState 0x%08X->0x%08X",
                lastMode,
                mode,
                ScreenModeName(static_cast<std::uint8_t>(mode)),
                lastSub,
                subState);
        }
    }
}

}

const char* ScreenModeName(std::uint8_t mode)
{
    switch (mode)
    {
    case 0:
        return "main_menu";
    case 1:
        return "battle";
    case 2:
        return "char_select";
    case 3:
        return "loading";
    case 4:
        return "title";
    default:
        return "unknown";
    }
}

std::uint8_t PackWireMaskFromPlayer(const void* playerBase)
{
    return codec::PackWireMaskFromPlayer(playerBase);
}

bool ReadPlayerInputSnapshot(int side, PlayerInputSnapshot* out)
{
    if (out == nullptr)
    {
        return false;
    }

    const std::uintptr_t va = (side == 0) ? trial::kVaP1Player : trial::kVaP2Player;
    const auto* const player = static_cast<const std::uint8_t*>(trial::ResolveVa(va));
    if (player == nullptr)
    {
        return false;
    }

    PlayerInputSnapshot snap = {};
    snap.side = side;
    snap.charId = player[0];
    snap.cpuFlag = *reinterpret_cast<const int*>(player + trial::kOffCpuControlFlag);
    snap.facing = static_cast<std::int8_t>(player[trial::kOffFacing]);
    snap.horiz = player[trial::kOffInputHoriz];
    snap.vert = player[trial::kOffInputVertical];
    snap.buttonA = player[trial::kOffInputButtonA];
    snap.buttonB = player[trial::kOffInputButtonB];
    snap.buttonC = player[trial::kOffInputButtonC];
    snap.buttonD = player[trial::kOffInputButtonD];
    snap.historyIndex = *reinterpret_cast<const std::uint16_t*>(player + trial::kOffInputHistoryIndex);

    const std::size_t historyOffset =
        trial::kOffInputHistoryBase + static_cast<std::size_t>(snap.historyIndex) * sizeof(std::uint16_t);
    if (historyOffset + sizeof(std::uint16_t) <= trial::kPlayerWordStride)
    {
        snap.lastPackedWord = *reinterpret_cast<const std::uint16_t*>(player + historyOffset);
    }

    snap.wireMask = PackWireMaskFromPlayer(player);
    *out = snap;
    return true;
}

RuntimeSnapshot CaptureRuntimeSnapshot()
{
    RuntimeSnapshot snap = {};

    const auto* const modePtr = static_cast<const std::uint8_t*>(trial::ResolveVa(trial::kVaBattleScreenMode));
    const auto* const subPtr = static_cast<const std::uint32_t*>(trial::ResolveVa(trial::kVaScreenSubState));
    const auto* const diPtr = static_cast<const std::uint32_t*>(trial::ResolveVa(trial::kVaDiActiveFlag));

    if (modePtr != nullptr)
    {
        snap.screenMode = *modePtr;
        snap.battleActive = *modePtr == 1u;
    }
    if (subPtr != nullptr)
    {
        snap.screenSubState = *subPtr;
    }
    if (diPtr != nullptr)
    {
        snap.diActive = *diPtr != 0u;
    }

    snap.humanSide = GetHumanSide();
    snap.framestepPaused = IsFramestepPaused();
    snap.localFrame = det::GetLocalFrame();

    const det::Status detStatus = det::GetStatus();
    snap.fpuCapturedCw = detStatus.capturedFpuCw;
    snap.fpuLastObservedCw = detStatus.lastObservedFpuCw;
    snap.fpuMismatchCount = detStatus.fpuMismatchCount;

    const det::rng::Status rngStatus = det::rng::GetStatus();
    snap.rngIatRand = rngStatus.iatRandHooked;
    snap.rngIatSrand = rngStatus.iatSrandHooked;
    snap.rngStaticRand = rngStatus.staticRandHooked;
    snap.rngStaticSrand = rngStatus.staticSrandHooked;
    snap.rngShadow = rngStatus.shadowState;
    snap.rngLastSeed = rngStatus.lastSeed;
    snap.rngSrandCalls = rngStatus.srandCalls;
    snap.rngRandCalls = rngStatus.randCalls;

    snap.saveSlot = GetActivePracticeSlot();
    snap.saveCount = GetSavestateCount();
    snap.loadCount = GetLoadstateCount();

    ReadPlayerInputSnapshot(0, &snap.p1);
    ReadPlayerInputSnapshot(1, &snap.p2);
    return snap;
}

bool Initialize()
{
    if (g_initialized.exchange(true))
    {
        return true;
    }

    if (!log::Initialize())
    {
        g_initialized = false;
        return false;
    }

    log::Info("init", "Telemetry online (Trial Hitbox Viewer caster debug)");
    DumpFullSnapshot("startup");
    return true;
}

void Shutdown()
{
    if (!g_initialized.exchange(false))
    {
        return;
    }

    lifecycle::Exit("telemetry", "shutdown", nullptr);
    log::Info("init", "Telemetry shutting down");
    log::Shutdown();
}

void DumpFullSnapshot(const char* reason)
{
    const char* label = reason != nullptr ? reason : "snapshot";
    lifecycle::Checkpoint(label);
    const RuntimeSnapshot snap = CaptureRuntimeSnapshot();
    LogRuntimeSnapshot(snap, label);
}

bool VerboseInputLogging()
{
    return g_verboseInput.load();
}

void SetVerboseInputLogging(bool enabled)
{
    g_verboseInput.store(enabled);
    log::Info("input", "verbose input logging %s", enabled ? "ON" : "OFF");
}

void OnInputPolled(void* player, unsigned int requestedIndex, unsigned int effectiveIndex)
{
    if (!g_verboseInput.load())
    {
        return;
    }

    const std::uintptr_t p1 = reinterpret_cast<std::uintptr_t>(trial::ResolveVa(trial::kVaP1Player));
    const int side = (reinterpret_cast<std::uintptr_t>(player) == p1) ? 0 : 1;

    PlayerInputSnapshot snap = {};
    if (!ReadPlayerInputSnapshot(side, &snap))
    {
        return;
    }

    log::Info(
        "input",
        "poll side=P%d reqIdx=%u effIdx=%u",
        side + 1,
        requestedIndex,
        effectiveIndex);
    LogPlayerSnapshot("poll", snap);
}

void OnBattleFrameBegin()
{
    MaybeLogScreenTransition();

    // Developer-only: never poll during a player or spectator session.
    if (caster::IsNetplayActive() || caster::IsSpectatorActive())
    {
        return;
    }

    const std::uint64_t frame = det::GetLocalFrame();
    if (frame == 0 || (frame % kHeartbeatFrames) != 0)
    {
        return;
    }

    const RuntimeSnapshot snap = CaptureRuntimeSnapshot();
  if (!snap.battleActive)
  {
    return;
  }

    log::Info(
        "frame",
        "heartbeat frame=%llu P1mask=0x%02X P2mask=0x%02X rng=0x%08X human=P%d paused=%d",
        static_cast<unsigned long long>(snap.localFrame),
        static_cast<unsigned>(snap.p1.wireMask),
        static_cast<unsigned>(snap.p2.wireMask),
        snap.rngShadow,
        snap.humanSide + 1,
        snap.framestepPaused ? 1 : 0);
}

void PollHotkeys()
{
    if (!g_initialized.load())
    {
        return;
    }

    MaybeLogScreenTransition();

    if (caster::IsNetplayActive() || caster::IsSpectatorActive())
    {
        return;
    }

    if (input::WasPressed(SDL_SCANCODE_L))
    {
        DumpFullSnapshot("manual dump (L)");
    }

    if (input::WasPressed(SDL_SCANCODE_O))
    {
        SetVerboseInputLogging(!g_verboseInput.load());
    }

    if (input::WasPressed(SDL_SCANCODE_I))
    {
        savestate_profiler::SetEnabled(!savestate_profiler::IsEnabled());
        log::Info(
            "telemetry",
            "savestate profiler %s",
            savestate_profiler::IsEnabled() ? "ON (play a match, press I again to dump)" : "OFF (dumped)");
    }
}

}
