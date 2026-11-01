#include "savestate_digest.h"

#include "debug_log.h"
#include "savestate_layout.h"
#include "trial_addresses.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace thv::savestate_digest
{
namespace
{

constexpr std::uintptr_t kVaComboHits = 0x00CCC132u;
constexpr std::uintptr_t kVaComboDamage = 0x00CCC138u;
constexpr std::uintptr_t kVaWinCount = 0x0047B580u;
constexpr std::uintptr_t kVaFrameSubsteps = 0x0047B658u;

std::atomic<bool> g_loggedContract{false};
std::atomic<unsigned int> g_artifactSerial{0};

std::uint32_t Crc32Update(std::uint32_t crc, const std::uint8_t* data, std::size_t size)
{
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
        {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

template <typename T>
bool ReadVa(const std::uintptr_t va, const std::uint8_t* blob, T* out)
{
    if (out == nullptr)
    {
        return false;
    }

    if (blob != nullptr)
    {
        if (va < trial::kVaBattleStateBegin || va + sizeof(T) > trial::kVaBattleStateEnd)
        {
            return false;
        }

        const std::size_t offset = va - trial::kVaBattleStateBegin;
        if (offset + sizeof(T) > trial::kBattleStateSize)
        {
            return false;
        }

        std::memcpy(out, blob + offset, sizeof(T));
        return true;
    }

    void* const live = trial::ResolveVa(va);
    if (live == nullptr)
    {
        return false;
    }

    std::memcpy(out, live, sizeof(T));
    return true;
}

int CountProjectilesAlive(const std::uint8_t* playerBase)
{
    if (playerBase == nullptr)
    {
        return 0;
    }

    int alive = 0;
    for (int slot = 0; slot < trial::kProjectileRingSize; ++slot)
    {
        const auto* const aliveWord = reinterpret_cast<const std::uint32_t*>(
            playerBase + trial::kOffProjectileAlive + 4u * static_cast<std::size_t>(slot));
        if (*aliveWord != 0u)
        {
            ++alive;
        }
    }
    return alive;
}

PlayerDigest CapturePlayerDigest(const int side, const std::uint8_t* blob)
{
    PlayerDigest digest = {};
    const std::uintptr_t baseVa = (side == 0) ? trial::kVaP1Player : trial::kVaP2Player;

    const std::uint8_t* player = nullptr;
    if (blob != nullptr)
    {
        const std::size_t offset = baseVa - trial::kVaBattleStateBegin;
        if (offset >= trial::kBattleStateSize)
        {
            return digest;
        }
        player = blob + offset;
    }
    else
    {
        player = static_cast<const std::uint8_t*>(trial::ResolveVa(baseVa));
        if (player == nullptr)
        {
            return digest;
        }
    }

    digest.charId = player[0];
    std::memcpy(&digest.moveId, player + trial::kOffMoveId, sizeof(digest.moveId));
    std::memcpy(&digest.hp, player + 0x14u, sizeof(digest.hp));
    std::memcpy(&digest.eForce, player + 0x20u, sizeof(digest.eForce));
    std::memcpy(&digest.sp, player + 0x48u, sizeof(digest.sp));
    digest.facing = static_cast<std::int8_t>(player[trial::kOffFacing]);
    digest.cpuFlag = static_cast<std::int8_t>(player[trial::kOffCpuControlFlag]);
    std::memcpy(&digest.posX, player + 0x70u, sizeof(digest.posX));
    std::memcpy(&digest.posY, player + 0x78u, sizeof(digest.posY));
    digest.subframe = static_cast<std::int32_t>(player[trial::kOffSubframe]);
    digest.projectileAlive = CountProjectilesAlive(player);
    return digest;
}

void AppendPlayerDigest(FILE* file, const char* label, const PlayerDigest& player)
{
    std::fprintf(
        file,
        "%s char=%u move=%d hp=%d eForce=%.1f sp=%d face=%d cpu=%d pos=(%.1f,%.1f) sub=%d proj=%d\n",
        label,
        player.charId,
        player.moveId,
        player.hp,
        player.eForce,
        player.sp,
        player.facing,
        player.cpuFlag,
        player.posX,
        player.posY,
        player.subframe,
        player.projectileAlive);
}

}

void LogCaptureContractOnce()
{
    if (g_loggedContract.exchange(true))
    {
        return;
    }

    log::Info("savestate", "capture contract v4 (sim-safe regions only):");
    for (const auto& region : savestate_layout::kRegionsV4)
    {
        log::Info(
            "savestate",
            "  region %s va=0x%08X size=0x%zX (%zu bytes)",
            region.name,
            static_cast<unsigned>(region.va),
            region.size,
            region.size);
    }
    log::Info(
        "savestate",
        "  excluded: scene_ctx (live screen ptrs), effects (live pool), screen dispatch tail 0x%zX bytes",
        trial::kScreenDispatchSize);
    log::Info(
        "savestate",
        "  payload=%zu bytes (%.2f MB) + FPU 108 + frame metadata",
        savestate_layout::ExpectedPayloadSizeV4(),
        static_cast<double>(savestate_layout::ExpectedPayloadSizeV4()) / (1024.0 * 1024.0));
    log::Info(
        "savestate",
        "  v3 files still load via selective battle_data apply — re-save with F6 for v4");

    ProbeWritableRegion();
}

SavestateDigest CaptureLiveDigest()
{
    SavestateDigest digest = {};
    digest.fromBlob = false;
    digest.p1 = CapturePlayerDigest(0, nullptr);
    digest.p2 = CapturePlayerDigest(1, nullptr);
    ReadVa(trial::kVaCameraX, nullptr, &digest.cameraX);
    ReadVa(trial::kVaCameraY, nullptr, &digest.cameraY);
    ReadVa(trial::kVaBattleScreenMode, nullptr, &digest.screenMode);
    ReadVa(trial::kVaScreenSubState, nullptr, &digest.screenSub);
    ReadVa(trial::kVaScreenAnimCounter, nullptr, &digest.screenAnim);
    ReadVa(kVaComboHits, nullptr, digest.comboHits);
    ReadVa(kVaComboDamage, nullptr, digest.comboDamage);
    ReadVa(kVaWinCount, nullptr, &digest.winCount);
    ReadVa(kVaFrameSubsteps, nullptr, &digest.frameSubsteps);

    void* const battle = trial::ResolveVa(trial::kVaBattleStateBegin);
    if (battle != nullptr)
    {
        digest.blobCrc32 = Crc32(static_cast<const std::uint8_t*>(battle), trial::kBattleStateSize);
    }
    return digest;
}

SavestateDigest CaptureBlobDigest(const std::uint8_t* blob, const std::size_t size)
{
    SavestateDigest digest = {};
    digest.fromBlob = true;
    if (blob == nullptr || size != trial::kBattleStateSize)
    {
        return digest;
    }

    digest.p1 = CapturePlayerDigest(0, blob);
    digest.p2 = CapturePlayerDigest(1, blob);
    ReadVa(trial::kVaCameraX, blob, &digest.cameraX);
    ReadVa(trial::kVaCameraY, blob, &digest.cameraY);
    ReadVa(trial::kVaBattleScreenMode, blob, &digest.screenMode);
    ReadVa(trial::kVaScreenSubState, blob, &digest.screenSub);
    ReadVa(trial::kVaScreenAnimCounter, blob, &digest.screenAnim);
    ReadVa(kVaComboHits, blob, digest.comboHits);
    ReadVa(kVaComboDamage, blob, digest.comboDamage);
    digest.blobCrc32 = Crc32(blob, size);
    return digest;
}

std::uint32_t Crc32(const std::uint8_t* data, const std::size_t size)
{
    if (data == nullptr || size == 0)
    {
        return 0;
    }
    return Crc32Update(0, data, size);
}

void LogDigestSummary(const char* tag, const SavestateDigest& digest)
{
    log::Info(
        "savestate",
        "%s crc=0x%08X screen=%u sub=%u anim=%d cam=(%d,%d) "
        "P1(c=%u m=%d hp=%d) P2(c=%u m=%d hp=%d) outside(win=%u substeps=%d)",
        tag,
        digest.blobCrc32,
        digest.screenMode,
        digest.screenSub,
        digest.screenAnim,
        digest.cameraX,
        digest.cameraY,
        digest.p1.charId,
        digest.p1.moveId,
        digest.p1.hp,
        digest.p2.charId,
        digest.p2.moveId,
        digest.p2.hp,
        digest.winCount,
        digest.frameSubsteps);
}

void LogDigestDetail(const char* tag, const SavestateDigest& digest)
{
    log::Info(
        "savestate",
        "%s P1 move=%d eForce=%.1f sp=%d face=%d cpu=%d pos=(%.1f,%.1f) sub=%d proj=%d",
        tag,
        digest.p1.moveId,
        digest.p1.eForce,
        digest.p1.sp,
        digest.p1.facing,
        digest.p1.cpuFlag,
        digest.p1.posX,
        digest.p1.posY,
        digest.p1.subframe,
        digest.p1.projectileAlive);
    log::Info(
        "savestate",
        "%s P2 move=%d eForce=%.1f sp=%d face=%d cpu=%d pos=(%.1f,%.1f) sub=%d proj=%d",
        tag,
        digest.p2.moveId,
        digest.p2.eForce,
        digest.p2.sp,
        digest.p2.facing,
        digest.p2.cpuFlag,
        digest.p2.posX,
        digest.p2.posY,
        digest.p2.subframe,
        digest.p2.projectileAlive);
    log::Info(
        "savestate",
        "%s combo hits=(%d,%d) damage=(%d,%d) source=%s",
        tag,
        digest.comboHits[0],
        digest.comboHits[1],
        digest.comboDamage[0],
        digest.comboDamage[1],
        digest.fromBlob ? "blob" : "live");
}

void WriteDigestArtifact(const char* tag, const SavestateDigest& digest, const char* extraLines)
{
    const std::filesystem::path sessionDir = log::SessionDirectoryPath();
    if (sessionDir.empty())
    {
        return;
    }

    const unsigned serial = g_artifactSerial.fetch_add(1) + 1;
    char fileName[128] = {};
    std::snprintf(fileName, sizeof(fileName), "savestate_%s_%03u.txt", tag, serial);
    const std::filesystem::path path = sessionDir / fileName;
    const std::string displayPath = log::SessionDirectory() + "\\" + fileName;

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"w") != 0 || file == nullptr)
    {
        log::Warn("savestate", "failed to write digest artifact %s", displayPath.c_str());
        return;
    }

    std::fprintf(file, "Trial EFZ savestate digest\n");
    std::fprintf(file, "tag=%s\n", tag);
    std::fprintf(file, "crc32=0x%08X\n", digest.blobCrc32);
    std::fprintf(file, "source=%s\n", digest.fromBlob ? "blob" : "live");
    if (extraLines != nullptr && extraLines[0] != '\0')
    {
        std::fprintf(file, "%s\n", extraLines);
    }

    std::fprintf(
        file,
        "screen mode=%u sub=%u anim=%d camera=(%d,%d)\n",
        digest.screenMode,
        digest.screenSub,
        digest.screenAnim,
        digest.cameraX,
        digest.cameraY);
    std::fprintf(
        file,
        "outside_region win_count=%u frame_substeps=%d\n",
        digest.winCount,
        digest.frameSubsteps);
    std::fprintf(
        file,
        "combo hits=(%d,%d) damage=(%d,%d)\n",
        digest.comboHits[0],
        digest.comboHits[1],
        digest.comboDamage[0],
        digest.comboDamage[1]);

    AppendPlayerDigest(file, "P1", digest.p1);
    AppendPlayerDigest(file, "P2", digest.p2);
    std::fclose(file);

    log::Info("savestate", "digest artifact -> %s", displayPath.c_str());
}

void WriteTextArtifact(const char* tag, const char* body)
{
    const std::filesystem::path sessionDir = log::SessionDirectoryPath();
    if (sessionDir.empty() || body == nullptr)
    {
        return;
    }

    const unsigned serial = g_artifactSerial.fetch_add(1) + 1;
    char fileName[128] = {};
    std::snprintf(fileName, sizeof(fileName), "savestate_%s_%03u.txt", tag, serial);
    const std::filesystem::path path = sessionDir / fileName;
    const std::string displayPath = log::SessionDirectory() + "\\" + fileName;

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"w") != 0 || file == nullptr)
    {
        log::Warn("savestate", "failed to write fulldump artifact %s", displayPath.c_str());
        return;
    }

    std::fputs(body, file);
    std::fclose(file);
    log::Info("savestate", "fulldump artifact -> %s", displayPath.c_str());
}

void LogDigestDiff(const char* tag, const SavestateDigest& before, const SavestateDigest& after)
{
    if (before.blobCrc32 != after.blobCrc32)
    {
        log::Warn(
            "savestate",
            "%s crc changed 0x%08X -> 0x%08X",
            tag,
            before.blobCrc32,
            after.blobCrc32);
    }

    auto logPlayerDiff = [&](const char* label, const PlayerDigest& a, const PlayerDigest& b) {
        if (a.moveId != b.moveId || a.hp != b.hp || a.subframe != b.subframe || a.cpuFlag != b.cpuFlag)
        {
            log::Info(
                "savestate",
                "%s %s move %d->%d hp %d->%d sub %d->%d cpu %d->%d proj %d->%d",
                tag,
                label,
                a.moveId,
                b.moveId,
                a.hp,
                b.hp,
                a.subframe,
                b.subframe,
                a.cpuFlag,
                b.cpuFlag,
                a.projectileAlive,
                b.projectileAlive);
        }
    };

    logPlayerDiff("P1", before.p1, after.p1);
    logPlayerDiff("P2", before.p2, after.p2);

    if (before.screenMode != after.screenMode || before.screenSub != after.screenSub)
    {
        log::Warn(
            "savestate",
            "%s screen mode %u->%u sub %u->%u",
            tag,
            before.screenMode,
            after.screenMode,
            before.screenSub,
            after.screenSub);
    }

    if (before.cameraX != after.cameraX || before.cameraY != after.cameraY)
    {
        log::Info(
            "savestate",
            "%s camera (%d,%d)->(%d,%d)",
            tag,
            before.cameraX,
            before.cameraY,
            after.cameraX,
            after.cameraY);
    }
}

void ProbeWritableRegion()
{
    auto* cursor = static_cast<std::uint8_t*>(trial::ResolveVa(trial::kVaBattleStateBegin));
    if (cursor == nullptr)
    {
        log::Error("savestate", "region probe failed: battle VA unresolved");
        return;
    }

    std::size_t remaining = trial::kBattleStateSize;
    std::size_t regionIndex = 0;
    std::size_t writableBytes = 0;
    std::size_t readOnlyBytes = 0;
    std::size_t noAccessBytes = 0;

    while (remaining > 0u)
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(cursor, &mbi, sizeof(mbi)) == 0u)
        {
            log::Error(
                "savestate",
                "region probe VirtualQuery failed at region=%zu offset=0x%zX",
                regionIndex,
                trial::kBattleStateSize - remaining);
            return;
        }

        const auto pageBase = reinterpret_cast<std::uint8_t*>(mbi.BaseAddress);
        const auto offsetInPage = static_cast<std::size_t>(cursor - pageBase);
        const auto bytesInRegion = mbi.RegionSize - offsetInPage;
        const auto chunk = (remaining < bytesInRegion) ? remaining : bytesInRegion;

        const bool writable =
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            != 0u;
        const bool readable =
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ
                            | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            != 0u;

        if (writable)
        {
            writableBytes += chunk;
        }
        else if (readable)
        {
            readOnlyBytes += chunk;
        }
        else
        {
            noAccessBytes += chunk;
        }

        if (!writable || !readable)
        {
            log::Warn(
                "savestate",
                "region[%zu] offset=0x%zX size=0x%zX protect=0x%lX state=%lu writable=%d",
                regionIndex,
                trial::kBattleStateSize - remaining,
                chunk,
                static_cast<unsigned long>(mbi.Protect),
                static_cast<unsigned long>(mbi.State),
                writable ? 1 : 0);
        }

        cursor += chunk;
        remaining -= chunk;
        ++regionIndex;
    }

    log::Info(
        "savestate",
        "region probe %zu pages: writable=%zu readonly=%zu noaccess=%zu total=%zu",
        regionIndex,
        writableBytes,
        readOnlyBytes,
        noAccessBytes,
        trial::kBattleStateSize);
}

namespace
{

constexpr std::size_t kMaxHexDumpBytes = 4096u;

void AppendHexDump(std::string& out, const char* label, const std::uint8_t* data, std::size_t size)
{
    const std::size_t dumpSize = (size > kMaxHexDumpBytes) ? kMaxHexDumpBytes : size;
    out += "=== ";
    out += label;
    out += " (";
    out += std::to_string(size);
    if (size > dumpSize)
    {
        out += " bytes, hex first ";
        out += std::to_string(dumpSize);
        out += " only";
    }
    out += " bytes, crc32=0x";
    char crcBuf[16];
    std::snprintf(crcBuf, sizeof(crcBuf), "%08X", Crc32(data, size));
    out += crcBuf;
    out += ") ===\n";

    constexpr std::size_t kBytesPerLine = 16;
    for (std::size_t offset = 0; offset < dumpSize; offset += kBytesPerLine)
    {
        char line[96];
        const std::size_t lineLen = (dumpSize - offset < kBytesPerLine) ? (dumpSize - offset) : kBytesPerLine;
        std::snprintf(line, sizeof(line), "%08zX: ", offset);
        out += line;
        for (std::size_t i = 0; i < lineLen; ++i)
        {
            std::snprintf(line, sizeof(line), "%02X ", data[offset + i]);
            out += line;
        }
        for (std::size_t i = lineLen; i < kBytesPerLine; ++i)
        {
            out += "   ";
        }
        out += " |";
        for (std::size_t i = 0; i < lineLen; ++i)
        {
            const unsigned char c = data[offset + i];
            out += (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
        }
        out += "|\n";
    }
    out += '\n';
}

void AppendPlayerFields(std::string& out, const char* label, const std::uint8_t* player, std::size_t playerSize)
{
    out += "--- decoded ";
    out += label;
    out += " ---\n";
    if (playerSize < 0x100)
    {
        out += "  (buffer too small for decode)\n";
        return;
    }

    const auto readI32 = [&](std::size_t off) -> std::int32_t {
        if (off + 4 > playerSize)
        {
            return 0;
        }
        std::int32_t v = 0;
        std::memcpy(&v, player + off, 4);
        return v;
    };
    const auto readI16 = [&](std::size_t off) -> std::int16_t {
        if (off + 2 > playerSize)
        {
            return 0;
        }
        std::int16_t v = 0;
        std::memcpy(&v, player + off, 2);
        return v;
    };
    const auto readU8 = [&](std::size_t off) -> std::uint8_t {
        return (off < playerSize) ? player[off] : 0;
    };
    const auto readF64 = [&](std::size_t off) -> double {
        if (off + 8 > playerSize)
        {
            return 0.0;
        }
        double v = 0.0;
        std::memcpy(&v, player + off, 8);
        return v;
    };

    char buf[256];
    std::snprintf(buf, sizeof(buf), "  charId=%u moveId=%d hp=%d\n", readU8(0), readI16(2), readI32(4));
    out += buf;
    std::snprintf(buf, sizeof(buf), "  posX=%.3f posY=%.3f facing=%d\n", readF64(0x10), readF64(0x18), readI16(0x20));
    out += buf;
    std::snprintf(buf, sizeof(buf), "  sp=%d cpuFlag=%d subframe=%d\n", readI16(0x24), readU8(0x26), readI32(0x28));
    out += buf;
}

}

void WriteFullSavestateDump(
    const char* tag,
    const savestate_layout::SnapshotPayload& snapshot,
    const std::uint8_t* fpu,
    const std::size_t fpuSize,
    const char* extraLines)
{
    std::string body;
    if (extraLines != nullptr && extraLines[0] != '\0')
    {
        body += extraLines;
        body += '\n';
    }

    body += "=== v3 snapshot fulldump ===\n";
    body += "payloadSize=";
    body += std::to_string(snapshot.bytes.size());
    body += " payloadCrc32=0x";
    char crcBuf[16];
    std::snprintf(crcBuf, sizeof(crcBuf), "%08X", snapshot.meta.payloadCrc32);
    body += crcBuf;
    body += " regionCount=";
    body += std::to_string(snapshot.regions.size());
    body += '\n';

    char frameBuf[128];
    std::snprintf(
        frameBuf,
        sizeof(frameBuf),
        "p1Frame moveId=%d subframe=%d | p2Frame moveId=%d subframe=%d\n",
        snapshot.meta.p1Frame.moveId,
        snapshot.meta.p1Frame.subframe,
        snapshot.meta.p2Frame.moveId,
        snapshot.meta.p2Frame.subframe);
    body += frameBuf;

    for (const savestate_layout::RegionEntry& region : snapshot.regions)
    {
        if (region.payloadOffset + region.size > snapshot.bytes.size())
        {
            continue;
        }

        const std::uint8_t* const data = snapshot.bytes.data() + region.payloadOffset;
        char label[48];
        std::snprintf(label, sizeof(label), "%s@0x%08X", region.name, region.va);
        AppendHexDump(body, label, data, region.size);

        if (std::strncmp(region.name, "p1_core", sizeof(region.name)) == 0
            || std::strncmp(region.name, "p2_core", sizeof(region.name)) == 0)
        {
            AppendPlayerFields(body, region.name, data, region.size);
        }
        if (std::strncmp(region.name, "battle_data", sizeof(region.name)) == 0)
        {
            const std::size_t p1Off = trial::kVaP1Player - trial::kVaBattleStateBegin;
            const std::size_t p2Off = trial::kVaP2Player - trial::kVaBattleStateBegin;
            if (region.size >= p1Off + 0x100)
            {
                AppendPlayerFields(body, "p1", data + p1Off, region.size - p1Off);
            }
            if (region.size >= p2Off + 0x100)
            {
                AppendPlayerFields(body, "p2", data + p2Off, region.size - p2Off);
            }
        }
    }

    if (fpu != nullptr && fpuSize > 0)
    {
        AppendHexDump(body, "fpu", fpu, fpuSize);
    }

    AppendHexDump(body, "p1_frame_blob", snapshot.meta.p1Frame.blob, sizeof(snapshot.meta.p1Frame.blob));
    AppendHexDump(body, "p2_frame_blob", snapshot.meta.p2Frame.blob, sizeof(snapshot.meta.p2Frame.blob));

    WriteTextArtifact(tag, body.c_str());
    log::Info("savestate", "wrote fulldump artifact tag=%s bytes=%zu", tag, body.size());
}

void WriteFullBattleDump(const char* tag, const std::uint8_t* battleBlob, std::size_t battleSize, const char* extraLines)
{
    std::string body;
    if (extraLines != nullptr && extraLines[0] != '\0')
    {
        body += extraLines;
        body += '\n';
    }

    body += "=== v2 battle blob fulldump ===\n";
    body += "size=";
    body += std::to_string(battleSize);
    body += " crc32=0x";
    char crcBuf[16];
    std::snprintf(crcBuf, sizeof(crcBuf), "%08X", Crc32(battleBlob, battleSize));
    body += crcBuf;
    body += "\n\n";

    AppendHexDump(body, "battle_blob", battleBlob, battleSize);

    const std::size_t p1Off = trial::kVaP1Player - trial::kVaBattleStateBegin;
    if (battleSize >= p1Off + 0x100)
    {
        AppendPlayerFields(body, "p1", battleBlob + p1Off, battleSize - p1Off);
    }
    const std::size_t p2Off = trial::kVaP2Player - trial::kVaBattleStateBegin;
    if (battleSize >= p2Off + 0x100)
    {
        AppendPlayerFields(body, "p2", battleBlob + p2Off, battleSize - p2Off);
    }

    WriteTextArtifact(tag, body.c_str());
    log::Info("savestate", "wrote fulldump artifact tag=%s bytes=%zu", tag, body.size());
}

}
