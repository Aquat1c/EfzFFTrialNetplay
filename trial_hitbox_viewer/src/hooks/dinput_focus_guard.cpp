#include "dinput_focus_guard.h"

#include "dinput_relay.h"
#include "win32_compat.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace thv
{
namespace
{

// Slots: IDirectInputA[3] = CreateDevice, IDirectInputDeviceA[7] = Acquire.
constexpr int kSlotCreateDevice = 3;
constexpr int kSlotAcquire = 7;

using DirectInputCreateAFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, void**, void*);
using CreateDeviceFn = HRESULT(__stdcall*)(void*, const GUID*, void**, void*);
using AcquireFn = HRESULT(__stdcall*)(void*);

std::atomic<DirectInputCreateAFn> g_origCreate{nullptr};
void** g_iatSlot = nullptr;

struct TrackedSlot
{
    void** slot = nullptr;
    void* original = nullptr;
};

TrackedSlot g_createDeviceSlots[4] = {};
unsigned g_createDeviceSlotCount = 0u;
TrackedSlot g_acquireSlots[8] = {};
unsigned g_acquireSlotCount = 0u;
std::atomic<unsigned> g_rescues{0};
SRWLOCK g_registryLock = SRWLOCK_INIT;

struct RegistrySharedGuard
{
    RegistrySharedGuard() { AcquireSRWLockShared(&g_registryLock); }
    ~RegistrySharedGuard() { ReleaseSRWLockShared(&g_registryLock); }
};

struct RegistryExclusiveGuard
{
    RegistryExclusiveGuard() { AcquireSRWLockExclusive(&g_registryLock); }
    ~RegistryExclusiveGuard() { ReleaseSRWLockExclusive(&g_registryLock); }
};

template <std::size_t N>
void* OriginalForObject(
    void* const object,
    const int slotIndex,
    const TrackedSlot (&slots)[N],
    const unsigned* const count)
{
    if (!win32::IsReadableRange(object, sizeof(void*)))
    {
        return nullptr;
    }
    void** const vtbl = *reinterpret_cast<void***>(object);
    if (!win32::IsReadableRange(vtbl, (static_cast<std::size_t>(slotIndex) + 1u) * sizeof(void*)))
    {
        return nullptr;
    }
    void** const objectSlot = &vtbl[slotIndex];
    if (count == nullptr)
    {
        return nullptr;
    }
    RegistrySharedGuard registryGuard;
    for (unsigned index = 0u; index < *count && index < N; ++index)
    {
        if (slots[index].slot == objectSlot)
        {
            return slots[index].original;
        }
    }
    return nullptr;
}

bool PatchPointerExpected(void** slot, void* expected, void* replacement)
{
    if (slot == nullptr || expected == nullptr || replacement == nullptr
        || expected == replacement)
    {
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
    {
        return false;
    }
    void* const observed = InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(slot), replacement, expected);
    DWORD ignored = 0u;
    if (!VirtualProtect(slot, sizeof(void*), oldProtect, &ignored))
    {
        OutputDebugStringA(
            "netplay: DirectInput pointer patched but page protection restore failed\n");
    }
    return observed == expected || observed == replacement;
}

template <std::size_t N>
bool PatchTrackedSlot(
    void** const slot,
    void* const replacement,
    TrackedSlot (&slots)[N],
    unsigned* const count)
{
    if (slot == nullptr || replacement == nullptr || count == nullptr)
    {
        return false;
    }
    RegistryExclusiveGuard registryGuard;
    for (unsigned index = 0u; index < *count && index < N; ++index)
    {
        if (slots[index].slot == slot)
        {
            return *slot == replacement;
        }
    }
    if (*count >= N || *slot == nullptr || *slot == replacement)
    {
        return false;
    }
    void* const original = *slot;
    // Publish before any object can dispatch through the slot; retain once exposed.
    const unsigned recordIndex = *count;
    slots[recordIndex] = {slot, original};
    ++(*count);
    if (!PatchPointerExpected(slot, original, replacement))
    {
        // Never exposed by this attempt, so the tail record is safe to recycle.
        --(*count);
        slots[recordIndex] = {};
        return false;
    }
    return true;
}

HRESULT __stdcall GuardedAcquire(void* device)
{
    const auto original = reinterpret_cast<AcquireFn>(
        OriginalForObject(device, kSlotAcquire, g_acquireSlots, &g_acquireSlotCount));
    const HRESULT hr = original != nullptr ? original(device) : E_FAIL;
    if (hr == static_cast<HRESULT>(0x80070005L)) // E_ACCESSDENIED: window not foreground
    {
        g_rescues.fetch_add(1, std::memory_order_relaxed);
        // Loader lock may be held: OutputDebugString only.
        OutputDebugStringA("netplay: DI Acquire E_ACCESSDENIED swallowed (no foreground focus)\n");
        return 0; // DI_OK
    }
    return hr;
}

HRESULT __stdcall GuardedCreateDevice(void* self, const GUID* rguid, void** outDevice, void* outer)
{
    const auto original = reinterpret_cast<CreateDeviceFn>(
        OriginalForObject(self, kSlotCreateDevice, g_createDeviceSlots, &g_createDeviceSlotCount));
    const HRESULT hr = original != nullptr
        ? original(self, rguid, outDevice, outer)
        : E_FAIL;
    if (SUCCEEDED(hr) && outDevice != nullptr && *outDevice != nullptr)
    {
        void** vtbl = *reinterpret_cast<void***>(*outDevice);
        if (vtbl != nullptr && vtbl[kSlotAcquire] != reinterpret_cast<void*>(&GuardedAcquire))
        {
            // Wine may vary the implementation per device class: keep the original per vtable.
            (void)PatchTrackedSlot(
                &vtbl[kSlotAcquire],
                reinterpret_cast<void*>(&GuardedAcquire),
                g_acquireSlots,
                &g_acquireSlotCount);
        }

        // Attach here on the game thread: later sampling can race Release() on Wine.
        OnDinputDeviceCreated(rguid, *outDevice);
    }
    return hr;
}

HRESULT WINAPI GuardedDirectInputCreateA(HINSTANCE inst, DWORD version, void** outDI, void* outer)
{
    const DirectInputCreateAFn original = g_origCreate.load(std::memory_order_acquire);
    const HRESULT hr = original != nullptr ? original(inst, version, outDI, outer) : E_FAIL;
    if (SUCCEEDED(hr) && outDI != nullptr && *outDI != nullptr)
    {
        void** vtbl = *reinterpret_cast<void***>(*outDI);
        if (vtbl != nullptr && vtbl[kSlotCreateDevice] != reinterpret_cast<void*>(&GuardedCreateDevice))
        {
            (void)PatchTrackedSlot(
                &vtbl[kSlotCreateDevice],
                reinterpret_cast<void*>(&GuardedCreateDevice),
                g_createDeviceSlots,
                &g_createDeviceSlotCount);
        }
    }
    return hr;
}

}

bool InstallDinputFocusGuard()
{
    const HMODULE host = GetModuleHandleW(nullptr);
    if (host == nullptr)
    {
        return false;
    }
    g_iatSlot = win32::FindImportAddressSlot(host, "DINPUT.dll", "DirectInputCreateA");
    if (g_iatSlot == nullptr)
    {
        return false;
    }
    void* const original = *g_iatSlot;
    if (original == nullptr || original == reinterpret_cast<void*>(&GuardedDirectInputCreateA))
    {
        return false;
    }
    // Publish the upstream before exposing the guard; the CAS is the release boundary.
    g_origCreate.store(reinterpret_cast<DirectInputCreateAFn>(original), std::memory_order_release);
    if (!PatchPointerExpected(
            g_iatSlot, original, reinterpret_cast<void*>(&GuardedDirectInputCreateA)))
    {
        g_origCreate.store(nullptr, std::memory_order_release);
        return false;
    }
    return true;
}

void ReassertDinputFocusGuard()
{
    if (g_iatSlot == nullptr || *g_iatSlot == reinterpret_cast<void*>(&GuardedDirectInputCreateA))
    {
        return;
    }
    // A later wrapper chains to our guard, so re-patching on top would recurse.
    OutputDebugStringA(
        "netplay: DirectInputCreateA IAT changed after guard install; preserving downstream hook chain\n");
}

unsigned DinputAcquireRescues()
{
    return g_rescues.load(std::memory_order_relaxed);
}

void ShutdownDinputFocusGuard()
{
    RegistryExclusiveGuard registryGuard;
    for (unsigned index = 0u; index < g_acquireSlotCount; ++index)
    {
        void** const slot = g_acquireSlots[index].slot;
        if (slot != nullptr && *slot == reinterpret_cast<void*>(&GuardedAcquire)
            && g_acquireSlots[index].original != nullptr)
        {
            (void)PatchPointerExpected(
                slot,
                reinterpret_cast<void*>(&GuardedAcquire),
                g_acquireSlots[index].original);
        }
    }
    for (unsigned index = 0u; index < g_createDeviceSlotCount; ++index)
    {
        void** const slot = g_createDeviceSlots[index].slot;
        if (slot != nullptr && *slot == reinterpret_cast<void*>(&GuardedCreateDevice)
            && g_createDeviceSlots[index].original != nullptr)
        {
            (void)PatchPointerExpected(
                slot,
                reinterpret_cast<void*>(&GuardedCreateDevice),
                g_createDeviceSlots[index].original);
        }
    }
    const DirectInputCreateAFn originalCreate = g_origCreate.load(std::memory_order_acquire);
    if (g_iatSlot != nullptr
        && *g_iatSlot == reinterpret_cast<void*>(&GuardedDirectInputCreateA)
        && originalCreate != nullptr)
    {
        (void)PatchPointerExpected(
            g_iatSlot,
            reinterpret_cast<void*>(&GuardedDirectInputCreateA),
            reinterpret_cast<void*>(originalCreate));
    }
    // Keep forwarding records after exposure: a wrapper or in-flight call can enter.
}

}
