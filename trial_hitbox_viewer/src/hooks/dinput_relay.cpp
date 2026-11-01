#include "dinput_relay.h"

#include "caster_input.h"
#include "control_swap.h"
#include "debug_log.h"
#include "lifecycle_log.h"
#include "menu_nav.h"
#include "rollback.h"
#include "sdl_input.h"
#include "trial_addresses.h"
#include "trial_input_codec.h"
#include "win32_compat.h"

#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <dinput.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace thv
{
namespace
{

// Vtable index 9 = GetDeviceState; other slots would unbalance the stack.
constexpr int kDiVtableGetDeviceState = 9;
constexpr int kDiVtableRelease = 2;
constexpr std::size_t kMaxTrackedKeyboardDevices = 4u;
constexpr std::size_t kMaxTrackedDeviceVtables = 8u;

constexpr GUID kSystemKeyboardGuid = {
    0x6F1D2B61u,
    0xD5A0u,
    0x11CFu,
    {0xBFu, 0xC7u, 0x44u, 0x45u, 0x53u, 0x54u, 0x00u, 0x00u}};

using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8*, DWORD, LPVOID);
using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(IDirectInputDevice8*);

HRESULT STDMETHODCALLTYPE HookedGetDeviceState(
    IDirectInputDevice8* self, DWORD cbData, LPVOID lpvData);
ULONG STDMETHODCALLTYPE HookedRelease(IDirectInputDevice8* self);

struct VtableRelay
{
    // DINPUT reads metadata next to the native vtable, so a cloned vptr is invalid.
    std::atomic<void**> vtable{nullptr};
    GetDeviceStateFn originalGetDeviceState = nullptr;
    ReleaseFn originalRelease = nullptr;
    std::atomic<bool> releaseExposed{false};
    std::atomic<bool> getDeviceStateExposed{false};
    std::atomic<bool> usable{false};
};

std::atomic<bool> g_initialized{false};
std::atomic<bool> g_attachFailed{false};
std::atomic<bool> g_forwardAttachFailed{false};
std::atomic<bool> g_attachReported{false};
std::atomic<bool> g_authorityCallReported{false};
std::atomic<bool> g_onlineOverlayReported{false};
std::atomic<bool> g_installFailureLogged{false};
std::atomic<IDirectInputDevice8*> g_createdKeyboardCandidate{nullptr};
// Only this exact object may modify EFZ's state; other native devices only forward.
std::atomic<IDirectInputDevice8*> g_overlayAuthority{nullptr};
std::atomic<IDirectInputDevice8*> g_keyboardDevices[kMaxTrackedKeyboardDevices] = {};
VtableRelay g_vtableRelays[kMaxTrackedDeviceVtables] = {};
SRWLOCK g_vtableInstallLock = SRWLOCK_INIT;

struct VtableInstallGuard
{
    VtableInstallGuard() { AcquireSRWLockExclusive(&g_vtableInstallLock); }
    ~VtableInstallGuard() { ReleaseSRWLockExclusive(&g_vtableInstallLock); }
};

void** ClaimedVtableSentinel()
{
    return reinterpret_cast<void**>(static_cast<std::uintptr_t>(1u));
}

VtableRelay* FindRelayForVtable(void** const vtable)
{
    if (vtable == nullptr)
    {
        return nullptr;
    }
    for (VtableRelay& relay : g_vtableRelays)
    {
        if (relay.vtable.load(std::memory_order_acquire) == vtable)
        {
            return &relay;
        }
    }
    return nullptr;
}

VtableRelay* FindRelayForDevice(IDirectInputDevice8* const device)
{
    if (device == nullptr)
    {
        return nullptr;
    }
    return FindRelayForVtable(*reinterpret_cast<void***>(device));
}

bool IsTrackedKeyboard(IDirectInputDevice8* const device)
{
    for (const auto& tracked : g_keyboardDevices)
    {
        if (tracked.load(std::memory_order_acquire) == device)
        {
            return true;
        }
    }
    return false;
}

bool RegisterKeyboard(IDirectInputDevice8* const device)
{
    if (IsTrackedKeyboard(device))
    {
        return true;
    }
    for (auto& tracked : g_keyboardDevices)
    {
        IDirectInputDevice8* expected = nullptr;
        if (tracked.compare_exchange_strong(
                expected,
                device,
                std::memory_order_release,
                std::memory_order_acquire))
        {
            return true;
        }
    }
    return false;
}

void RetireKeyboard(IDirectInputDevice8* const device)
{
    for (auto& tracked : g_keyboardDevices)
    {
        IDirectInputDevice8* expected = device;
        (void)tracked.compare_exchange_strong(
            expected,
            nullptr,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    IDirectInputDevice8* expectedAuthority = device;
    if (g_overlayAuthority.compare_exchange_strong(
            expectedAuthority,
            nullptr,
            std::memory_order_acq_rel,
            std::memory_order_acquire)
        && g_initialized.load(std::memory_order_acquire))
    {
        // Fail closed: physical input must not reach one peer.
        g_attachFailed.store(true, std::memory_order_release);
    }
}

bool PatchSharedSlot(void** const slot, void* const expected, void* const replacement)
{
    if (slot == nullptr || expected == nullptr || replacement == nullptr
        || expected == replacement)
    {
        return false;
    }
    DWORD oldProtect = 0u;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
    {
        return false;
    }
    void* const observed = InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(slot), replacement, expected);
    DWORD ignored = 0u;
    const BOOL protectionRestored = VirtualProtect(
        slot, sizeof(void*), oldProtect, &ignored);
    if (observed != expected)
    {
        return false;
    }
    if (protectionRestored == FALSE)
    {
        OutputDebugStringA(
            "netplay: DirectInput shared vtable slot patched but protection restore failed\n");
    }
    return true;
}

void ReportAttachedIfReady()
{
    if (!g_initialized.load(std::memory_order_acquire)
        || g_overlayAuthority.load(std::memory_order_acquire) == nullptr
        || g_attachReported.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    lifecycle::Init("hooks", "DirectInput GetDeviceState relay active SDL overlay");
    log::Info(
        "hooks",
        "DirectInput game-facing keyboard authority ready (object vtable identity preserved; shared slots Release-tracked)");
}

IDirectInputDevice8* ReadGameFacingKeyboard()
{
    auto* const slot = static_cast<void**>(trial::ResolveVa(trial::kVaKeyboardDevice));
    if (!win32::IsReadableRange(slot, sizeof(void*)))
    {
        return nullptr;
    }
    // Interlocked so the double read below detects a lifecycle transition.
    return static_cast<IDirectInputDevice8*>(InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(slot), nullptr, nullptr));
}

bool TryOverlayNetplayBindings(std::uint8_t* buffer, const menu::NavScreen screen)
{
    if (buffer == nullptr
        || (!caster::IsNetplayActive() && !caster::IsSpectatorActive()))
    {
        return false;
    }

    if (caster::IsNetplayActive())
    {
        caster::RefreshNetplayWireInject();
    }

    if (screen == menu::NavScreen::MainMenu || screen == menu::NavScreen::Title)
    {
        std::uint8_t navMask = 0;
        const bool available = caster::TryGetWireMaskForPlayerSide(0, &navMask);
        lifecycle::DiOverlay("wire_p1_nav", navMask, 0, available, static_cast<int>(screen));
        std::memset(buffer, 0, 256u);
        if (available)
        {
            input::MergeWireMaskIntoDiBuffer(buffer, 0, navMask);
        }
        input::ApplySystemStateToDiBuffer(buffer);
        return true;
    }

    if (menu::IsFightInputAllowed(screen) || menu::UsesLockstepWireDiOverlay(screen))
    {
        lifecycle::DiOverlay("system_only", 0, 0, false, static_cast<int>(screen));
        std::memset(buffer, 0, 256u);
        input::ApplySystemStateToDiBuffer(buffer);
        return true;
    }

    if (menu::IsSyncEligible(screen))
    {
        lifecycle::DiOverlay("local_bindings", 0, 0, false, static_cast<int>(screen));
        std::memset(buffer, 0, 256u);
        input::MergeLocalNetplayBindingsIntoDiBuffer(buffer);
        input::ApplySystemStateToDiBuffer(buffer);
        return true;
    }

    // Fail closed on unknown screens: the native buffer would bypass the policy.
    lifecycle::DiOverlay("unknown_online_neutral", 0, 0, false, static_cast<int>(screen));
    std::memset(buffer, 0, 256u);
    input::ApplySystemStateToDiBuffer(buffer);
    return true;
}

void OverlayFightBindings(std::uint8_t* buffer, const menu::NavScreen screen)
{
    if (buffer == nullptr)
    {
        return;
    }

    if (TryOverlayNetplayBindings(buffer, screen))
    {
        return;
    }

    if (!menu::IsFightInputAllowed(screen))
    {
        return;
    }

    input::SetMergedSystemState(0);
    if (BothPlayersUseHumanInput())
    {
        input::OverlayDiKeyboardBufferBothPlayers(buffer);
    }
    else
    {
        input::OverlayDiKeyboardBuffer(buffer, GetHumanSide());
    }
}

bool OverlayOfflineManagedBindings(std::uint8_t* const buffer, const menu::NavScreen screen)
{
    if (buffer == nullptr || caster::IsNetplayActive() || caster::IsSpectatorActive()
        || !menu::IsSyncEligible(screen))
    {
        return false;
    }

    std::uint8_t p1Mask = 0u;
    std::uint8_t p2Mask = 0u;
    if (screen == menu::NavScreen::MainMenu || screen == menu::NavScreen::Title)
    {
        p1Mask = codec::SampleSdlWireMaskForPlayer(0);
        input::ReplacePlayerInDiBuffer(buffer, 0, p1Mask);
    }
    else if (BothPlayersUseHumanInput())
    {
        p1Mask = codec::SampleSdlWireMaskForPlayer(0);
        p2Mask = codec::SampleSdlWireMaskForPlayer(1);
        input::ReplaceBothPlayersInDiBuffer(buffer, p1Mask, p2Mask);
    }
    else
    {
        const int side = GetHumanSide();
        const std::uint8_t mask = codec::SampleSdlWireMaskForPlayer(side);
        p1Mask = side == 0 ? mask : 0u;
        p2Mask = side == 1 ? mask : 0u;
        input::ReplacePlayerInDiBuffer(buffer, side, mask);
    }
    lifecycle::DiOverlay(
        "offline_selected_source",
        p1Mask,
        p2Mask,
        true,
        static_cast<int>(screen));
    input::ApplyOfflinePracticeBindingsToDiBuffer(buffer);
    return true;
}

HRESULT STDMETHODCALLTYPE HookedGetDeviceState(IDirectInputDevice8* self, DWORD cbData, LPVOID lpvData)
{
    VtableRelay* const relay = FindRelayForDevice(self);
    const GetDeviceStateFn original = relay != nullptr
        ? relay->originalGetDeviceState
        : nullptr;
    if (original == nullptr)
    {
        if (lpvData != nullptr && cbData == 256u)
        {
            std::memset(lpvData, 0, 256u);
        }
        return DIERR_NOTINITIALIZED;
    }

    // Run the next layer first: the ddraw.dll wrapper may return a neutral buffer.
    const HRESULT nativeResult = original(self, cbData, lpvData);
    if (FAILED(nativeResult))
    {
        // Keep DIERR_INPUTLOST/NOTACQUIRED: EFZ re-Acquires. Never manufacture input on failure.
        if (lpvData != nullptr && cbData == 256u)
        {
            std::memset(lpvData, 0, 256u);
        }
        return nativeResult;
    }

    // Load the authority after the forwarded call: Release may clear it meanwhile.
    if (!g_initialized.load(std::memory_order_acquire)
        || g_overlayAuthority.load(std::memory_order_acquire) != self
        || cbData != 256u
        || lpvData == nullptr)
    {
        return nativeResult;
    }

    auto* const buffer = static_cast<std::uint8_t*>(lpvData);
    input::PumpControllerEventsOnGameThread();
    const menu::NavScreen screen = menu::DetectNavScreen();
    if (!g_authorityCallReported.exchange(true, std::memory_order_acq_rel))
    {
        log::Info(
            "hooks",
            "DirectInput overlay authority observed device=%p cb=%lu screen=%d native_hr=0x%08lX",
            static_cast<void*>(self),
            static_cast<unsigned long>(cbData),
            static_cast<int>(screen),
            static_cast<unsigned long>(nativeResult));
    }

    if (TryOverlayNetplayBindings(buffer, screen))
    {
        if (!g_onlineOverlayReported.exchange(true, std::memory_order_acq_rel))
        {
            lifecycle::Init(
                "hooks",
                "DirectInput confirmed-input overlay observed device=%p screen=%d",
                static_cast<void*>(self),
                static_cast<int>(screen));
            log::Info(
                "hooks",
                "DirectInput online overlay observed device=%p cb=%lu screen=%d native_hr=0x%08lX",
                static_cast<void*>(self),
                static_cast<unsigned long>(cbData),
                static_cast<int>(screen),
                static_cast<unsigned long>(nativeResult));
        }
        return nativeResult;
    }

    if (!OverlayOfflineManagedBindings(buffer, screen))
    {
        OverlayFightBindings(buffer, screen);
    }
    return nativeResult;
}

ULONG STDMETHODCALLTYPE HookedRelease(IDirectInputDevice8* const self)
{
    VtableRelay* const relay = FindRelayForDevice(self);
    const ReleaseFn original = relay != nullptr ? relay->originalRelease : nullptr;
    if (original == nullptr)
    {
        return 0u;
    }

    // Zero means freed: do not touch the object. Clear the filter for recycled addresses.
    const ULONG remaining = original(self);
    if (remaining == 0u)
    {
        RetireKeyboard(self);
    }
    return remaining;
}

bool TryInstallSharedVtableHooks(IDirectInputDevice8* const device)
{
    if (!win32::IsReadableRange(device, sizeof(void*)))
    {
        return false;
    }

    void** const vtable = *reinterpret_cast<void***>(device);
    if (vtable == nullptr
        || !win32::IsReadableRange(
            vtable, (static_cast<std::size_t>(kDiVtableGetDeviceState) + 1u) * sizeof(void*)))
    {
        return false;
    }

    VtableInstallGuard installGuard;
    VtableRelay* const existing = FindRelayForVtable(vtable);
    if (existing != nullptr)
    {
        if (!existing->getDeviceStateExposed.load(std::memory_order_acquire))
        {
            if (vtable[kDiVtableGetDeviceState]
                == reinterpret_cast<void*>(&HookedGetDeviceState))
            {
                existing->getDeviceStateExposed.store(true, std::memory_order_release);
            }
            else if (!PatchSharedSlot(
                         &vtable[kDiVtableGetDeviceState],
                         reinterpret_cast<void*>(existing->originalGetDeviceState),
                         reinterpret_cast<void*>(&HookedGetDeviceState)))
            {
                return false;
            }
            else
            {
                existing->getDeviceStateExposed.store(true, std::memory_order_release);
            }
        }
        // A later wrapper may have displaced a slot: publish only if both still point to us.
        const bool usable = existing->releaseExposed.load(std::memory_order_acquire)
            && existing->getDeviceStateExposed.load(std::memory_order_acquire)
            && vtable[kDiVtableRelease] == reinterpret_cast<void*>(&HookedRelease)
            && vtable[kDiVtableGetDeviceState]
                == reinterpret_cast<void*>(&HookedGetDeviceState);
        existing->usable.store(usable, std::memory_order_release);
        const bool registered = usable && RegisterKeyboard(device);
        if (registered)
        {
            ReportAttachedIfReady();
        }
        return registered;
    }

    const auto original = reinterpret_cast<GetDeviceStateFn>(
        vtable[kDiVtableGetDeviceState]);
    const auto originalRelease = reinterpret_cast<ReleaseFn>(vtable[kDiVtableRelease]);
    if (original == nullptr || originalRelease == nullptr
        || original == &HookedGetDeviceState || originalRelease == &HookedRelease)
    {
        return false;
    }

    VtableRelay* claimed = nullptr;
    for (VtableRelay& relay : g_vtableRelays)
    {
        void** expected = nullptr;
        if (relay.vtable.compare_exchange_strong(
                expected,
                ClaimedVtableSentinel(),
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            claimed = &relay;
            break;
        }
    }
    if (claimed == nullptr)
    {
        return false;
    }

    claimed->originalGetDeviceState = original;
    claimed->originalRelease = originalRelease;
    claimed->releaseExposed.store(false, std::memory_order_release);
    claimed->getDeviceStateExposed.store(false, std::memory_order_release);
    claimed->usable.store(false, std::memory_order_release);
    claimed->vtable.store(vtable, std::memory_order_release);

    // Patch Release first so the keyboard address can never outlive the object.
    if (!PatchSharedSlot(
            &vtable[kDiVtableRelease],
            reinterpret_cast<void*>(originalRelease),
            reinterpret_cast<void*>(&HookedRelease)))
    {
        // No slot ever pointed at us, so this unpublished record is safe to recycle.
        claimed->originalGetDeviceState = nullptr;
        claimed->originalRelease = nullptr;
        claimed->vtable.store(nullptr, std::memory_order_release);
        return false;
    }
    claimed->releaseExposed.store(true, std::memory_order_release);
    if (!PatchSharedSlot(
            &vtable[kDiVtableGetDeviceState],
            reinterpret_cast<void*>(original),
            reinterpret_cast<void*>(&HookedGetDeviceState)))
    {
        // Keep the record: Release was exposed, so a call may still enter HookedRelease.
        return false;
    }
    claimed->getDeviceStateExposed.store(true, std::memory_order_release);
    const bool visible = vtable[kDiVtableRelease] == reinterpret_cast<void*>(&HookedRelease)
        && vtable[kDiVtableGetDeviceState]
            == reinterpret_cast<void*>(&HookedGetDeviceState);
    claimed->usable.store(visible, std::memory_order_release);
    if (!visible)
    {
        return false;
    }
    if (!RegisterKeyboard(device))
    {
        // Never clear a mapping after either shared slot was exposed.
        return false;
    }

    ReportAttachedIfReady();
    return true;
}

bool BindGameFacingKeyboardAuthority()
{
    // Read once after the init gate. A live rebind needs a game-thread handoff.
    IDirectInputDevice8* const first = ReadGameFacingKeyboard();
    if (first == nullptr || !TryInstallSharedVtableHooks(first))
    {
        return false;
    }

    IDirectInputDevice8* const second = ReadGameFacingKeyboard();
    if (second != first)
    {
        RetireKeyboard(first);
        return false;
    }

    void** const vtable = *reinterpret_cast<void***>(first);
    VtableRelay* const relay = FindRelayForVtable(vtable);
    if (relay == nullptr || !relay->usable.load(std::memory_order_acquire)
        || vtable[kDiVtableRelease] != reinterpret_cast<void*>(&HookedRelease)
        || vtable[kDiVtableGetDeviceState]
            != reinterpret_cast<void*>(&HookedGetDeviceState))
    {
        RetireKeyboard(first);
        return false;
    }

    // Forwarding records and both slots are published before this release-store.
    g_overlayAuthority.store(first, std::memory_order_release);
    g_attachFailed.store(false, std::memory_order_release);

    const IDirectInputDevice8* const created =
        g_createdKeyboardCandidate.load(std::memory_order_acquire);
    log::Info(
        "hooks",
        "DirectInput overlay authority bound source=EFZ+0x%05X device=%p vtable=%p same_as_created=%d forwarding_attach_failed=%d",
        static_cast<unsigned>(trial::VaToRva(trial::kVaKeyboardDevice)),
        static_cast<void*>(first),
        static_cast<void*>(vtable),
        created == first ? 1 : 0,
        g_forwardAttachFailed.load(std::memory_order_acquire) ? 1 : 0);
    ReportAttachedIfReady();
    return true;
}

bool IsSystemKeyboardGuid(const GUID* const deviceGuid)
{
    return deviceGuid != nullptr
        && std::memcmp(deviceGuid, &kSystemKeyboardGuid, sizeof(GUID)) == 0;
}

}

void OnDinputDeviceCreated(const GUID* const deviceGuid, void* const device)
{
    if (!IsSystemKeyboardGuid(deviceGuid) || device == nullptr)
    {
        return;
    }

    auto* const keyboard = static_cast<IDirectInputDevice8*>(device);
    g_createdKeyboardCandidate.store(keyboard, std::memory_order_release);
    if (!TryInstallSharedVtableHooks(keyboard))
    {
        g_forwardAttachFailed.store(true, std::memory_order_release);
    }
    else
    {
        g_forwardAttachFailed.store(false, std::memory_order_release);
    }
}

bool InitializeDinputRelay()
{
    if (g_initialized.exchange(true))
    {
        return true;
    }

    log::Info(
        "hooks",
        "DirectInput relay binding exact game-facing keyboard after Trial input initialization");
    g_installFailureLogged.store(false, std::memory_order_release);
    g_attachReported.store(false, std::memory_order_release);
    g_authorityCallReported.store(false, std::memory_order_release);
    g_onlineOverlayReported.store(false, std::memory_order_release);
    if (!BindGameFacingKeyboardAuthority())
    {
        g_attachFailed.store(true, std::memory_order_release);
        log::Error(
            "hooks",
            "DirectInput overlay authority unavailable or EFZ keyboard global changed during bind");
        return false;
    }
    return true;
}

void ShutdownDinputRelay()
{
    if (!g_initialized.exchange(false))
    {
        return;
    }

    g_overlayAuthority.store(nullptr, std::memory_order_release);

    // Leave the vtable slots chained: restoring could race a live DINPUT call.
}

void MaintainDinputRelay()
{
    if (!g_initialized.load(std::memory_order_acquire))
    {
        return;
    }

    IDirectInputDevice8* const authority =
        g_overlayAuthority.load(std::memory_order_acquire);
    if (authority != nullptr && ReadGameFacingKeyboard() != authority)
    {
        // A change is a lifecycle violation: lock-free switching could desync the peers.
        IDirectInputDevice8* expected = authority;
        (void)g_overlayAuthority.compare_exchange_strong(
            expected,
            nullptr,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
        g_attachFailed.store(true, std::memory_order_release);
    }

    if (!g_attachFailed.load(std::memory_order_acquire)
        || g_installFailureLogged.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    lifecycle::Fault(
        "hooks",
        "DirectInput game-facing keyboard authority was lost");
    log::Error(
        "hooks",
        "DirectInput authority unavailable/global changed; refusing controller/system overlay instead of accepting unsynchronized input");
    if (GetEnvironmentVariableW(L"TRIAL_CASTER_SESSION_INI", nullptr, 0u) > 1u)
    {
        log::FlushFile();
        ExitProcess(ERROR_DLL_INIT_FAILED);
    }
}

}
