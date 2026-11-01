#include "rollback_memory.h"

#include "debug_log.h"
#include "lifecycle_log.h"
#include "mem_write_log.h"
#include "determinism.h"
#include "rng.h"
#include "trial_addresses.h"

#include <windows.h>

#include <cstring>

namespace thv::rollback::memory
{
namespace
{

bool WriteRegionImpl(
    void* dst,
    const void* src,
    const std::size_t size,
    const char* context,
    const char* label)
{
    if (dst == nullptr || src == nullptr || size == 0)
    {
        lifecycle::Rollback("mem", "WriteRegion bad args dst=%p src=%p size=%zu", dst, src, size);
        log::Error("rollback.mem", "WriteRegion bad args dst=%p src=%p size=%zu", dst, src, size);
        return false;
    }

    memlog::LogRegionWrite(context, label, dst, src, size);

    MEMORY_BASIC_INFORMATION mbi = {};
    auto* cursor = static_cast<std::uint8_t*>(dst);
    auto remaining = size;
    const auto* read = static_cast<const std::uint8_t*>(src);

    std::size_t written = 0;
    while (remaining > 0u)
    {
        if (VirtualQuery(cursor, &mbi, sizeof(mbi)) == 0u)
        {
            lifecycle::Rollback(
                "mem",
                "VirtualQuery failed offset=0x%zX remaining=%zu",
                written,
                remaining);
            log::Error(
                "rollback.mem",
                "VirtualQuery failed offset=0x%zX remaining=%zu",
                written,
                remaining);
            return false;
        }

        const auto pageBase = reinterpret_cast<std::uint8_t*>(mbi.BaseAddress);
        const auto offsetInPage = static_cast<std::size_t>(cursor - pageBase);
        const auto writableInRegion = mbi.RegionSize - offsetInPage;
        const auto chunk = (remaining < writableInRegion) ? remaining : writableInRegion;

        DWORD oldProtect = 0;
        const bool needProtect =
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0u;
        if (needProtect && !VirtualProtect(pageBase, mbi.RegionSize, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            lifecycle::Rollback(
                "mem",
                "VirtualProtect failed offset=0x%zX protect=0x%lX state=%lu",
                written,
                static_cast<unsigned long>(mbi.Protect),
                static_cast<unsigned long>(mbi.State));
            log::Error(
                "rollback.mem",
                "VirtualProtect failed offset=0x%zX protect=0x%lX state=%lu",
                written,
                static_cast<unsigned long>(mbi.Protect),
                static_cast<unsigned long>(mbi.State));
            return false;
        }

        std::memcpy(cursor, read, chunk);

        if (needProtect)
        {
            DWORD ignored = 0;
            VirtualProtect(pageBase, mbi.RegionSize, oldProtect, &ignored);
        }

        cursor += chunk;
        read += chunk;
        remaining -= chunk;
        written += chunk;
    }

    memlog::LogPostWriteVerify(context, label, dst, src, size);
    return true;
}

}

bool WriteRegion(
    void* dst,
    const void* src,
    const std::size_t size,
    const char* context,
    const char* label)
{
    return WriteRegionImpl(dst, src, size, context, label);
}

bool CaptureBattle(std::vector<std::uint8_t>* out)
{
    return CaptureBattleSim(out);
}

bool CaptureBattleSim(std::vector<std::uint8_t>* out)
{
    if (out == nullptr)
    {
        lifecycle::Rollback("mem", "CaptureBattleSim out=null");
        log::Error("rollback.mem", "CaptureBattleSim out=null");
        return false;
    }

    out->resize(trial::kDynamicBattleSize);
    std::size_t off = 0;
    for (const trial::DynRegion& region : trial::kDynamicBattleRegions)
    {
        const void* const src = trial::ResolveVa(region.va);
        if (src == nullptr)
        {
            lifecycle::Rollback("mem", "CaptureBattleSim VA 0x%08X unresolved", static_cast<unsigned>(region.va));
            log::Error("rollback.mem", "CaptureBattleSim VA 0x%08X unresolved", static_cast<unsigned>(region.va));
            return false;
        }
        std::memcpy(out->data() + off, src, region.size);
        off += region.size;
    }

    memlog::LogBlobHash("rollback_capture", "dyn_battle", out->data(), out->size());
    log::Debug("rollback.mem", "CaptureBattleSim ok bytes=%zu (dynamic)", out->size());
    return true;
}

bool RestoreBattle(const void* src, const std::size_t size, const char* context)
{
    return RestoreBattleSelective(src, size, context);
}

bool RestoreBattleSelective(const void* src, const std::size_t size, const char* context)
{
    if (src == nullptr)
    {
        log::Error("rollback.mem", "RestoreBattleSelective src=null");
        return false;
    }

    const char* const ctx = context != nullptr ? context : "rollback_restore";
    const auto* const bytes = static_cast<const std::uint8_t*>(src);
    memlog::LogGameHeadline("before_restore");
    bool ok = true;

    if (size == trial::kDynamicBattleSize)
    {
        std::size_t off = 0;
        for (const trial::DynRegion& region : trial::kDynamicBattleRegions)
        {
            auto* const dst = static_cast<std::uint8_t*>(trial::ResolveVa(region.va));
            if (dst == nullptr)
            {
                log::Error("rollback.mem", "RestoreBattleSelective VA 0x%08X unresolved", static_cast<unsigned>(region.va));
                return false;
            }
            if (!WriteRegionImpl(dst, bytes + off, region.size, ctx, "dyn_battle"))
            {
                ok = false;
                break;
            }
            off += region.size;
        }
    }
    else if (size >= trial::kBattleSimSize + trial::kMatchAuxSize)
    {
        auto* const simDst = static_cast<std::uint8_t*>(trial::ResolveVa(trial::kVaBattleStateBegin));
        auto* const auxDst = static_cast<std::uint8_t*>(trial::ResolveVa(trial::kVaMatchAuxBegin));
        if (simDst == nullptr || auxDst == nullptr)
        {
            log::Error("rollback.mem", "RestoreBattleSelective battle VA unresolved");
            return false;
        }
        ok = WriteRegionImpl(simDst, bytes, trial::kBattleSimSize, ctx, "battle_sim")
            && WriteRegionImpl(auxDst, bytes + trial::kBattleSimSize, trial::kMatchAuxSize, ctx, "match_aux");
    }
    else
    {
        lifecycle::Rollback("mem", "RestoreBattleSelective bad size=%zu", size);
        log::Error("rollback.mem", "RestoreBattleSelective bad size=%zu (need %zu or >=%zu)",
            size, trial::kDynamicBattleSize, trial::kBattleSimSize + trial::kMatchAuxSize);
        return false;
    }

    if (ok)
    {
        memlog::LogGameHeadline("after_restore");
        log::Debug("rollback.mem", "RestoreBattleSelective ok bytes=%zu ctx=%s", size, ctx);
    }
    else
    {
        lifecycle::Rollback("mem", "RestoreBattleSelective failed ctx=%s", ctx);
        log::Error("rollback.mem", "RestoreBattleSelective failed");
    }
    return ok;
}

// Restoring these is safe because BatchRender fences every handler transition.
void CaptureShakeAux(std::uint8_t* out)
{
    if (out == nullptr)
    {
        return;
    }
    std::memset(out, 0, trial::kAuxStateSize);
    const void* const shake = trial::ResolveVa(trial::kVaShakeState);
    if (shake != nullptr)
    {
        std::memcpy(out + trial::kAuxShakeOffset, shake, trial::kShakeStateSize);
    }
    const void* const charge = trial::ResolveVa(trial::kVaChargeEdgeFlags);
    if (charge != nullptr)
    {
        std::memcpy(out + trial::kAuxChargeEdgeOffset, charge, trial::kChargeEdgeSize);
    }
    const auto capture = [out](const std::uintptr_t va, const std::size_t offset, const std::size_t size)
    {
        const void* const src = trial::ResolveVa(va);
        if (src != nullptr)
        {
            std::memcpy(out + offset, src, size);
        }
    };
    capture(trial::kVaBattleScreenMode, trial::kAuxScreenModeOffset, sizeof(std::uint8_t));
    capture(trial::kVaScreenSubState, trial::kAuxScreenSubStateOffset, sizeof(std::uint32_t));
    capture(trial::kVaScreenAnimCounter, trial::kAuxScreenAnimCounterOffset, sizeof(std::int32_t));
    capture(trial::kVaWinCount, trial::kAuxWinCountOffset, sizeof(std::uint8_t));
    capture(trial::kVaCharSelectSide, trial::kAuxWinnerSideOffset, sizeof(std::uint8_t));
}

void RestoreShakeAux(const std::uint8_t* in)
{
    if (in == nullptr)
    {
        return;
    }
    void* const shake = trial::ResolveVa(trial::kVaShakeState);
    void* const charge = trial::ResolveVa(trial::kVaChargeEdgeFlags);
    if ((shake != nullptr
            && std::memcmp(shake, in + trial::kAuxShakeOffset, trial::kShakeStateSize) != 0)
        || (charge != nullptr
            && std::memcmp(charge, in + trial::kAuxChargeEdgeOffset, trial::kChargeEdgeSize) != 0))
    {
        memlog::LogBlobHash("rollback_shake", "shake_aux", in, trial::kAuxStateSize);
    }
    if (shake != nullptr)
    {
        std::memcpy(shake, in + trial::kAuxShakeOffset, trial::kShakeStateSize);
    }
    if (charge != nullptr)
    {
        std::memcpy(charge, in + trial::kAuxChargeEdgeOffset, trial::kChargeEdgeSize);
    }
    const auto restore = [in](const std::uintptr_t va, const std::size_t offset, const std::size_t size)
    {
        void* const dst = trial::ResolveVa(va);
        if (dst != nullptr)
        {
            std::memcpy(dst, in + offset, size);
        }
    };
    restore(trial::kVaBattleScreenMode, trial::kAuxScreenModeOffset, sizeof(std::uint8_t));
    restore(trial::kVaScreenSubState, trial::kAuxScreenSubStateOffset, sizeof(std::uint32_t));
    restore(trial::kVaScreenAnimCounter, trial::kAuxScreenAnimCounterOffset, sizeof(std::int32_t));
    restore(trial::kVaWinCount, trial::kAuxWinCountOffset, sizeof(std::uint8_t));
    restore(trial::kVaCharSelectSide, trial::kAuxWinnerSideOffset, sizeof(std::uint8_t));
}

void CaptureVisualEffects(std::uint8_t* out)
{
    if (out == nullptr)
    {
        return;
    }
    const void* const effects = trial::ResolveVa(trial::kVaEffectSystem);
    if (effects != nullptr)
    {
        std::memcpy(out, effects, trial::kEffectDynamicStateSize);
    }
    else
    {
        std::memset(out, 0, trial::kEffectDynamicStateSize);
    }
}

void RestoreVisualEffects(const std::uint8_t* in)
{
    if (in == nullptr)
    {
        return;
    }
    void* const effects = trial::ResolveVa(trial::kVaEffectSystem);
    if (effects != nullptr)
    {
        std::memcpy(effects, in, trial::kEffectDynamicStateSize);
    }
}

void CaptureDeterminism(std::uint8_t* fpuOut, std::uint32_t* rngOut)
{
    if (fpuOut != nullptr)
    {
        det::CaptureFpuState(fpuOut);
    }
    if (rngOut != nullptr)
    {
        *rngOut = det::rng::Snapshot();
    }
}

void RestoreDeterminism(const std::uint8_t* fpuIn, const std::uint32_t rng)
{
    const std::uint32_t rngBefore = det::rng::Snapshot();
    const std::uint16_t fpuCwBefore = det::GetCapturedFpuCw();
    if (fpuIn != nullptr)
    {
        det::RestoreFpuState(fpuIn);
    }
    det::rng::Restore(rng);
    const std::uint32_t rngAfter = det::rng::Snapshot();
    const std::uint16_t fpuCwAfter = det::GetCapturedFpuCw();
    if (fpuIn != nullptr && fpuCwBefore != fpuCwAfter)
    {
        memlog::LogScalarWrite("rollback_fpu", 0u, "fpu_cw", fpuCwBefore, fpuCwAfter, 2);
    }
    if (fpuIn != nullptr)
    {
        memlog::LogBlobHash("rollback_fpu", "fpu_blob", fpuIn, 108u);
        log::Debug(
            "rollback.mem",
            "ctx=rollback_fpu cw %u->%u rng %u->%u",
            static_cast<unsigned>(fpuCwBefore),
            static_cast<unsigned>(fpuCwAfter),
            rngBefore,
            rngAfter);
    }
}

}
