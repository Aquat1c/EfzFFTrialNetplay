// CPU flag: player+0x4C (non-zero => AI path, zero => human poll).

#include "control_swap.h"
#include "caster_input.h"
#include "control_config.h"
#include "control_config.h"
#include "framestep.h"
#include "menu_nav.h"
#include "netplay_input.h"
#include "rollback.h"
#include "caster_telemetry.h"
#include "debug_log.h"
#include "lifecycle_log.h"
#include "mem_write_log.h"
#include "sdl_input.h"
#include "trial_input_codec.h"
#include "trial_addresses.h"

#include <MinHook.h>

#include <windows.h>

#include <atomic>

namespace thv
{
namespace
{

using PollPlayerInputFn = void(__fastcall*)(void* player, void* /*edx*/, unsigned int playerIndex, HWND hWnd);
PollPlayerInputFn g_originalPollPlayerInput = nullptr;

std::atomic<bool> g_initialized{false};
std::atomic<int> g_humanSide{0};

int* PlayerCpuFlag(int side)
{
    const std::uintptr_t va = (side == 0) ? trial::kVaP1Player : trial::kVaP2Player;
    void* const player = trial::ResolveVa(va);
    if (player == nullptr)
    {
        return nullptr;
    }
    return reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(player) + trial::kOffCpuControlFlag);
}

std::uintptr_t CpuFlagVa(const int side)
{
    return (side == 0) ? trial::kVaVsHumanFlagP1 : trial::kVaVsHumanFlagP2;
}

void WriteCpuFlag(int* const ptr, const int side, const int value, const char* context)
{
    if (ptr == nullptr)
    {
        return;
    }

    const int before = *ptr;
    if (before != value)
    {
        memlog::LogScalarWrite(
            context,
            CpuFlagVa(side),
            side == 0 ? "p1_cpu" : "p2_cpu",
            static_cast<std::uint64_t>(before),
            static_cast<std::uint64_t>(value),
            4);
    }
    *ptr = value;
}

void WritePlayerByte(
    std::uint8_t* const base,
    const std::uintptr_t playerVa,
    const std::size_t offset,
    const std::uint8_t value,
    const char* field,
    const char* context)
{
    if (base == nullptr)
    {
        return;
    }

    const std::uint8_t before = base[offset];
    if (before != value)
    {
        memlog::LogScalarWrite(context, playerVa + offset, field, before, value, 1);
    }
    base[offset] = value;
}

void WritePlayerInt(
    std::uint8_t* const base,
    const std::uintptr_t playerVa,
    const std::size_t offset,
    const int value,
    const char* field,
    const char* context)
{
    if (base == nullptr)
    {
        return;
    }

    auto* const fieldPtr = reinterpret_cast<int*>(base + offset);
    const int before = *fieldPtr;
    if (before != value)
    {
        memlog::LogScalarWrite(
            context,
            playerVa + offset,
            field,
            static_cast<std::uint64_t>(before),
            static_cast<std::uint64_t>(value),
            4);
    }
    *fieldPtr = value;
}

bool BothPlayersUseHumanInputPath()
{
    int* const p1Cpu = PlayerCpuFlag(0);
    int* const p2Cpu = PlayerCpuFlag(1);
    if (p1Cpu == nullptr || p2Cpu == nullptr)
    {
        return false;
    }

    return *p1Cpu == 0 && *p2Cpu == 0;
}

int DetectHumanSideFromCpuFlags()
{
    int* const p1Cpu = PlayerCpuFlag(0);
    int* const p2Cpu = PlayerCpuFlag(1);
    if (p1Cpu == nullptr || p2Cpu == nullptr)
    {
        return 0;
    }

    const bool p1Human = *p1Cpu == 0;
    const bool p2Human = *p2Cpu == 0;
    if (p1Human && !p2Human)
    {
        return 0;
    }
    if (p2Human && !p1Human)
    {
        return 1;
    }
    return g_humanSide.load();
}

void ApplyHumanSide(int humanSide)
{
    if (humanSide != 0 && humanSide != 1)
    {
        return;
    }

    g_humanSide.store(humanSide);

    // Practice: swap input routing only; do not flip +0x4C.
    if (BothPlayersUseHumanInputPath())
    {
        return;
    }

    int* const p1Cpu = PlayerCpuFlag(0);
    int* const p2Cpu = PlayerCpuFlag(1);
    if (p1Cpu == nullptr || p2Cpu == nullptr)
    {
        return;
    }

    WriteCpuFlag(p1Cpu, 0, humanSide == 0 ? 0 : 1, "control_swap");
    WriteCpuFlag(p2Cpu, 1, humanSide == 1 ? 0 : 1, "control_swap");
}

void ToggleHumanSide()
{
    const int nextSide = g_humanSide.load() == 0 ? 1 : 0;
    ApplyHumanSide(nextSide);
    caster::SetLocalSide(g_humanSide.load());
    rollback::SetLocalSide(g_humanSide.load());

    log::Info(
        "session",
        "human side -> P%d (%s)",
        g_humanSide.load() + 1,
        BothPlayersUseHumanInputPath() ? "practice input swap" : "cpu flag swap");
}

int PlayerSideFromPointer(void* player)
{
    const std::uintptr_t p1 = reinterpret_cast<std::uintptr_t>(trial::ResolveVa(trial::kVaP1Player));
    return reinterpret_cast<std::uintptr_t>(player) == p1 ? 0 : 1;
}

bool IsIntroPollTraceFrame(rollback::Status* const outStatus)
{
    if (!rollback::IsEnabled())
    {
        return false;
    }
    const rollback::Status status = rollback::GetStatus();
    if (status.currentFrame < status.matchStartFrame)
    {
        return false;
    }
    const std::uint32_t offset = status.currentFrame - status.matchStartFrame;
    const bool trace = offset == 0u || offset == 1u || offset == 2u || offset == 5u
        || offset == 10u || offset == 20u || offset == 40u || offset == 60u
        || offset == 90u;
    if (trace && outStatus != nullptr)
    {
        *outStatus = status;
    }
    return trace;
}

void TraceIntroPoll(
    void* const player,
    const unsigned int requestedIndex,
    const char* const route,
    const int mask)
{
    rollback::Status status{};
    if (!IsIntroPollTraceFrame(&status))
    {
        return;
    }
    const int side = PlayerSideFromPointer(player);
    const int* const cpu = PlayerCpuFlag(side);
    lifecycle::Input(
        "battle_intro_poll",
        "frame=%u offset=%u side=P%d req=%u route=%s memorial=%d cpu=%d authority=%d mask=%s%02X",
        status.currentFrame,
        status.currentFrame - status.matchStartFrame,
        side + 1,
        requestedIndex,
        route != nullptr ? route : "?",
        rollback::MapMemorialScreenIndexFromGame(),
        cpu != nullptr ? *cpu : -1,
        rollback::IsInputAuthorityActive() ? 1 : 0,
        mask >= 0 ? "" : "n/a:",
        static_cast<unsigned>(mask >= 0 ? mask : 0));
}

bool UsesStructInputInjection()
{
    if (!menu::IsSyncEligibleNow())
    {
        return false;
    }

    if (caster::IsNetplayActive() || rollback::IsInputAuthorityActive())
    {
        return true;
    }

    return caster::GetTransportMode() == caster::TransportMode::LocalLoopback
        || caster::GetTransportMode() == caster::TransportMode::Spectator;
}

void LogOfflineStructInput(const int side, const std::uint8_t mask)
{
    static std::uint8_t s_lastMask[2] = {0xFFu, 0xFFu};
    if (side < 0 || side > 1 || s_lastMask[side] == mask)
    {
        return;
    }
    s_lastMask[side] = mask;
    lifecycle::Input(
        "offline-route",
        "P%d source=%s route=player-struct screen=%s mask=0x%02X",
        side + 1,
        trialnet::input::SourceDisplayName(controls::SourceForPlayer(side)),
        menu::NavScreenName(menu::DetectNavScreen()),
        static_cast<unsigned>(mask));
}

void InjectWireMaskIntoPlayerState(void* player, const unsigned int playerIndex)
{
    const int side = PlayerSideFromPointer(player);
    std::uint8_t mask = 0;
    (void)caster::TryGetWireMaskForPlayerSide(side, &mask);
    codec::ApplyWireMaskToPlayer(player, mask);
    TraceIntroPoll(player, playerIndex, "struct_inject", mask);

    // Direction bits only: attack buttons are edge-detected, so a held one reads 0.
    constexpr std::uint8_t kDirMask = 0x0Fu;
    constexpr std::uint8_t kButtonMask = 0xF0u;
    const std::uint8_t readBack = codec::PackWireMaskFromPlayer(player);
    if ((readBack & kDirMask) != (mask & kDirMask))
    {
        static std::atomic<std::uint32_t> s_mismatches{0};
        const std::uint32_t n = s_mismatches.fetch_add(1u) + 1u;
        if (n <= 8u || (n % 512u) == 0u)
        {
            lifecycle::Fault(
                "input",
                "inject verify DIR MISMATCH side=%d applied=0x%02X readBack=0x%02X (count=%u)",
                side,
                mask,
                readBack,
                n);
            log::Error(
                "input",
                "inject verify DIR MISMATCH side=%d applied=0x%02X readBack=0x%02X (count=%u)",
                side,
                mask,
                readBack,
                n);
        }
    }

    // Latch dwords hold the state the native poll reads; visible fields are edge pulses.
    const auto* const playerBytes = static_cast<const std::uint8_t*>(player);
    std::uint8_t heldButtons = 0u;
    if (*reinterpret_cast<const std::uint32_t*>(playerBytes + trial::kOffInputButtonALatch) != 0u) heldButtons |= 1u << 4u;
    if (*reinterpret_cast<const std::uint32_t*>(playerBytes + trial::kOffInputButtonBLatch) != 0u) heldButtons |= 1u << 5u;
    if (*reinterpret_cast<const std::uint32_t*>(playerBytes + trial::kOffInputButtonCLatch) != 0u) heldButtons |= 1u << 6u;
    if (*reinterpret_cast<const std::uint32_t*>(playerBytes + trial::kOffInputButtonDLatch) != 0u) heldButtons |= 1u << 7u;
    if ((heldButtons & kButtonMask) != (mask & kButtonMask))
    {
        static std::atomic<std::uint32_t> s_buttonMismatches{0};
        const std::uint32_t n = s_buttonMismatches.fetch_add(1u) + 1u;
        if (n <= 8u || (n % 512u) == 0u)
        {
            lifecycle::Fault(
                "input",
                "inject verify BUTTON MISMATCH side=%d applied=0x%02X latches=0x%02X (count=%u)",
                side, mask, heldButtons, n);
            log::Error(
                "input",
                "inject verify BUTTON MISMATCH side=%d applied=0x%02X latches=0x%02X (count=%u)",
                side, mask, heldButtons, n);
        }
    }

    telemetry::OnInputPolled(player, playerIndex, static_cast<unsigned int>(side));
}

void __fastcall HookedPollPlayerInput(void* player, void* /*edx*/, unsigned int playerIndex, HWND hWnd)
{
    if (g_originalPollPlayerInput == nullptr)
    {
        return;
    }

    if (hWnd != nullptr)
    {
        SetGameWindow(hWnd);
    }

    // Loading must inject both sides: its poll appends to the command-history ring.
    if (UsesStructInputInjection())
    {
        caster::RefreshNetplayWireInject();
        InjectWireMaskIntoPlayerState(player, playerIndex);
        return;
    }

    TraceIntroPoll(player, playerIndex, "non_struct_fallback", -1);

    if (!menu::IsFightInputAllowedNow())
    {
        if (menu::IsSyncEligibleNow())
        {
            const int side = PlayerSideFromPointer(player);
            const bool localPlayer =
                side == g_humanSide.load() || BothPlayersUseHumanInputPath();
            if (localPlayer)
            {
                g_originalPollPlayerInput(player, nullptr, playerIndex, hWnd);
                telemetry::OnInputPolled(player, playerIndex, playerIndex);
                return;
            }
        }

        codec::ApplyWireMaskToPlayer(player, 0);
        telemetry::OnInputPolled(player, playerIndex, playerIndex);
        return;
    }

    const int side = PlayerSideFromPointer(player);

    const int humanSide = g_humanSide.load();

    // Offline Practice bypasses the native poll; pump SDL here on the game thread.
    if (controls::SourceForPlayer(side) == trialnet::input::Source::Controller)
    {
        input::PumpControllerEventsOnGameThread();
    }

    if (BothPlayersUseHumanInputPath())
    {
        const std::uint8_t mask = codec::SampleSdlWireMaskForPlayer(side);
        LogOfflineStructInput(side, mask);
        codec::ApplyWireMaskToPlayer(player, mask);
        telemetry::OnInputPolled(player, playerIndex, static_cast<unsigned>(side));
        return;
    }
    else if (caster::IsBridgeActive())
    {
        if (side == caster::GetLocalSide())
        {
            codec::ApplyWireMaskToPlayer(player, codec::SampleSdlWireMaskForPlayer(side));
            telemetry::OnInputPolled(player, playerIndex, playerIndex);
            return;
        }

        if (side == caster::GetRemoteSide() && caster::HasRemoteInput())
        {
            codec::ApplyWireMaskToPlayer(player, caster::GetRemoteWireMask());
            telemetry::OnInputPolled(player, playerIndex, playerIndex);
            return;
        }
    }
    else if (side == humanSide)
    {
        const std::uint8_t mask = codec::SampleSdlWireMaskForPlayer(side);
        LogOfflineStructInput(side, mask);
        codec::ApplyWireMaskToPlayer(player, mask);
        telemetry::OnInputPolled(player, playerIndex, playerIndex);
        return;
    }

    unsigned int effectiveIndex = playerIndex;
    if (humanSide == 1 && playerIndex <= 1u)
    {
        effectiveIndex = playerIndex ^ 1u;
    }

    g_originalPollPlayerInput(player, nullptr, effectiveIndex, hWnd);
    telemetry::OnInputPolled(player, playerIndex, effectiveIndex);
}

}

bool InitializeControlSwap()
{
    if (g_initialized.exchange(true))
    {
        return true;
    }

    const int detectedSide = DetectHumanSideFromCpuFlags();
    g_humanSide.store(detectedSide);
    if (!BothPlayersUseHumanInputPath())
    {
        ApplyHumanSide(detectedSide);
    }
    caster::SetLocalSide(g_humanSide.load());
    rollback::SetLocalSide(g_humanSide.load());

    void* const target = trial::ResolveVa(trial::kVaPollPlayerInput);
    if (target == nullptr)
    {
        g_initialized = false;
        return false;
    }

    if (MH_CreateHook(target, &HookedPollPlayerInput, reinterpret_cast<void**>(&g_originalPollPlayerInput)) != MH_OK)
    {
        g_initialized = false;
        return false;
    }

    if (MH_QueueEnableHook(target) != MH_OK)
    {
        g_initialized = false;
        return false;
    }

    log::Info(
        "hooks",
        "input poll hooked EFZ+0x%05X humanSide=P%d",
        static_cast<unsigned>(trial::VaToRva(trial::kVaPollPlayerInput)),
        g_humanSide.load() + 1);
    return true;
}

void ShutdownControlSwap()
{
    if (!g_initialized.exchange(false))
    {
        return;
    }

    void* const target = trial::ResolveVa(trial::kVaPollPlayerInput);
    if (target != nullptr)
    {
        MH_DisableHook(target);
        MH_RemoveHook(target);
    }
}

void PollControlSwapHotkeys()
{
    if (!g_initialized.load())
    {
        return;
    }

    if (caster::IsNetplayActive() || caster::IsSpectatorActive())
    {
        return;
    }

    const auto action = trialnet::shortcuts::Action::SwitchPlayer;
    if (!controls::ShortcutConflictsWithAnyFightBinding(action)
        && input::WasPressed(controls::ShortcutKey(action)))
    {
        ToggleHumanSide();
    }
}

int GetHumanSide()
{
    return g_humanSide.load();
}

void SetHumanSide(int side)
{
    ApplyHumanSide(side);
    caster::SetLocalSide(side);
    rollback::SetLocalSide(side);
}

bool BothPlayersUseHumanInput()
{
    return BothPlayersUseHumanInputPath();
}

std::atomic<int> g_configuredNetplaySide{-1};

void ResetNetplaySideConfig()
{
    g_configuredNetplaySide.store(-1);
    netplay_input::RestoreCleanupPair();
}

void MaintainNetplayCpuFlags()
{
    if (!caster::IsNetplayActive() && !caster::IsSpectatorActive())
    {
        return;
    }

    // Both stay human: the win screen simulates both characters via the same poll.
    const int memorial = rollback::MapMemorialScreenIndexFromGame();
    if (memorial != 1 && memorial != 2 && memorial != 3 && !rollback::IsPostMatchScreen())
    {
        return;
    }

    static int s_lastLoggedMemorial = -1;
    if (memorial != s_lastLoggedMemorial && memorial >= 1 && memorial <= 3)
    {
        lifecycle::InputPath("netplay struct-inject memorial=%d", memorial);
        s_lastLoggedMemorial = memorial;
    }

    int* const p1Cpu = PlayerCpuFlag(0);
    int* const p2Cpu = PlayerCpuFlag(1);
    if (p1Cpu != nullptr)
    {
        WriteCpuFlag(p1Cpu, 0, 0, "netplay_cpu_maintain");
    }
    if (p2Cpu != nullptr)
    {
        WriteCpuFlag(p2Cpu, 1, 0, "netplay_cpu_maintain");
    }
}

void ConfigureNetplaySides(const int localSide)
{
    if (localSide != 0 && localSide != 1)
    {
        return;
    }

    if (g_configuredNetplaySide.load() == localSide)
    {
        return;
    }

    g_configuredNetplaySide.store(localSide);
    lifecycle::InputPath("ConfigureNetplaySides local=P%d (host=P1 client=P2)", localSide + 1);
    log::Step(
        "session",
        "ConfigureNetplaySides local=P%d (host=P1 client=P2, local bindings=P1 layout)",
        localSide + 1);
    g_humanSide.store(localSide);
    caster::SetLocalSide(localSide);
    rollback::SetLocalSide(localSide);
    netplay_input::SyncCleanupPairForLocalSide(localSide);
    MaintainNetplayCpuFlags();

    auto* const p1Base = static_cast<std::uint8_t*>(trial::ResolveVa(trial::kVaP1Player));
    auto* const p2Base = static_cast<std::uint8_t*>(trial::ResolveVa(trial::kVaP2Player));
    if (p1Base != nullptr)
    {
        WritePlayerInt(p1Base, trial::kVaP1Player, trial::kOffTrainingModeFlag, 0, "p1_training", "netplay_sides");
        WritePlayerByte(p1Base, trial::kVaP1Player, trial::kOffPlayerTypeByte, 0, "p1_type_byte", "netplay_sides");
    }
    if (p2Base != nullptr)
    {
        WritePlayerInt(p2Base, trial::kVaP2Player, trial::kOffTrainingModeFlag, 0, "p2_training", "netplay_sides");
        WritePlayerByte(p2Base, trial::kVaP2Player, trial::kOffPlayerTypeByte, 0, "p2_type_byte", "netplay_sides");
    }
}

void RestorePracticeBothHuman(const int humanSide)
{
    if (humanSide != 0 && humanSide != 1)
    {
        return;
    }

    log::Step("session", "RestorePracticeBothHuman human=P%d", humanSide + 1);
    g_humanSide.store(humanSide);
    caster::SetLocalSide(humanSide);
    rollback::SetLocalSide(humanSide);

    int* const p1Cpu = PlayerCpuFlag(0);
    int* const p2Cpu = PlayerCpuFlag(1);
    if (p1Cpu != nullptr)
    {
        WriteCpuFlag(p1Cpu, 0, 0, "practice_both_human");
    }
    if (p2Cpu != nullptr)
    {
        WriteCpuFlag(p2Cpu, 1, 0, "practice_both_human");
    }
}

}
