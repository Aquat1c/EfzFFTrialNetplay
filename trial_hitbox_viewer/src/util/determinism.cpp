#include "determinism.h"

#include "debug_log.h"
#include "lifecycle_log.h"

#include <windows.h>

#include <atomic>
#include <cstring>

namespace thv::det
{
namespace
{

constexpr std::uint16_t kPinMask = 0x0F00u;
constexpr std::uint16_t kPinValue = 0x0200u;

std::atomic<bool> g_initialized{false};
std::atomic<bool> g_fpuPinningEnabled{false};
std::atomic<std::uint16_t> g_capturedFpuCw{0};
std::atomic<std::uint16_t> g_lastObservedFpuCw{0};
std::atomic<std::uint32_t> g_fpuMismatchCount{0};
std::atomic<std::uint64_t> g_localFrame{0};

std::uint16_t ReadFpuControlWord()
{
    std::uint16_t cw = 0;
    __asm fnstcw cw;
    return cw;
}

void RepinFpuControlWord()
{
    std::uint16_t cw = ReadFpuControlWord();
    cw = static_cast<std::uint16_t>((cw & ~kPinMask) | kPinValue);
    __asm fldcw cw;
}

void RepinFpuAndTrack()
{
    if (!g_fpuPinningEnabled.load(std::memory_order_acquire))
    {
        return;
    }

    const std::uint16_t live = ReadFpuControlWord();
    const std::uint16_t captured = g_capturedFpuCw.load(std::memory_order_acquire);
    g_lastObservedFpuCw.store(live, std::memory_order_relaxed);
    if ((live & kPinMask) != (captured & kPinMask))
    {
        g_fpuMismatchCount.fetch_add(1, std::memory_order_acq_rel);
    }
    RepinFpuControlWord();
}

}

bool Initialize()
{
    if (g_initialized.exchange(true))
    {
        return true;
    }

    const std::uint16_t before = ReadFpuControlWord();
    RepinFpuControlWord();
    const std::uint16_t pinned = ReadFpuControlWord();
    g_capturedFpuCw.store(pinned, std::memory_order_release);
    g_lastObservedFpuCw.store(pinned, std::memory_order_relaxed);
    g_fpuPinningEnabled.store(true, std::memory_order_release);
    g_localFrame.store(0, std::memory_order_release);
    g_fpuMismatchCount.store(0, std::memory_order_release);

    log::Info(
        "det",
        "FPU pinning enabled before=0x%04X pinned=0x%04X",
        static_cast<unsigned>(before),
        static_cast<unsigned>(pinned));
    return true;
}

void Shutdown()
{
    if (!g_initialized.exchange(false))
    {
        return;
    }
    g_fpuPinningEnabled.store(false, std::memory_order_release);
}

std::uint64_t OnEngineFrameBegin()
{
    if (g_fpuPinningEnabled.load(std::memory_order_acquire))
    {
        const std::uint16_t live = ReadFpuControlWord();
        const std::uint16_t captured = g_capturedFpuCw.load(std::memory_order_acquire);
        g_lastObservedFpuCw.store(live, std::memory_order_relaxed);
        if ((live & kPinMask) != (captured & kPinMask))
        {
            const std::uint32_t count = g_fpuMismatchCount.fetch_add(1, std::memory_order_acq_rel) + 1u;
            if (count == 1u || (count % 60u) == 0u)
            {
                lifecycle::Desync(
                    "fpu",
                    "cw mismatch live=0x%04X captured=0x%04X count=%u frame=%llu",
                    static_cast<unsigned>(live),
                    static_cast<unsigned>(captured),
                    count,
                    static_cast<unsigned long long>(g_localFrame.load(std::memory_order_relaxed)));
            }
        }
        RepinFpuControlWord();
    }

    return g_localFrame.fetch_add(1, std::memory_order_acq_rel) + 1;
}

void RepinFpu()
{
    RepinFpuAndTrack();
}

void ResetFpuForSimulation()
{
    const std::uint16_t pinned = g_capturedFpuCw.load(std::memory_order_acquire);
    __asm {
        fninit
        fldcw pinned
    }
    g_lastObservedFpuCw.store(pinned, std::memory_order_relaxed);
}

void CaptureFpuState(std::uint8_t* dest)
{
    if (dest == nullptr)
    {
        return;
    }

    __asm {
        mov eax, dest
        fsave byte ptr [eax]
        frstor byte ptr [eax]
    }
}

void RestoreFpuState(const std::uint8_t* src)
{
    if (src == nullptr)
    {
        return;
    }

    __asm {
        mov eax, src
        frstor byte ptr [eax]
    }
}

void CanonicalizeFpuStateForHash(const std::uint8_t* src, std::uint8_t* dest)
{
    if (src == nullptr || dest == nullptr)
    {
        return;
    }

    std::memset(dest, 0, kFpuStateSize);
    // FSAVE: control @0, status @4, tag @8, pointers @12..27, registers @28.
    std::memcpy(dest + 0u, src + 0u, 2u);
    std::memcpy(dest + 4u, src + 4u, 2u);
    std::memcpy(dest + 8u, src + 8u, 2u);

    std::uint16_t tag = 0xFFFFu;
    std::memcpy(&tag, src + 8u, sizeof(tag));
    for (int reg = 0; reg < 8; ++reg)
    {
        const bool empty = ((tag >> (reg * 2)) & 0x3u) == 0x3u;
        if (!empty)
        {
            const std::size_t offset = 28u + static_cast<std::size_t>(reg) * 10u;
            std::memcpy(dest + offset, src + offset, 10u);
        }
    }
}

std::uint64_t GetLocalFrame()
{
    return g_localFrame.load(std::memory_order_acquire);
}

std::uint16_t GetCapturedFpuCw()
{
    return g_capturedFpuCw.load(std::memory_order_acquire);
}

Status GetStatus()
{
    Status status = {};
    status.fpuPinningEnabled = g_fpuPinningEnabled.load(std::memory_order_acquire);
    status.capturedFpuCw = g_capturedFpuCw.load(std::memory_order_acquire);
    status.lastObservedFpuCw = g_lastObservedFpuCw.load(std::memory_order_acquire);
    status.fpuMismatchCount = g_fpuMismatchCount.load(std::memory_order_acquire);
    status.localFrame = g_localFrame.load(std::memory_order_acquire);
    return status;
}

}
