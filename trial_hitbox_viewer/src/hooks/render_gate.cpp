#include "render_gate.h"

#include "caster_input.h"
#include "crash_handler.h"
#include "debug_log.h"
#include "lifecycle_log.h"
#include "rng.h"
#include "state_paths.h"
#include "trial_addresses.h"
#include "win32_compat.h"

#include <MinHook.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <vector>

namespace thv
{
namespace
{

using PresentFn = int(__cdecl*)();
PresentFn g_originalPresent = nullptr;
void* g_presentTarget = nullptr;

// __thiscall (this, bufferIndex, flags), modelled as __fastcall with a dummy EDX slot.
using PlaySoundFn = int(__fastcall*)(void* thisPtr, void* edx, int bufferIndex, int flags);
PlaySoundFn g_originalPlaySound = nullptr;
void* g_playSoundTarget = nullptr;

std::atomic<bool> g_initialized{false};

// Atomic: present and sound are reachable off the game thread's AdvanceOne.
std::atomic<bool> g_suppressed{false};
std::atomic<std::uint64_t> g_presentedFrames{0};
std::atomic<bool> g_audioFrameActive{false};
std::atomic<std::uint32_t> g_audioFrame{0};
std::mutex g_audioDedupMutex;
std::array<std::uint64_t, 512> g_audioDedupKeys{};
std::size_t g_audioDedupWrite = 0;
std::size_t g_audioDedupCount = 0;

int __cdecl HookedPresent()
{
    if (g_suppressed.load(std::memory_order_acquire))
    {
        return 0;
    }
    g_presentedFrames.fetch_add(1u, std::memory_order_relaxed);
    return g_originalPresent != nullptr ? g_originalPresent() : 0;
}

int __fastcall HookedPlaySound(void* thisPtr, void* edx, int bufferIndex, int flags)
{
    if (g_suppressed.load(std::memory_order_acquire))
    {
        // Buffer index mimics success; the SFX fires once on the visible frame.
        return bufferIndex;
    }
    if (g_audioFrameActive.load(std::memory_order_acquire))
    {
        const std::uint64_t key =
            (static_cast<std::uint64_t>(g_audioFrame.load(std::memory_order_acquire)) << 32u)
            | (static_cast<std::uint64_t>(static_cast<std::uint16_t>(bufferIndex)) << 16u)
            | static_cast<std::uint16_t>(flags);
        std::lock_guard<std::mutex> lock(g_audioDedupMutex);
        for (std::size_t i = 0; i < g_audioDedupCount; ++i)
        {
            if (g_audioDedupKeys[i] == key)
            {
                return bufferIndex;
            }
        }
        g_audioDedupKeys[g_audioDedupWrite++ % g_audioDedupKeys.size()] = key;
        if (g_audioDedupCount < g_audioDedupKeys.size())
        {
            ++g_audioDedupCount;
        }
    }
    return g_originalPlaySound != nullptr
        ? g_originalPlaySound(thisPtr, edx, bufferIndex, flags)
        : bufferIndex;
}

// The game checks only the sound table: skip a freed buffer before the vtable read.
using SoundSeekFn = int(__fastcall*)(char* thisPtr, void* edx, unsigned int bufferIndex, unsigned int soundId);
SoundSeekFn g_originalSoundSeek = nullptr;
void* g_soundSeekTarget = nullptr;
std::atomic<unsigned> g_soundSeekSkips{0};

bool SoundBufferReadable(const char* thisPtr, unsigned int index)
{
    if (thisPtr == nullptr)
    {
        return false;
    }
    const void* const* slot = reinterpret_cast<const void* const*>(thisPtr + 4u * index + 1208u);
    if (!win32::IsReadableRange(slot, sizeof(void*)))
    {
        return false;
    }
    const void* buf = *slot;
    const std::uintptr_t p = reinterpret_cast<std::uintptr_t>(buf);
    if (p == 0u || p == 0xDDDDDDDDu || p == 0xCDCDCDCDu || p == 0xFEEEFEEEu || p == 0xBAADF00Du)
    {
        return false;
    }
    if (!win32::IsReadableRange(buf, sizeof(void*)))
    {
        return false;
    }
    const void* vtbl = *reinterpret_cast<void* const*>(buf);
    return win32::IsReadableRange(vtbl, sizeof(void*));
}

int __fastcall HookedSoundSeek(char* thisPtr, void* edx, unsigned int bufferIndex, unsigned int soundId)
{
    if (!SoundBufferReadable(thisPtr, bufferIndex))
    {
        const unsigned n = g_soundSeekSkips.fetch_add(1) + 1u;
        if (n <= 4u || (n % 256u) == 0u)
        {
            log::Warn("hooks", "sound seek skipped — freed/invalid buffer idx=%u sound=%u (skips=%u)",
                bufferIndex, soundId, n);
        }
        return 0;
    }
    return g_originalSoundSeek != nullptr ? g_originalSoundSeek(thisPtr, edx, bufferIndex, soundId) : 0;
}

// Returning 1 (success) degrades a missing palette to default colors.
using AssetFileLoadFn = int(__cdecl*)(const char* lpFileName, int a2, int a3, int a4, int a5, int a6);
AssetFileLoadFn g_originalAssetFileLoad = nullptr;
void* g_assetFileLoadTarget = nullptr;
std::atomic<unsigned> g_assetLoadSkips{0};

int __cdecl HookedAssetFileLoad(const char* lpFileName, int a2, int a3, int a4, int a5, int a6)
{
    bool missing = lpFileName == nullptr;
    if (!missing && GetFileAttributesA(lpFileName) == INVALID_FILE_ATTRIBUTES)
    {
        // Only an absent file counts: ERROR_ACCESS_DENIED must reach the loader.
        const DWORD err = GetLastError();
        missing = err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
    }
    if (missing)
    {
        const unsigned n = g_assetLoadSkips.fetch_add(1) + 1u;
        if (n <= 8u || (n % 256u) == 0u)
        {
            log::Warn("hooks",
                "asset load skipped — file missing '%s' (would operator new(0xFFFFFFFF); skips=%u)",
                lpFileName != nullptr ? lpFileName : "(null)", n);
        }
        return 1;
    }
    return g_originalAssetFileLoad != nullptr
        ? g_originalAssetFileLoad(lpFileName, a2, a3, a4, a5, a6)
        : 1;
}

using CreateFileAFn = HANDLE(WINAPI*)(
    LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
void* g_originalCreateFileARaw = nullptr;
HMODULE g_createFileAHostModule = nullptr;
std::atomic<bool> g_createFileAIatHooked{false};
std::atomic<unsigned> g_missingFileRedirects{0};
std::atomic<int> g_dummyState{0}; // 0=uncreated, 1=creating, 2=ready, 3=failed
std::atomic<HANDLE> g_dummyAssetOwner{INVALID_HANDLE_VALUE};
wchar_t g_dummyAssetPath[32768] = {};

bool EnsureDummyAssetFile()
{
    int failed = 3;
    if (g_dummyState.compare_exchange_strong(failed, 0, std::memory_order_acq_rel))
    {
        const HANDLE staleOwner = g_dummyAssetOwner.exchange(
            INVALID_HANDLE_VALUE, std::memory_order_acq_rel);
        if (staleOwner != INVALID_HANDLE_VALUE)
        {
            CloseHandle(staleOwner);
        }
    }

    int expected = 0;
    if (g_dummyState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
    {
        bool ok = false;
        wchar_t leaf[96] = {};
        std::swprintf(
            leaf,
            std::size(leaf),
            L"TrialNetplay_blank_asset_%lu.bin",
            static_cast<unsigned long>(GetCurrentProcessId()));
        const std::filesystem::path dummyPath = paths::MutableStateRoot() / leaf;
        const std::wstring& dummyPathWide = dummyPath.native();
        if (!dummyPathWide.empty() && dummyPathWide.size() < std::size(g_dummyAssetPath))
        {
            std::wmemcpy(g_dummyAssetPath, dummyPathWide.data(), dummyPathWide.size());
            g_dummyAssetPath[dummyPathWide.size()] = L'\0';
            // Per-PID: loopback instances share a state root.
            (void)DeleteFileW(g_dummyAssetPath);
            HANDLE h = CreateFileW(
                g_dummyAssetPath,
                GENERIC_READ | GENERIC_WRITE | DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                const std::filesystem::path fallbackPath =
                    paths::MutableStateRoot() / L"TrialNetplay_blank_asset_fallback.bin";
                const std::wstring& fallbackWide = fallbackPath.native();
                if (!fallbackWide.empty()
                    && fallbackWide.size() < std::size(g_dummyAssetPath))
                {
                    std::wmemcpy(
                        g_dummyAssetPath, fallbackWide.data(), fallbackWide.size());
                    g_dummyAssetPath[fallbackWide.size()] = L'\0';
                    h = CreateFileW(
                        g_dummyAssetPath,
                        GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr,
                        OPEN_ALWAYS,
                        FILE_ATTRIBUTE_TEMPORARY,
                        nullptr);
                }
            }
            if (h != INVALID_HANDLE_VALUE)
            {
                SetFilePointer(h, 1 << 20, nullptr, FILE_BEGIN);
                if (SetEndOfFile(h) != FALSE)
                {
                    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
                    g_dummyAssetOwner.store(h, std::memory_order_release);
                    ok = true;
                }
                else
                {
                    CloseHandle(h);
                }
            }
        }
        g_dummyState.store(ok ? 2 : 3, std::memory_order_release);
    }
    // Real wait: Sleep(0) can burn through instantly.
    for (int i = 0; i < 500 && g_dummyState.load(std::memory_order_acquire) == 1; ++i)
    {
        Sleep(1);
    }
    return g_dummyState.load(std::memory_order_acquire) == 2;
}

// Returns INVALID_HANDLE_VALUE if unavailable; callers must fall back to the real result.
HANDLE OpenDummyAssetFile()
{
    if (!EnsureDummyAssetFile())
    {
        return INVALID_HANDLE_VALUE;
    }
    const HANDLE d = CreateFileW(
        g_dummyAssetPath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (d == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND)
    {
        g_dummyState.store(3, std::memory_order_release);
    }
    return d;
}

// Loaders ignore CreateSurface failures and the blit Locks unchecked: skip null surfaces.
using SurfaceBlitFn = int(__cdecl*)(
    int r1a, int r1b, int r1c, int r1d, void* surface,
    int r2a, int r2b, int r2c, int r2d, int flag, int mode);
SurfaceBlitFn g_originalSurfaceBlit = nullptr;
void* g_surfaceBlitTarget = nullptr;
std::atomic<unsigned> g_nullBlitSkips{0};

// Keep the gameplay-state clamps; skip only the draw on hidden replay.
using BattleSceneCompositeFn = void(__cdecl*)(int p1, int p2, int cameraX, int cameraY);
BattleSceneCompositeFn g_originalBattleSceneComposite = nullptr;
void* g_battleSceneCompositeTarget = nullptr;

void SanitizeCompositePlayerState(const int playerAddress)
{
    auto* const player = reinterpret_cast<std::uint8_t*>(
        static_cast<std::uintptr_t>(static_cast<std::uint32_t>(playerAddress)));
    if (player == nullptr)
    {
        return;
    }

    auto* const meter = reinterpret_cast<std::int16_t*>(player + 72u);
    *meter = std::clamp<std::int16_t>(*meter, 0, 4000);

    auto* const gauge = reinterpret_cast<double*>(player + 32u);
    if (*gauge < -1000.0)
    {
        *gauge = -1000.0;
    }
    else if (*gauge > 1000.0)
    {
        *gauge = 1000.0;
    }
}

void __cdecl HookedBattleSceneComposite(int p1, int p2, int cameraX, int cameraY)
{
    if (g_suppressed.load(std::memory_order_acquire))
    {
        SanitizeCompositePlayerState(p1);
        SanitizeCompositePlayerState(p2);
        return;
    }
    if (g_originalBattleSceneComposite != nullptr)
    {
        g_originalBattleSceneComposite(p1, p2, cameraX, cameraY);
    }
}

int __cdecl HookedSurfaceBlit(
    int r1a, int r1b, int r1c, int r1d, void* surface,
    int r2a, int r2b, int r2c, int r2d, int flag, int mode)
{
    if (g_suppressed.load(std::memory_order_acquire))
    {
        // Keep all game/effect updates; skip only the DirectDraw Lock/blit.
        return 0;
    }
    if (surface == nullptr)
    {
        const unsigned n = g_nullBlitSkips.fetch_add(1) + 1u;
        if (n <= 4u || (n % 512u) == 0u)
        {
            log::Warn("hooks",
                "null-surface blit skipped (surface never created — loader CreateSurface failed?) n=%u",
                n);
        }
        return 0;
    }
    return g_originalSurfaceBlit != nullptr
        ? g_originalSurfaceBlit(r1a, r1b, r1c, r1d, surface, r2a, r2b, r2c, r2d, flag, mode)
        : 0;
}

// Never return a smaller buffer: the caller keeps its count and would overflow the heap.
using OperatorNewFn = void*(__cdecl*)(unsigned int size);
OperatorNewFn g_originalOperatorNew = nullptr;
void* g_operatorNewTarget = nullptr;
std::atomic<unsigned> g_hugeAllocGuards{0};
constexpr unsigned int kHugeAllocThreshold = 0x10000000u;
std::atomic<int> g_launcherOnlineMode{-1}; // -1 unknown, 0 no/offline, 1 online

bool IsLauncherManagedOnlineSession()
{
    if (caster::IsNetplayActive() || caster::IsSpectatorActive())
    {
        return true;
    }
    const int cached = g_launcherOnlineMode.load(std::memory_order_acquire);
    if (cached >= 0)
    {
        return cached != 0;
    }

    // Read the descriptor directly: hooks enable before the session reaches the bridge.
    const DWORD required = GetEnvironmentVariableW(
        L"TRIAL_CASTER_SESSION_INI", nullptr, 0u);
    bool online = false;
    if (required > 1u && required < 32768u)
    {
        std::vector<wchar_t> path(required, L'\0');
        const DWORD length = GetEnvironmentVariableW(
            L"TRIAL_CASTER_SESSION_INI",
            path.data(),
            static_cast<DWORD>(path.size()));
        if (length > 0u && length < path.size())
        {
            wchar_t mode[16] = {};
            const DWORD modeLength = GetPrivateProfileStringW(
                L"session", L"mode", L"", mode, static_cast<DWORD>(std::size(mode)), path.data());
            online = modeLength > 0u
                && (lstrcmpiW(mode, L"host") == 0
                    || lstrcmpiW(mode, L"client") == 0
                    || lstrcmpiW(mode, L"spectator") == 0);
        }
    }
    g_launcherOnlineMode.store(online ? 1 : 0, std::memory_order_release);
    return online;
}

void* __cdecl HookedOperatorNew(unsigned int size)
{
    if (size >= kHugeAllocThreshold)
    {
        const unsigned n = g_hugeAllocGuards.fetch_add(1) + 1u;
        log::Critical("hooks",
            "operator new(0x%08X / %u) is impossible (>=256MB); preserving native allocation semantics (n=%u)",
            size, size, n);
        crash::WriteManualDump("operator new bogus size (battle-entry asset crash)", size);
        if (IsLauncherManagedOnlineSession())
        {
            lifecycle::Fault(
                "hooks", "impossible allocation size=%u; launcher-managed online session closing", size);
            log::FlushFile();
            ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
        }
        return g_originalOperatorNew != nullptr ? g_originalOperatorNew(size) : nullptr;
    }
    return g_originalOperatorNew != nullptr ? g_originalOperatorNew(size) : nullptr;
}

std::atomic<unsigned> g_shareUpgrades{0};
std::atomic<unsigned> g_shareRetries{0};

HANDLE WINAPI HookedCreateFileA(
    LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sec, DWORD disp, DWORD flags,
    HANDLE tmpl)
{
    // Local copy keeps every path stable while a caller is parked in the retry Sleep.
    const CreateFileAFn orig = reinterpret_cast<CreateFileAFn>(g_originalCreateFileARaw);
    if (orig == nullptr)
    {
        SetLastError(ERROR_INVALID_FUNCTION);
        return INVALID_HANDLE_VALUE;
    }

    const bool readOnly = (access & GENERIC_WRITE) == 0 && (access & GENERIC_READ) != 0;

    if (readOnly && share == 0)
    {
        share = FILE_SHARE_READ;
        g_shareUpgrades.fetch_add(1, std::memory_order_relaxed);
    }

    // OPEN_ALWAYS would create a 0-byte file for a missing asset: use the dummy if available.
    if (readOnly && disp == OPEN_ALWAYS && name != nullptr
        && GetFileAttributesA(name) == INVALID_FILE_ATTRIBUTES)
    {
        const DWORD attrError = GetLastError();
        if (attrError == ERROR_FILE_NOT_FOUND || attrError == ERROR_PATH_NOT_FOUND)
        {
            const HANDLE d = OpenDummyAssetFile();
            if (d != INVALID_HANDLE_VALUE)
            {
                const unsigned n = g_missingFileRedirects.fetch_add(1) + 1u;
                if (n <= 16u || (n % 256u) == 0u)
                {
                    log::Warn("hooks",
                        "missing asset '%s' -> blank dummy (OPEN_ALWAYS would litter a 0-byte file; n=%u)",
                        name, n);
                }
                SetLastError(ERROR_SUCCESS);
                return d;
            }
        }
    }

    HANDLE h = orig(name, access, share, sec, disp, flags, tmpl);

    // Capture the error before logging: log::Warn's file I/O clobbers GetLastError.
    if (h == INVALID_HANDLE_VALUE && readOnly && GetLastError() == ERROR_SHARING_VIOLATION)
    {
        for (int attempt = 0; attempt < 10 && h == INVALID_HANDLE_VALUE; ++attempt)
        {
            Sleep(5);
            h = orig(name, access, share, sec, disp, flags, tmpl);
        }
        const DWORD afterRetry = GetLastError();
        const unsigned n = g_shareRetries.fetch_add(1) + 1u;
        if (n <= 8u)
        {
            log::Warn("hooks", "sharing violation on '%s' — retried, %s",
                name != nullptr ? name : "(null)",
                h != INVALID_HANDLE_VALUE ? "recovered" : "STILL FAILING");
        }
        SetLastError(afterRetry);

        if (h == INVALID_HANDLE_VALUE && afterRetry == ERROR_SHARING_VIOLATION)
        {
            log::Critical(
                "hooks",
                "existing asset remained sharing-locked after retry '%s'; zero substitution forbidden",
                name != nullptr ? name : "(null)");
            if (IsLauncherManagedOnlineSession())
            {
                lifecycle::Fault(
                    "hooks",
                    "existing asset sharing violation after retry path=%s; closing to prevent divergent content",
                    name != nullptr ? name : "(null)");
                log::FlushFile();
                ExitProcess(ERROR_SHARING_VIOLATION);
            }
            SetLastError(afterRetry);
            return INVALID_HANDLE_VALUE;
        }
    }

    // OPEN_EXISTING carries gameplay .dat content: never substitute zeros.
    if (h == INVALID_HANDLE_VALUE && name != nullptr && readOnly && disp == OPEN_EXISTING)
    {
        const DWORD err = GetLastError();
        log::Critical(
            "hooks",
            "OPEN_EXISTING gameplay asset failed '%s' error=%lu; zero substitution forbidden",
            name,
            static_cast<unsigned long>(err));
        if (IsLauncherManagedOnlineSession())
        {
            lifecycle::Fault(
                "hooks",
                "gameplay asset unavailable path=%s error=%lu; closing to prevent divergent content",
                name,
                static_cast<unsigned long>(err));
            log::FlushFile();
            ExitProcess(err != ERROR_SUCCESS ? err : ERROR_FILE_NOT_FOUND);
        }
        SetLastError(err);
        return h;
    }

    if (h == INVALID_HANDLE_VALUE && name != nullptr && readOnly
        && disp == OPEN_ALWAYS)
    {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        {
            const HANDLE d = OpenDummyAssetFile();
            if (d != INVALID_HANDLE_VALUE)
            {
                const unsigned n = g_missingFileRedirects.fetch_add(1) + 1u;
                if (n <= 16u || (n % 256u) == 0u)
                {
                    log::Warn("hooks",
                        "missing OPEN_ALWAYS visual asset '%s' (err=%lu) -> blank dummy (n=%u)",
                        name, static_cast<unsigned long>(err), n);
                }
                SetLastError(ERROR_SUCCESS);
                return d;
            }
            SetLastError(err);
        }
    }
    return h;
}

// __thiscall (effect blob, slot), modelled as __fastcall with a dummy EDX slot.
using EffectAnimUpdateFn = std::int16_t(__fastcall*)(void* thisPtr, void* edx, std::uint8_t slot);
EffectAnimUpdateFn g_originalEffectAnimUpdate = nullptr;
void* g_effectAnimTarget = nullptr;

std::int16_t __fastcall HookedEffectAnimUpdate(void* thisPtr, void* edx, std::uint8_t slot)
{
    if (g_originalEffectAnimUpdate == nullptr)
    {
        return 0;
    }
    det::rng::PushVisualRand();
    const std::int16_t result = g_originalEffectAnimUpdate(thisPtr, edx, slot);
    det::rng::PopVisualRand();
    return result;
}

}

bool InitializeRenderGate()
{
    if (g_initialized.exchange(true))
    {
        return true;
    }

    void* const target = trial::ResolveVa(trial::kVaPresent);
    if (target == nullptr)
    {
        lifecycle::Fault("hooks", "present target unresolved");
        log::Error("hooks", "present (render gate) target unresolved");
        g_initialized.store(false);
        return false;
    }

    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedPresent),
            reinterpret_cast<void**>(&g_originalPresent)) != MH_OK
        || MH_QueueEnableHook(target) != MH_OK)
    {
        lifecycle::Fault("hooks", "present hook install failed");
        log::Error("hooks", "present (render gate) hook install failed");
        g_initialized.store(false);
        return false;
    }

    g_presentTarget = target;

    void* const soundTarget = trial::ResolveVa(trial::kVaPlaySound);
    if (soundTarget != nullptr
        && MH_CreateHook(soundTarget, reinterpret_cast<void*>(&HookedPlaySound),
               reinterpret_cast<void**>(&g_originalPlaySound)) == MH_OK
        && MH_QueueEnableHook(soundTarget) == MH_OK)
    {
        g_playSoundTarget = soundTarget;
    }
    else
    {
        log::Warn("hooks", "render gate — SFX hook not installed (re-sim sounds will repeat)");
    }

    void* const effectTarget = trial::ResolveVa(trial::kVaEffectAnimUpdate);
    if (effectTarget != nullptr
        && MH_CreateHook(effectTarget, reinterpret_cast<void*>(&HookedEffectAnimUpdate),
               reinterpret_cast<void**>(&g_originalEffectAnimUpdate)) == MH_OK
        && MH_QueueEnableHook(effectTarget) == MH_OK)
    {
        g_effectAnimTarget = effectTarget;
        log::Info("hooks", "effect anim update hooked EFZ+0x%05X (visual rand stream)",
            static_cast<unsigned>(trial::VaToRva(trial::kVaEffectAnimUpdate)));
    }
    else
    {
        log::Warn("hooks",
            "effect anim hook not installed — draw-path rand() drains the SIM stream (re-sim desync risk)");
    }

    void* const seekTarget = trial::ResolveVa(trial::kVaSoundBufferSeek);
    if (seekTarget != nullptr
        && MH_CreateHook(seekTarget, reinterpret_cast<void*>(&HookedSoundSeek),
               reinterpret_cast<void**>(&g_originalSoundSeek)) == MH_OK
        && MH_QueueEnableHook(seekTarget) == MH_OK)
    {
        g_soundSeekTarget = seekTarget;
    }
    else
    {
        log::Warn("hooks", "render gate — sound seek guard not installed (freed-buffer AV unguarded)");
    }

    void* const assetTarget = trial::ResolveVa(trial::kVaAssetFileLoad);
    if (assetTarget != nullptr
        && MH_CreateHook(assetTarget, reinterpret_cast<void*>(&HookedAssetFileLoad),
               reinterpret_cast<void**>(&g_originalAssetFileLoad)) == MH_OK
        && MH_QueueEnableHook(assetTarget) == MH_OK)
    {
        g_assetFileLoadTarget = assetTarget;
        log::Info("hooks", "asset file-load guard installed EFZ+0x%05X (missing-file crash guard)",
            static_cast<unsigned>(trial::VaToRva(trial::kVaAssetFileLoad)));
    }
    else
    {
        log::Warn("hooks",
            "asset file-load guard not installed (missing-palette operator new(0xFFFFFFFF) unguarded)");
    }

    // Patch EFZ's IAT only: Wine exports may be thunks, and SDL must not get the dummy.
    g_createFileAHostModule = GetModuleHandleA(nullptr);
    if (g_createFileAHostModule != nullptr
        && win32::PatchImportAddressSlot(
            g_createFileAHostModule,
            "KERNEL32.dll",
            "CreateFileA",
            reinterpret_cast<void*>(&HookedCreateFileA),
            &g_originalCreateFileARaw))
    {
        g_createFileAIatHooked.store(true, std::memory_order_release);
        log::Info(
            "hooks",
            "EFZ CreateFileA IAT guard installed (game-only blank-dummy redirect; Wine-safe)");
    }
    else
    {
        g_createFileAHostModule = nullptr;
        log::Warn("hooks", "EFZ CreateFileA IAT guard not installed");
    }

    void* const blitTarget = trial::ResolveVa(trial::kVaSurfaceBlit);
    if (blitTarget != nullptr
        && MH_CreateHook(blitTarget, reinterpret_cast<void*>(&HookedSurfaceBlit),
               reinterpret_cast<void**>(&g_originalSurfaceBlit)) == MH_OK
        && MH_QueueEnableHook(blitTarget) == MH_OK)
    {
        g_surfaceBlitTarget = blitTarget;
        log::Info("hooks", "null-surface blit guard installed EFZ+0x%05X",
            static_cast<unsigned>(trial::VaToRva(trial::kVaSurfaceBlit)));
    }
    else
    {
        log::Warn("hooks", "null-surface blit guard NOT installed (0x450466 AV unguarded)");
    }

    void* const compositeTarget = trial::ResolveVa(trial::kVaBattleSceneComposite);
    if (compositeTarget != nullptr
        && MH_CreateHook(compositeTarget, reinterpret_cast<void*>(&HookedBattleSceneComposite),
               reinterpret_cast<void**>(&g_originalBattleSceneComposite)) == MH_OK
        && MH_QueueEnableHook(compositeTarget) == MH_OK)
    {
        g_battleSceneCompositeTarget = compositeTarget;
        log::Info(
            "hooks",
            "hidden battle composite gate installed EFZ+0x%05X",
            static_cast<unsigned>(trial::VaToRva(trial::kVaBattleSceneComposite)));
    }
    else
    {
        log::Warn("hooks", "hidden battle composite gate NOT installed");
    }

    void* const newTarget = trial::ResolveVa(trial::kVaOperatorNew);
    if (newTarget != nullptr
        && MH_CreateHook(newTarget, reinterpret_cast<void*>(&HookedOperatorNew),
               reinterpret_cast<void**>(&g_originalOperatorNew)) == MH_OK
        && MH_QueueEnableHook(newTarget) == MH_OK)
    {
        g_operatorNewTarget = newTarget;
        log::Info("hooks", "operator new guard installed EFZ+0x%05X (huge-alloc diagnostic/fail-closed)",
            static_cast<unsigned>(trial::VaToRva(trial::kVaOperatorNew)));
    }
    else
    {
        log::Warn("hooks", "operator new guard NOT installed (huge-alloc crash unguarded)");
    }

    const bool rollbackCriticalHooksReady =
        g_playSoundTarget != nullptr
        && g_effectAnimTarget != nullptr
        && g_battleSceneCompositeTarget != nullptr;
    if (!rollbackCriticalHooksReady)
    {
        lifecycle::Fault(
            "hooks",
            "rollback presentation contract incomplete sound=%d effectRng=%d composite=%d",
            g_playSoundTarget != nullptr ? 1 : 0,
            g_effectAnimTarget != nullptr ? 1 : 0,
            g_battleSceneCompositeTarget != nullptr ? 1 : 0);
        log::Error(
            "hooks",
            "rollback presentation contract incomplete sound=%d effectRng=%d composite=%d",
            g_playSoundTarget != nullptr ? 1 : 0,
            g_effectAnimTarget != nullptr ? 1 : 0,
            g_battleSceneCompositeTarget != nullptr ? 1 : 0);
        return false;
    }

    log::Info("hooks", "render gate active — present EFZ+0x%05X sound EFZ+0x%05X",
        static_cast<unsigned>(trial::VaToRva(trial::kVaPresent)),
        static_cast<unsigned>(trial::VaToRva(trial::kVaPlaySound)));
    return true;
}

void ShutdownRenderGate()
{
    if (!g_initialized.exchange(false))
    {
        return;
    }
    g_suppressed.store(false);
    g_audioFrameActive.store(false, std::memory_order_release);
    ResetRollbackAudioDedup();
    if (g_battleSceneCompositeTarget != nullptr)
    {
        MH_DisableHook(g_battleSceneCompositeTarget);
        MH_RemoveHook(g_battleSceneCompositeTarget);
        g_battleSceneCompositeTarget = nullptr;
        g_originalBattleSceneComposite = nullptr;
    }
    if (g_surfaceBlitTarget != nullptr)
    {
        MH_DisableHook(g_surfaceBlitTarget);
        MH_RemoveHook(g_surfaceBlitTarget);
        g_surfaceBlitTarget = nullptr;
        g_originalSurfaceBlit = nullptr;
    }
    if (g_operatorNewTarget != nullptr)
    {
        MH_DisableHook(g_operatorNewTarget);
        MH_RemoveHook(g_operatorNewTarget);
        g_operatorNewTarget = nullptr;
        g_originalOperatorNew = nullptr;
    }
    if (g_createFileAIatHooked.exchange(false, std::memory_order_acq_rel))
    {
        (void)win32::RestoreImportAddressSlot(
            g_createFileAHostModule,
            "KERNEL32.dll",
            "CreateFileA",
            reinterpret_cast<void*>(&HookedCreateFileA),
            g_originalCreateFileARaw);
    }
    // Keep the forwarding record after exposure: a wrapper or call may still enter.
    const HANDLE dummyOwner = g_dummyAssetOwner.exchange(
        INVALID_HANDLE_VALUE, std::memory_order_acq_rel);
    if (dummyOwner != INVALID_HANDLE_VALUE)
    {
        CloseHandle(dummyOwner);
    }
    if (g_dummyAssetPath[0] != L'\0')
    {
        DeleteFileW(g_dummyAssetPath);
        g_dummyAssetPath[0] = L'\0';
        g_dummyState.store(0, std::memory_order_release);
    }
    if (g_assetFileLoadTarget != nullptr)
    {
        MH_DisableHook(g_assetFileLoadTarget);
        MH_RemoveHook(g_assetFileLoadTarget);
        g_assetFileLoadTarget = nullptr;
        g_originalAssetFileLoad = nullptr;
    }
    if (g_effectAnimTarget != nullptr)
    {
        MH_DisableHook(g_effectAnimTarget);
        MH_RemoveHook(g_effectAnimTarget);
        g_effectAnimTarget = nullptr;
        g_originalEffectAnimUpdate = nullptr;
    }
    if (g_soundSeekTarget != nullptr)
    {
        MH_DisableHook(g_soundSeekTarget);
        MH_RemoveHook(g_soundSeekTarget);
        g_soundSeekTarget = nullptr;
        g_originalSoundSeek = nullptr;
    }
    if (g_playSoundTarget != nullptr)
    {
        MH_DisableHook(g_playSoundTarget);
        MH_RemoveHook(g_playSoundTarget);
        g_playSoundTarget = nullptr;
        g_originalPlaySound = nullptr;
    }
    if (g_presentTarget != nullptr)
    {
        MH_DisableHook(g_presentTarget);
        MH_RemoveHook(g_presentTarget);
        g_presentTarget = nullptr;
        g_originalPresent = nullptr;
    }
}

void SetPresentSuppressed(const bool suppressed)
{
    g_suppressed.store(suppressed, std::memory_order_release);
}

bool IsPresentSuppressed()
{
    return g_suppressed.load(std::memory_order_acquire);
}

std::uint64_t GetPresentedFrameCount()
{
    return g_presentedFrames.load(std::memory_order_relaxed);
}

void SetRollbackAudioFrame(const std::uint32_t frame, const bool active)
{
    g_audioFrame.store(frame, std::memory_order_release);
    g_audioFrameActive.store(active, std::memory_order_release);
}

void ResetRollbackAudioDedup()
{
    std::lock_guard<std::mutex> lock(g_audioDedupMutex);
    g_audioDedupKeys.fill(0u);
    g_audioDedupWrite = 0;
    g_audioDedupCount = 0;
}

}
