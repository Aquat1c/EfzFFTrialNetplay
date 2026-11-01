#include "savestate.h"
#include "caster_input.h"
#include "control_config.h"
#include "collision_display.h"
#include "control_swap.h"
#include "debug_log.h"
#include "lifecycle_log.h"
#include "mem_write_log.h"
#include "determinism.h"
#include "rng.h"
#include "rollback_memory.h"
#include "savestate_digest.h"
#include "savestate_layout.h"
#include "state_paths.h"
#include "sdl_input.h"
#include "trial_addresses.h"

#include <windows.h>

#include <mmsystem.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace thv
{
namespace
{

constexpr int kDebugSlotMarker = -1;
constexpr std::uint32_t kFileMagic = 0x53534554u; // "TESS" little-endian
constexpr std::uint32_t kFileVersion = savestate_layout::kFileVersionV4;
constexpr std::uint32_t kFileVersionV3 = savestate_layout::kFileVersionV3;
constexpr std::uint32_t kFileVersionV2 = 2u;
constexpr std::uint32_t kFileVersionV1 = 1u;

#pragma pack(push, 1)
struct SavestateHeaderV1
{
    std::uint32_t magic = kFileMagic;
    std::uint32_t version = kFileVersionV1;
    std::uint32_t stateSize = 0;
    std::uint8_t p1CharacterId = 0;
    std::uint8_t p2CharacterId = 0;
    std::uint8_t reserved[2] = {};
    std::int32_t humanSide = 0;
    std::uint32_t slot = 0;
};

struct SavestateHeaderV2 : SavestateHeaderV1
{
    std::uint32_t rngShadowState = 0;
    std::uint32_t fpuStateSize = 0;
};
#pragma pack(pop)

using SavestateHeader = SavestateHeaderV2;

struct PendingRestore
{
    enum class Format
    {
        V2Battle,
        V3Snapshot,
    };

    Format format = Format::V2Battle;
    std::vector<std::uint8_t> battle;
    savestate_layout::SnapshotPayload snapshot;
    std::array<std::uint8_t, det::kFpuStateSize> fpu{};
    std::uint32_t rng = 0;
    bool hasDeterminism = false;
    bool bothHuman = false;
    int humanSide = 0;
    int slot = -1;
};

std::mutex g_mutex;
std::atomic<bool> g_initialized{false};
std::atomic<int> g_activeSlot{0};
std::atomic<unsigned int> g_saveCount{0};
std::atomic<unsigned int> g_loadCount{0};

bool IsBattleScreenActive()
{
    const auto* const mode = static_cast<const std::uint8_t*>(trial::ResolveVa(trial::kVaBattleScreenMode));
    return mode != nullptr && *mode == 1u;
}

std::uint8_t ReadCharacterId(int side)
{
    const std::uintptr_t va = (side == 0) ? trial::kVaP1Player : trial::kVaP2Player;
    const auto* const player = static_cast<const std::uint8_t*>(trial::ResolveVa(va));
    if (player == nullptr)
    {
        return 0;
    }
    return player[0];
}

bool CaptureBattleState(std::vector<std::uint8_t>* out)
{
    if (out == nullptr)
    {
        return false;
    }

    void* const src = trial::ResolveVa(trial::kVaBattleStateBegin);
    if (src == nullptr)
    {
        return false;
    }

    out->resize(trial::kBattleStateSize);
    std::memcpy(out->data(), src, trial::kBattleStateSize);
    return true;
}

bool ApplyBattleState(const std::uint8_t* data, std::size_t size)
{
    if (data == nullptr)
    {
        return false;
    }

    return rollback::memory::RestoreBattleSelective(data, size, "practice_apply");
}

void SanitizeBattleStateAfterRestore(const savestate_digest::SavestateDigest* blobDigest)
{
    auto* const screenMode = static_cast<std::uint8_t*>(trial::ResolveVa(trial::kVaBattleScreenMode));
    auto* const screenSub = static_cast<std::uint32_t*>(trial::ResolveVa(trial::kVaScreenSubState));
    auto* const screenAnim = static_cast<std::int32_t*>(trial::ResolveVa(trial::kVaScreenAnimCounter));
    auto* const pacingLastTick = static_cast<DWORD*>(trial::ResolveVa(trial::kVaFramePacingLastTick));
    auto* const pacingToggle = static_cast<std::int32_t*>(trial::ResolveVa(trial::kVaFramePacingToggle));
    auto* const pacingBusy = static_cast<std::int32_t*>(trial::ResolveVa(trial::kVaFramePacingBusyFlag));
    auto* const frameSubsteps = static_cast<std::int32_t*>(trial::ResolveVa(trial::kVaFrameSubsteps));

    const std::uint32_t prevSub = screenSub != nullptr ? *screenSub : 0u;
    const std::int32_t prevToggle = pacingToggle != nullptr ? *pacingToggle : 0;

    if (screenMode != nullptr)
    {
        const std::uint8_t before = *screenMode;
        if (before != trial::kScreenBattle)
        {
            memlog::LogScalarWrite(
                "savestate_sanitize",
                trial::kVaBattleScreenMode,
                "screen_mode",
                before,
                trial::kScreenBattle,
                1);
        }
        *screenMode = trial::kScreenBattle;
    }

    // The battle tick treats CCDBC0>=2 as round-end or loading: never resume into those.
    if (screenSub != nullptr && *screenSub >= 2u)
    {
        const std::uint32_t before = *screenSub;
        memlog::LogScalarWrite("savestate_sanitize", trial::kVaScreenSubState, "screen_substate", before, 0u, 4);
        *screenSub = 0u;
    }
    if (screenAnim != nullptr)
    {
        const std::int32_t before = *screenAnim;
        if (before != 0)
        {
            memlog::LogScalarWrite(
                "savestate_sanitize",
                trial::kVaScreenAnimCounter,
                "screen_anim",
                static_cast<std::uint64_t>(before),
                0u,
                4);
        }
        *screenAnim = 0;
    }

    // Restored pacing timestamps can trap the battle tick in a busy-wait spin at entry.
    if (pacingLastTick != nullptr)
    {
        const DWORD before = *pacingLastTick;
        const DWORD after = timeGetTime();
        if (before != after)
        {
            memlog::LogScalarWrite("savestate_sanitize", trial::kVaFramePacingLastTick, "frame_pacing_tick", before, after, 4);
        }
        *pacingLastTick = after;
    }
    if (pacingToggle != nullptr)
    {
        const std::int32_t before = *pacingToggle;
        if (before != 0)
        {
            memlog::LogScalarWrite(
                "savestate_sanitize",
                trial::kVaFramePacingToggle,
                "frame_pacing_battle",
                static_cast<std::uint64_t>(before),
                0u,
                4);
        }
        *pacingToggle = 0;
    }
    if (pacingBusy != nullptr)
    {
        const std::int32_t before = *pacingBusy;
        if (before != 0)
        {
            memlog::LogScalarWrite(
                "savestate_sanitize",
                trial::kVaFramePacingBusyFlag,
                "frame_pacing_busy",
                static_cast<std::uint64_t>(before),
                0u,
                4);
        }
        *pacingBusy = 0;
    }

    // Outside the battle blob but required by the inner sim loop (defaults to 3).
    if (frameSubsteps != nullptr && *frameSubsteps <= 0)
    {
        const std::int32_t before = *frameSubsteps;
        memlog::LogScalarWrite(
            "savestate_sanitize",
            trial::kVaFrameSubsteps,
            "frame_substeps",
            static_cast<std::uint64_t>(before),
            3u,
            4);
        *frameSubsteps = 3;
    }

    SetDebugDrawFlag(GetLayerMask() != 0);

    log::Info(
        "savestate",
        "sanitize after restore screenSub %u->%u pacingToggle %d->0 frameSubsteps=%d",
        prevSub,
        screenSub != nullptr ? *screenSub : 0u,
        prevToggle,
        frameSubsteps != nullptr ? *frameSubsteps : -1);

    if (blobDigest != nullptr && blobDigest->screenSub >= 2u)
    {
        log::Warn(
            "savestate",
            "blob had transition subState=%u (round-end/loading) — clamped to active battle",
            blobDigest->screenSub);
    }
}

void ClearTransientBattleScratch()
{
    auto* const queue = static_cast<std::uint8_t*>(trial::ResolveVa(trial::kVaScreenEventQueue));
    if (queue != nullptr)
    {
        memlog::LogMemsetWrite("savestate", "screen_event_queue", queue, 0xFF, trial::kScreenEventQueueSize);
        std::memset(queue, 0xFF, trial::kScreenEventQueueSize);
    }
}

bool ApplyPendingRestore(PendingRestore pending, const bool withDigest)
{
    if (pending.format == PendingRestore::Format::V3Snapshot)
    {
        if (pending.snapshot.bytes.empty())
        {
            log::Error("savestate", "restore aborted: v3 snapshot empty");
            return false;
        }

        if (withDigest)
        {
            const savestate_digest::SavestateDigest liveBefore = savestate_digest::CaptureLiveDigest();
            savestate_digest::LogDigestSummary("restore_before", liveBefore);
        }

        det::RepinFpu();
        if (pending.hasDeterminism)
        {
            // Manual savestates: FPU captured for diagnostics only — restore RNG only.
            det::rng::Restore(pending.rng);
        }

        memlog::LogGameHeadline("before_pending_restore");
        if (!savestate_layout::ApplySnapshot(pending.snapshot))
        {
            lifecycle::Rollback("savestate", "restore failed snapshot apply rejected");
            log::Error("savestate", "restore failed: snapshot apply rejected");
            return false;
        }

        ClearTransientBattleScratch();
        SanitizeBattleStateAfterRestore(nullptr);
        memlog::LogGameHeadline("after_pending_restore");

        if (pending.bothHuman)
        {
            RestorePracticeBothHuman(pending.humanSide == 1 ? 1 : 0);
            if (withDigest)
            {
                log::Info("savestate", "restored practice VS Human routing (human=P%d)", pending.humanSide + 1);
            }
        }
        else if (pending.humanSide == 0 || pending.humanSide == 1)
        {
            SetHumanSide(pending.humanSide);
        }

        if (withDigest)
        {
            const savestate_digest::SavestateDigest liveAfter = savestate_digest::CaptureLiveDigest();
            savestate_digest::LogDigestSummary("restore_after", liveAfter);

            char extra[320] = {};
            std::snprintf(
                extra,
                sizeof(extra),
                "slot=%d payload=%u file_crc=0x%08X rng=%d bothHuman=%d side=%d",
                pending.slot,
                pending.snapshot.meta.payloadSize,
                pending.snapshot.meta.payloadCrc32,
                pending.hasDeterminism ? 1 : 0,
                pending.bothHuman ? 1 : 0,
                pending.humanSide);
            savestate_digest::WriteDigestArtifact("restore_after", liveAfter, extra);
            log::FlushFile();
        }

        g_loadCount.fetch_add(1);
        log::Step(
            "savestate",
            "restored slot %d (payload=%u%s)",
            pending.slot,
            pending.snapshot.meta.payloadSize,
            pending.hasDeterminism ? ", rng" : "");
        return true;
    }

    if (pending.battle.empty())
    {
        log::Error("savestate", "restore aborted: legacy battle blob empty");
        return false;
    }

    const savestate_digest::SavestateDigest blobDigest =
        withDigest ? savestate_digest::CaptureBlobDigest(pending.battle.data(), pending.battle.size())
              : savestate_digest::SavestateDigest{};

    if (withDigest)
    {
        const savestate_digest::SavestateDigest liveBefore = savestate_digest::CaptureLiveDigest();
        savestate_digest::LogDigestSummary("restore_before", liveBefore);
        savestate_digest::LogDigestSummary("restore_blob", blobDigest);
        savestate_digest::LogDigestDiff("restore_plan", liveBefore, blobDigest);
    }

    det::RepinFpu();
    if (pending.hasDeterminism)
    {
        det::rng::Restore(pending.rng);
    }

    memlog::LogGameHeadline("before_legacy_restore");
    if (!ApplyBattleState(pending.battle.data(), pending.battle.size()))
    {
        lifecycle::Rollback("savestate", "restore failed memory write rejected");
        log::Error("savestate", "restore failed: memory write rejected");
        return false;
    }

    ClearTransientBattleScratch();
    SanitizeBattleStateAfterRestore(withDigest ? &blobDigest : nullptr);
    memlog::LogGameHeadline("after_legacy_restore");

    if (pending.bothHuman)
    {
        RestorePracticeBothHuman(pending.humanSide == 1 ? 1 : 0);
        if (withDigest)
        {
            log::Info("savestate", "restored practice VS Human routing (human=P%d)", pending.humanSide + 1);
        }
    }
    else if (pending.humanSide == 0 || pending.humanSide == 1)
    {
        SetHumanSide(pending.humanSide);
    }

    if (withDigest)
    {
        const savestate_digest::SavestateDigest liveAfter = savestate_digest::CaptureLiveDigest();
        savestate_digest::LogDigestSummary("restore_after", liveAfter);
        savestate_digest::LogDigestDiff("restore_result", blobDigest, liveAfter);

        char extra[256] = {};
        std::snprintf(
            extra,
            sizeof(extra),
            "slot=%d bytes=%u rng=%d bothHuman=%d side=%d",
            pending.slot,
            static_cast<unsigned>(pending.battle.size()),
            pending.hasDeterminism ? 1 : 0,
            pending.bothHuman ? 1 : 0,
            pending.humanSide);
        savestate_digest::WriteDigestArtifact("restore_after", liveAfter, extra);

        if (liveAfter.blobCrc32 != blobDigest.blobCrc32)
        {
            log::Error(
                "savestate",
                "restore CRC mismatch blob=0x%08X live=0x%08X (partial write or readonly pages?)",
                blobDigest.blobCrc32,
                liveAfter.blobCrc32);
        }

        log::FlushFile();
    }

    g_loadCount.fetch_add(1);
    log::Step(
        "savestate",
        "restored legacy v2 slot %d (%u bytes%s)",
        pending.slot,
        static_cast<unsigned>(pending.battle.size()),
        pending.hasDeterminism ? ", rng" : "");
    return true;
}

std::filesystem::path GetGameDirectory()
{
    return paths::GameDirectory();
}

std::filesystem::path GetSavestateRoot()
{
    return paths::MutableStateRoot() / L"trial_savestates";
}

const char* BankName(const SavestateBank bank)
{
    switch (bank)
    {
    case SavestateBank::Practice:
        return "practice";
    case SavestateBank::Netplay:
        return "netplay";
    case SavestateBank::Debug:
        return "debug";
    default:
        return "?";
    }
}

bool SlotInRange(const SavestateBank bank, const int slot)
{
    switch (bank)
    {
    case SavestateBank::Practice:
        return slot >= kPracticeMinSlot && slot <= kPracticeMaxSlot;
    case SavestateBank::Netplay:
        return slot >= kNetplayMinSlot && slot <= kNetplayMaxSlot;
    case SavestateBank::Debug:
        return slot == kDebugSlotMarker;
    default:
        return false;
    }
}

std::filesystem::path GetStatePath(const SavestateBank bank, const int slot)
{
    const std::filesystem::path root = GetSavestateRoot();
    switch (bank)
    {
    case SavestateBank::Practice:
    {
        char fileName[64] = {};
        std::snprintf(fileName, sizeof(fileName), "practice\\slot_%d.thvs", slot);
        return root / fileName;
    }
    case SavestateBank::Netplay:
    {
        char fileName[64] = {};
        std::snprintf(fileName, sizeof(fileName), "netplay\\slot_%d.thvs", slot);
        return root / fileName;
    }
    case SavestateBank::Debug:
        return root / L"debug.thvs";
    default:
        return {};
    }
}

std::filesystem::path ResolveLoadPath(const SavestateBank bank, const int slot)
{
    const std::filesystem::path primary = GetStatePath(bank, slot);
    const auto fileExists = [](const std::filesystem::path& path)
    {
        const DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
    };
    if (fileExists(primary))
    {
        return primary;
    }

    const std::filesystem::path oldRoot = GetGameDirectory() / L"trial_savestates";
    std::filesystem::path oldBankPath;
    switch (bank)
    {
    case SavestateBank::Practice:
        oldBankPath = oldRoot / L"practice" / (L"slot_" + std::to_wstring(slot) + L".thvs");
        break;
    case SavestateBank::Netplay:
        oldBankPath = oldRoot / L"netplay" / (L"slot_" + std::to_wstring(slot) + L".thvs");
        break;
    case SavestateBank::Debug:
        oldBankPath = oldRoot / L"debug.thvs";
        break;
    default:
        break;
    }
    if (!oldBankPath.empty() && fileExists(oldBankPath))
    {
        return oldBankPath;
    }

    if (bank != SavestateBank::Practice)
    {
        return primary;
    }
    char legacy[64] = {};
    std::snprintf(legacy, sizeof(legacy), "trial_savestates\\slot_%d.thvs", slot);
    const std::filesystem::path flatLegacy = GetGameDirectory() / legacy;
    return fileExists(flatLegacy) ? flatLegacy : primary;
}

bool EnsureSavestateDirectory()
{
    std::error_code error;
    std::filesystem::create_directories(GetSavestateRoot() / L"practice", error);
    if (error)
    {
        return false;
    }
    std::filesystem::create_directories(GetSavestateRoot() / L"netplay", error);
    return !error;
}

bool SaveSlotLocked(const SavestateBank bank, const int slot)
{
    if (!SlotInRange(bank, slot))
    {
        log::Warn("savestate", "save rejected: bank=%s slot=%d out of range", BankName(bank), slot);
        return false;
    }
    if (!IsBattleScreenActive())
    {
        log::Warn("savestate", "save rejected: not in battle");
        return false;
    }
    if (!EnsureSavestateDirectory())
    {
        log::Error(
            "savestate",
            "save rejected: could not create writable trial_savestates directory under %s",
            paths::PathUtf8(paths::MutableStateRoot()).c_str());
        return false;
    }

    savestate_layout::SnapshotPayload snapshot;
    if (!savestate_layout::CaptureSnapshot(&snapshot))
    {
        lifecycle::Rollback("savestate", "save rejected v3 snapshot capture failed");
        log::Error("savestate", "save rejected: v3 snapshot capture failed");
        return false;
    }

    std::array<std::uint8_t, det::kFpuStateSize> fpuState{};
    det::CaptureFpuState(fpuState.data());

    SavestateHeader header = {};
    header.magic = kFileMagic;
    header.version = kFileVersion;
    header.stateSize = snapshot.meta.payloadSize;
    header.p1CharacterId = ReadCharacterId(0);
    header.p2CharacterId = ReadCharacterId(1);
    header.humanSide = GetHumanSide();
    header.reserved[0] = BothPlayersUseHumanInput() ? 1u : 0u;
    header.slot = static_cast<std::uint32_t>(slot);
    header.rngShadowState = det::rng::Snapshot();
    header.fpuStateSize = static_cast<std::uint32_t>(det::kFpuStateSize);

    const std::filesystem::path path = GetStatePath(bank, slot);
    const std::string displayPath = path.u8string();
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr)
    {
        return false;
    }

    const bool ok = std::fwrite(&header, sizeof(header), 1, file) == 1
        && std::fwrite(&snapshot.meta, sizeof(snapshot.meta), 1, file) == 1
        && std::fwrite(snapshot.regions.data(), sizeof(savestate_layout::RegionEntry), snapshot.regions.size(), file)
            == snapshot.regions.size()
        && std::fwrite(fpuState.data(), 1, fpuState.size(), file) == fpuState.size()
        && std::fwrite(snapshot.bytes.data(), 1, snapshot.bytes.size(), file) == snapshot.bytes.size();
    std::fclose(file);
    if (ok)
    {
        g_saveCount.fetch_add(1);
        const savestate_digest::SavestateDigest digest = savestate_digest::CaptureLiveDigest();
        savestate_digest::LogDigestSummary("save_ok", digest);
        savestate_digest::LogDigestDetail("save_ok", digest);

        char extra[256] = {};
        std::snprintf(
            extra,
            sizeof(extra),
            "bank=%s slot=%d path=%s rng=0x%08X regions=%zu",
            BankName(bank),
            slot,
            displayPath.c_str(),
            header.rngShadowState,
            snapshot.regions.size());
        savestate_digest::WriteDigestArtifact("save", digest, extra);
        savestate_digest::WriteFullSavestateDump("save_fulldump", snapshot, fpuState.data(), fpuState.size(), extra);
        log::Info(
            "savestate",
            "saved %s slot %d (%s) v4 P1=%u P2=%u side=%d rng=0x%08X payload=%u crc=0x%08X",
            BankName(bank),
            slot,
            displayPath.c_str(),
            header.p1CharacterId,
            header.p2CharacterId,
            header.humanSide,
            header.rngShadowState,
            header.stateSize,
            snapshot.meta.payloadCrc32);
    }
    else
    {
        log::Error("savestate", "write failed for %s slot %d (%s)", BankName(bank), slot, displayPath.c_str());
    }
    return ok;
}

bool LoadSlotLocked(const SavestateBank bank, const int slot)
{
    if (!SlotInRange(bank, slot))
    {
        log::Warn("savestate", "load rejected: bank=%s slot=%d out of range", BankName(bank), slot);
        return false;
    }
    if (!IsBattleScreenActive())
    {
        log::Warn("savestate", "load rejected: not in battle");
        return false;
    }

    const std::filesystem::path path = ResolveLoadPath(bank, slot);
    const std::string displayPath = path.u8string();
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr)
    {
        log::Warn("savestate", "load rejected: missing %s file %s", BankName(bank), displayPath.c_str());
        return false;
    }

    SavestateHeaderV1 headerBase = {};
    if (std::fread(&headerBase, sizeof(headerBase), 1, file) != 1)
    {
        std::fclose(file);
        return false;
    }

    const bool isV4 = headerBase.version == savestate_layout::kFileVersionV4;
    const bool isV3 = headerBase.version == savestate_layout::kFileVersionV3;
    const bool isV2 = headerBase.version == kFileVersionV2;
    const bool isV1 = headerBase.version == kFileVersionV1;
    if (headerBase.magic != kFileMagic || (!isV1 && !isV2 && !isV3 && !isV4))
    {
        std::fclose(file);
        log::Warn(
            "savestate",
            "load rejected: bad header in %s (magic=0x%08X ver=%u)",
            displayPath.c_str(),
            headerBase.magic,
            headerBase.version);
        return false;
    }

    if (isV4 || isV3)
    {
        SavestateHeader header = {};
        std::memcpy(&header, &headerBase, sizeof(headerBase));
        if (std::fread(&header.rngShadowState, sizeof(header.rngShadowState), 1, file) != 1
            || std::fread(&header.fpuStateSize, sizeof(header.fpuStateSize), 1, file) != 1)
        {
            std::fclose(file);
            return false;
        }

        savestate_layout::SavestateHeaderV3Fields v3Meta = {};
        if (std::fread(&v3Meta, sizeof(v3Meta), 1, file) != 1)
        {
            std::fclose(file);
            return false;
        }

        if (v3Meta.regionCount == 0 || v3Meta.payloadSize == 0)
        {
            std::fclose(file);
            log::Warn("savestate", "load rejected: empty snapshot payload in %s", displayPath.c_str());
            return false;
        }

        std::vector<savestate_layout::RegionEntry> regions(v3Meta.regionCount);
        if (std::fread(regions.data(), sizeof(savestate_layout::RegionEntry), regions.size(), file) != regions.size())
        {
            std::fclose(file);
            return false;
        }

        std::array<std::uint8_t, det::kFpuStateSize> fpuState{};
        if (header.fpuStateSize != det::kFpuStateSize
            || std::fread(fpuState.data(), 1, fpuState.size(), file) != fpuState.size())
        {
            std::fclose(file);
            return false;
        }

        std::vector<std::uint8_t> payload(v3Meta.payloadSize);
        if (std::fread(payload.data(), 1, payload.size(), file) != payload.size())
        {
            std::fclose(file);
            return false;
        }
        std::fclose(file);

        const std::uint32_t payloadCrc = savestate_digest::Crc32(payload.data(), payload.size());
        if (payloadCrc != v3Meta.payloadCrc32)
        {
            log::Warn(
                "savestate",
                "load rejected: payload CRC mismatch file=0x%08X computed=0x%08X",
                v3Meta.payloadCrc32,
                payloadCrc);
            return false;
        }

        const std::uint8_t liveP1 = ReadCharacterId(0);
        const std::uint8_t liveP2 = ReadCharacterId(1);
        if (headerBase.p1CharacterId != liveP1 || headerBase.p2CharacterId != liveP2)
        {
            log::Warn(
                "savestate",
                "load rejected: character mismatch (file P1=%u P2=%u, live P1=%u P2=%u)",
                headerBase.p1CharacterId,
                headerBase.p2CharacterId,
                liveP1,
                liveP2);
            return false;
        }

        savestate_layout::SnapshotPayload snapshot = {};
        snapshot.meta = v3Meta;
        snapshot.regions = std::move(regions);
        snapshot.bytes = std::move(payload);

        if (savestate_layout::IsLegacyBrokenLayout(snapshot))
        {
            log::Warn(
                "savestate",
                "load rejected: obsolete layout in %s — delete debug.thvs and re-save with F6",
                displayPath.c_str());
            return false;
        }

        if (!savestate_layout::IsSupportedPayloadSize(v3Meta.payloadSize))
        {
            log::Warn(
                "savestate",
                "load rejected: payload size %u (expected v4=%zu or v3=%zu) in %s — re-save with F6",
                v3Meta.payloadSize,
                savestate_layout::ExpectedPayloadSizeV4(),
                savestate_layout::ExpectedPayloadSizeV3(),
                displayPath.c_str());
            return false;
        }

        PendingRestore pending = {};
        pending.format = PendingRestore::Format::V3Snapshot;
        pending.snapshot = std::move(snapshot);
        pending.fpu = fpuState;
        pending.rng = header.rngShadowState;
        pending.hasDeterminism = true;
        pending.humanSide = headerBase.humanSide;
        pending.bothHuman = headerBase.reserved[0] != 0u;
        pending.slot = slot;

        return ApplyPendingRestore(std::move(pending), false);
    }

    if (headerBase.stateSize != trial::kBattleStateSize)
    {
        std::fclose(file);
        log::Warn(
            "savestate",
            "load rejected: bad v2 size in %s (size=%u expected=%u)",
            displayPath.c_str(),
            headerBase.stateSize,
            static_cast<unsigned>(trial::kBattleStateSize));
        return false;
    }

    SavestateHeader header = {};
    std::memcpy(&header, &headerBase, sizeof(headerBase));
    if (isV2)
    {
        if (std::fread(&header.rngShadowState, sizeof(header.rngShadowState), 1, file) != 1
            || std::fread(&header.fpuStateSize, sizeof(header.fpuStateSize), 1, file) != 1)
        {
            std::fclose(file);
            return false;
        }
    }

    std::array<std::uint8_t, det::kFpuStateSize> fpuState{};
    if (isV2)
    {
        if (header.fpuStateSize != det::kFpuStateSize)
        {
            std::fclose(file);
            return false;
        }
        if (std::fread(fpuState.data(), 1, fpuState.size(), file) != fpuState.size())
        {
            std::fclose(file);
            return false;
        }
    }

    std::vector<std::uint8_t> state(trial::kBattleStateSize);
    if (std::fread(state.data(), 1, state.size(), file) != state.size())
    {
        std::fclose(file);
        return false;
    }
    std::fclose(file);

    const std::uint8_t liveP1 = ReadCharacterId(0);
    const std::uint8_t liveP2 = ReadCharacterId(1);
    if (headerBase.p1CharacterId != liveP1 || headerBase.p2CharacterId != liveP2)
    {
        log::Warn(
            "savestate",
            "load rejected: character mismatch (file P1=%u P2=%u, live P1=%u P2=%u)",
            headerBase.p1CharacterId,
            headerBase.p2CharacterId,
            liveP1,
            liveP2);
        return false;
    }

    PendingRestore pending = {};
    pending.format = PendingRestore::Format::V2Battle;
    pending.battle = std::move(state);
    pending.humanSide = headerBase.humanSide;
    pending.bothHuman = headerBase.reserved[0] != 0u;
    pending.slot = slot;
    if (isV2)
    {
        pending.fpu = fpuState;
        pending.rng = header.rngShadowState;
        pending.hasDeterminism = true;
    }

    return ApplyPendingRestore(std::move(pending), false);
}

int CyclePracticeSlot(const int delta)
{
    int slot = g_activeSlot.load();
    slot += delta;
    if (slot < kPracticeMinSlot)
    {
        slot = kPracticeMaxSlot;
    }
    if (slot > kPracticeMaxSlot)
    {
        slot = kPracticeMinSlot;
    }
    g_activeSlot.store(slot);
    return slot;
}

bool StateFileExists(const SavestateBank bank, const int slot)
{
    const std::filesystem::path primary = GetStatePath(bank, slot);
    DWORD attrs = GetFileAttributesW(primary.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return true;
    }

    if (bank == SavestateBank::Practice)
    {
        const std::filesystem::path legacy = ResolveLoadPath(bank, slot);
        attrs = GetFileAttributesW(legacy.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    return false;
}

}

bool InitializeSavestate()
{
    savestate_digest::LogCaptureContractOnce();
    g_initialized.store(true);
    return true;
}

void ShutdownSavestate()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_initialized.store(false);
}

void PollSavestateHotkeys()
{
    if (!g_initialized.load() || caster::IsNetplayActive() || caster::IsSpectatorActive())
    {
        return;
    }

    const auto pressed = [](const trialnet::shortcuts::Action action) {
        return !controls::ShortcutConflictsWithAnyFightBinding(action)
            && input::WasPressed(controls::ShortcutKey(action));
    };

    if (pressed(trialnet::shortcuts::Action::PreviousSaveSlot))
    {
        const int slot = CyclePracticeSlot(-1);
        log::Info("savestate", "practice slot -> %d", slot);
    }
    if (pressed(trialnet::shortcuts::Action::NextSaveSlot))
    {
        const int slot = CyclePracticeSlot(1);
        log::Info("savestate", "practice slot -> %d", slot);
    }

    if (pressed(trialnet::shortcuts::Action::SaveState))
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        SaveSlotLocked(SavestateBank::Practice, g_activeSlot.load());
    }

    if (pressed(trialnet::shortcuts::Action::LoadState))
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        LoadSlotLocked(SavestateBank::Practice, g_activeSlot.load());
    }

    if (pressed(trialnet::shortcuts::Action::DebugSave))
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        SaveSlotLocked(SavestateBank::Debug, kDebugSlotMarker);
    }

    if (pressed(trialnet::shortcuts::Action::DebugLoad))
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        LoadSlotLocked(SavestateBank::Debug, kDebugSlotMarker);
    }
}

void ProcessSavestateAtFrameBoundary()
{
}

bool SaveState(const SavestateBank bank, const int slot)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return SaveSlotLocked(bank, slot);
}

bool LoadState(const SavestateBank bank, const int slot)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return LoadSlotLocked(bank, slot);
}

int GetActivePracticeSlot()
{
    return g_activeSlot.load();
}

bool HasPracticeSlot(const int slot)
{
    return StateFileExists(SavestateBank::Practice, slot);
}

bool HasDebugSavestate()
{
    return StateFileExists(SavestateBank::Debug, kDebugSlotMarker);
}

unsigned int GetSavestateCount()
{
    return g_saveCount.load();
}

unsigned int GetLoadstateCount()
{
    return g_loadCount.load();
}

}
