#include "caster_input.h"

#include "control_swap.h"
#include "game_pacing.h"
#include "menu_nav.h"
#include "netplay_lockstep.h"
#include "netplay_frame_gate.h"
#include "netplay_sync.h"
#include "rollback.h"
#include "debug_log.h"
#include "lifecycle_log.h"
#include "sdl_input.h"
#include "test_harness.h"
#include "trial_input_codec.h"
#include "netplay_input.h"
#include "trial_addresses.h"

#include <atomic>
#include <climits>
#include <cstring>

namespace thv::caster
{
namespace
{

std::atomic<bool> g_initialized{false};
std::atomic<bool> g_bridgeActive{true};
std::atomic<TransportMode> g_mode{TransportMode::Offline};
std::atomic<int> g_localSide{0};

std::atomic<std::uint8_t> g_localWireMask{0};
std::atomic<std::uint8_t> g_remoteWireMask{0};
std::atomic<bool> g_hasRemoteInput{false};
std::atomic<std::uint16_t> g_spectatorP1Word{0u};
std::atomic<std::uint16_t> g_spectatorP2Word{0u};

proto::PlayerInputRecord g_localRecord{};
proto::PlayerInputRecord g_remoteRecord{};
proto::InputQueue g_localQueue{};

menu::NavScreen g_lastNavScreen = menu::NavScreen::None;
std::uint32_t g_lastDiInputToken = UINT32_MAX;

void PushLocalQueue(const proto::PlayerInputRecord& record)
{
    g_localQueue.slots[g_localQueue.writeIndex % proto::kMaxQueueSlot] = record;
    ++g_localQueue.writeIndex;
}

void MaybeLogNavTransition(const menu::NavScreen screen)
{
    if (screen == g_lastNavScreen)
    {
        return;
    }

    lifecycle::NavScreenChange(g_lastNavScreen, screen);
    g_lastNavScreen = screen;
}

void PumpNetplayScreenFrame();

void UpdateLockstepInjectRecords()
{
    proto::PlayerInputRecord localDelayed{};
    proto::PlayerInputRecord remote{};
    std::uint8_t mergedSystem = 0;

    if (netplay::lockstep::GetLocalInput(&localDelayed))
    {
        g_localWireMask.store(localDelayed.playerAction);
        g_localRecord.playerAction = localDelayed.playerAction;
        g_localRecord.systemState = localDelayed.systemState;
        mergedSystem |= localDelayed.systemState;
    }
    else
    {
        g_localWireMask.store(0);
    }

    if (netplay::lockstep::GetRemoteInput(&remote))
    {
        PushRemoteRecord(remote);
        mergedSystem |= remote.systemState;
    }
    else
    {
        g_hasRemoteInput.store(false);
        g_remoteWireMask.store(0);
    }

    input::SetMergedSystemState(mergedSystem);
}

void PumpNetplayScreenFrame()
{
    OnNetplayPollFrame();
}

}

bool Initialize()
{
    if (g_initialized.exchange(true))
    {
        return true;
    }

    g_localSide.store(GetHumanSide());
    g_localRecord = {};
    g_remoteRecord = {};
    g_localQueue = {};
    g_localWireMask.store(0);
    g_remoteWireMask.store(0);
    g_hasRemoteInput.store(false);
    g_spectatorP1Word.store(0u);
    g_spectatorP2Word.store(0u);
    g_bridgeActive.store(true);
    g_mode.store(TransportMode::Offline);
    g_lastNavScreen = menu::NavScreen::None;
    g_lastDiInputToken = UINT32_MAX;

    log::Info("caster", "input bridge online localSide=P%d", g_localSide.load() + 1);
    return true;
}

void Shutdown()
{
    if (!g_initialized.exchange(false))
    {
        return;
    }

    g_bridgeActive.store(false);
    g_hasRemoteInput.store(false);
}

void SampleAndPushSyncInput()
{
    const menu::NavScreen navScreen = menu::DetectNavScreen();
    if (!menu::IsSyncEligible(navScreen))
    {
        return;
    }

    if (g_mode.load() == TransportMode::Netplay && !rollback::IsEnabled())
    {
        UpdateLockstepInjectRecords();

        const bool warmup = netplay::lockstep::IsWaitLoopWarmup();
        // Always call WaitRemoteIterations: its ACK guard also applies during warmup.
        if (netplay::lockstep::WaitRemoteIterations() <= 0)
        {
            netplay::SendLocalSyncFrame();
            return;
        }

        std::uint8_t localMask = SampleSyncWireMaskForCapture();
        if (harness::ScriptedInputEnabled() && navScreen == menu::NavScreen::CharSelect)
        {
            localMask = harness::ScriptedCharSelectMask(netplay::lockstep::GetLocalHistorySize());
        }
        proto::PlayerInputRecord fresh{};
        fresh.playerAction = localMask;
        fresh.systemState = menu::SampleLocalSystemState();
        PushLocalQueue(fresh);

        const int preLocal = static_cast<int>(netplay::lockstep::GetLocalHistorySize());
        netplay::lockstep::OnLocalInput(fresh);
        netplay::SendLocalSyncFrame();
        if (!warmup && netplay::lockstep::SimIterationsAfterCapture(preLocal) > 0)
        {
            netplay::lockstep::NotifySimAdvanced();
        }
        return;
    }

    const std::uint8_t localMask = SampleSyncWireMaskForCapture();
    g_localWireMask.store(localMask);
    g_localRecord.playerAction = localMask;
    g_localRecord.systemState = menu::SampleLocalSystemState();
    PushLocalQueue(g_localRecord);

    if (g_mode.load() == TransportMode::LocalLoopback)
    {
        g_remoteWireMask.store(localMask);
        g_remoteRecord = g_localRecord;
        g_hasRemoteInput.store(true);
        return;
    }

    if (g_mode.load() != TransportMode::Netplay)
    {
        return;
    }

    if (rollback::IsEnabled())
    {
        return;
    }
}

void OnNetplayPollFrame()
{
    if (!g_initialized.load() || g_mode.load() != TransportMode::Netplay || !menu::IsSyncEligibleNow())
    {
        return;
    }

    if (netplay::ShouldStallLockstepFrame())
    {
        netplay::Poll();
        RefreshLockstepWireInject();
        pacing::ConsumeActiveScreenFrameSlot();
        return;
    }

    SampleAndPushSyncInput();
    netplay::Poll();
}

void OnInputTick()
{
    if (!g_initialized.load())
    {
        return;
    }

    if (g_mode.load() == TransportMode::Netplay)
    {
        const menu::NavScreen navScreen = menu::DetectNavScreen();
        MaybeLogNavTransition(navScreen);
        return;
    }

    g_localSide.store(GetHumanSide());

    const menu::NavScreen navScreen = menu::DetectNavScreen();
    MaybeLogNavTransition(navScreen);

    g_localWireMask.store(codec::SampleSdlWireMask());

    if (g_mode.load() == TransportMode::LocalLoopback && menu::IsFightInputAllowed(navScreen))
    {
        SampleAndPushSyncInput();
    }
}

void OnBattleFrameInput()
{
    if (!g_initialized.load())
    {
        return;
    }

    g_lastDiInputToken = UINT32_MAX;
    SampleAndPushSyncInput();
}

void OnScreenInputFrame()
{
    OnNetplayPollFrame();
}

void OnDiInputFrame()
{
    OnScreenInputFrame();
}

bool IsBridgeActive()
{
    if (rollback::IsInputAuthorityActive())
    {
        return false;
    }

    const TransportMode mode = g_mode.load();
    if (mode != TransportMode::Netplay && mode != TransportMode::LocalLoopback
        && mode != TransportMode::Spectator)
    {
        return false;
    }

    return g_initialized.load() && g_bridgeActive.load() && menu::IsFightInputAllowedNow();
}

TransportMode GetTransportMode()
{
    return g_mode.load();
}

void SetTransportMode(TransportMode mode)
{
    g_mode.store(mode);
    if (mode != TransportMode::Netplay && mode != TransportMode::LocalLoopback
        && mode != TransportMode::Spectator)
    {
        g_hasRemoteInput.store(false);
        g_remoteWireMask.store(0);
        g_remoteRecord = {};
        g_spectatorP1Word.store(0u);
        g_spectatorP2Word.store(0u);
        netplay_input::RestoreCleanupPair();
    }
    lifecycle::Phase(
        "caster",
        "transport",
        mode == TransportMode::Netplay ? "netplay"
            : mode == TransportMode::LocalLoopback ? "loopback"
            : mode == TransportMode::Spectator ? "spectator"
                                                   : "offline",
        "SetTransportMode");
    log::Info("caster", "transport mode -> %u", static_cast<unsigned>(mode));
}

bool IsNetplayActive()
{
    return g_mode.load() == TransportMode::Netplay;
}

bool IsSpectatorActive()
{
    return g_mode.load() == TransportMode::Spectator;
}

int GetLocalSide()
{
    return g_localSide.load();
}

int GetRemoteSide()
{
    return g_localSide.load() == 0 ? 1 : 0;
}

void SetLocalSide(int side)
{
    if (side == 0 || side == 1)
    {
        g_localSide.store(side);
    }
}

int LocalBindingPlayerIndex()
{
    if (IsNetplayActive())
    {
        return 0;
    }
    return g_localSide.load();
}

std::uint8_t GetLocalWireMask()
{
    return g_localWireMask.load();
}

std::uint8_t SampleSyncWireMaskForCapture()
{
    // Pump SDL here: Wine controller state only advances while events are pumped.
    if (g_mode.load(std::memory_order_acquire) == TransportMode::Netplay)
    {
        input::PumpControllerEventsOnGameThread();
    }
    const menu::NavScreen navScreen = menu::DetectNavScreen();
    const std::uint8_t raw = codec::SampleSdlWireMask();
    if (navScreen == menu::NavScreen::CharSelect)
    {
        constexpr std::uint8_t kSlotB = 1u << 5u;
        const std::uint8_t filtered = static_cast<std::uint8_t>(raw & ~kSlotB);
        if ((raw & kSlotB) != 0u)
        {
            lifecycle::Input(
                "charsel",
                "strip B from wire capture raw=0x%02X wire=0x%02X",
                static_cast<unsigned>(raw),
                static_cast<unsigned>(filtered));
        }
        return filtered;
    }
    return raw;
}

std::uint8_t GetRemoteWireMask()
{
    return g_remoteWireMask.load();
}

bool HasRemoteInput()
{
    return g_hasRemoteInput.load();
}

proto::PlayerInputRecord GetLocalRecord()
{
    return g_localRecord;
}

proto::PlayerInputRecord GetRemoteRecord()
{
    return g_remoteRecord;
}

const proto::InputQueue& GetLocalQueue()
{
    return g_localQueue;
}

void PushRemoteRecord(const proto::PlayerInputRecord& record)
{
    g_remoteRecord = record;
    g_remoteWireMask.store(record.playerAction);
    g_hasRemoteInput.store(true);
}

void RefreshLockstepWireInject()
{
    if (g_mode.load() != TransportMode::Netplay || rollback::IsEnabled())
    {
        return;
    }

    UpdateLockstepInjectRecords();
}

void RefreshNetplayWireInject()
{
    if (g_mode.load() != TransportMode::Netplay)
    {
        return;
    }

    if (rollback::IsEnabled())
    {
        if (!rollback::IsInputAuthorityActive())
        {
            g_localWireMask.store(0);
            g_hasRemoteInput.store(false);
            g_remoteWireMask.store(0);
            input::SetMergedSystemState(rollback::GetMergedSystemState());
            return;
        }

        std::uint8_t localMask = 0;
        std::uint8_t remoteMask = 0;
        const int localSide = GetLocalSide();
        const int remoteSide = GetRemoteSide();
        const bool hasLocal = rollback::TryGetWireMaskForPlayerSide(localSide, &localMask);
        const bool hasRemote = rollback::TryGetWireMaskForPlayerSide(remoteSide, &remoteMask);

        g_localWireMask.store(hasLocal ? localMask : 0u);
        g_localRecord.playerAction = hasLocal ? localMask : 0u;
        if (hasRemote)
        {
            proto::PlayerInputRecord remote{};
            remote.playerAction = remoteMask;
            PushRemoteRecord(remote);
        }
        else
        {
            g_hasRemoteInput.store(false);
            g_remoteWireMask.store(0);
        }
        input::SetMergedSystemState(rollback::GetMergedSystemState());
        return;
    }

    UpdateLockstepInjectRecords();
}

void EnqueueLocalForSync(std::uint32_t frame)
{
    (void)frame;
    PushLocalQueue(g_localRecord);
}

bool TryGetWireMaskForPlayerSide(const int playerSide, std::uint8_t* outMask)
{
    if (outMask == nullptr)
    {
        return false;
    }

    if (rollback::IsInputAuthorityActive())
    {
        return rollback::TryGetWireMaskForPlayerSide(playerSide, outMask);
    }

    const TransportMode mode = g_mode.load();
    if (mode != TransportMode::Netplay && mode != TransportMode::LocalLoopback
        && mode != TransportMode::Spectator)
    {
        return false;
    }

    if (mode == TransportMode::Spectator)
    {
        const std::uint16_t word = playerSide == 0
            ? g_spectatorP1Word.load(std::memory_order_acquire)
            : g_spectatorP2Word.load(std::memory_order_acquire);
        if (playerSide != 0 && playerSide != 1)
        {
            *outMask = 0u;
            return false;
        }
        *outMask = static_cast<std::uint8_t>(word & 0xFFu);
        return true;
    }

    if (mode == TransportMode::Netplay)
    {
        RefreshNetplayWireInject();
    }

    const int localSide = GetLocalSide();
    const int remoteSide = GetRemoteSide();

    if (playerSide == localSide)
    {
        *outMask = GetLocalWireMask();
        return true;
    }

    if (playerSide == remoteSide)
    {
        if (mode == TransportMode::LocalLoopback)
        {
            *outMask = GetLocalWireMask();
            return true;
        }

        if (HasRemoteInput())
        {
            *outMask = GetRemoteWireMask();
            return true;
        }
    }

    *outMask = 0;
    return false;
}

void SetSpectatorInputWords(const std::uint16_t p1Word, const std::uint16_t p2Word)
{
    const auto sanitize = [](const std::uint16_t word) -> std::uint16_t {
        const std::uint8_t action = static_cast<std::uint8_t>(word & 0xFFu);
        const std::uint8_t system = menu::FilterRecordedSystemState(
            static_cast<std::uint8_t>(word >> 8u));
        return static_cast<std::uint16_t>(
            action | (static_cast<std::uint16_t>(system) << 8u));
    };
    const std::uint16_t safeP1 = sanitize(p1Word);
    const std::uint16_t safeP2 = sanitize(p2Word);
    g_spectatorP1Word.store(safeP1, std::memory_order_release);
    g_spectatorP2Word.store(safeP2, std::memory_order_release);
    const std::uint8_t mergedSystem = static_cast<std::uint8_t>(
        static_cast<std::uint8_t>(safeP1 >> 8u)
        | static_cast<std::uint8_t>(safeP2 >> 8u));
    input::SetMergedSystemState(
        static_cast<std::uint8_t>(mergedSystem & menu::kSystemStateEsc));
}

}
