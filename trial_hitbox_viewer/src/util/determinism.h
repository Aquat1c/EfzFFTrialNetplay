#pragma once

#include <cstddef>
#include <cstdint>

namespace thv::det
{

constexpr std::size_t kFpuStateSize = 108u;

struct Status
{
    bool fpuPinningEnabled = false;
    std::uint16_t capturedFpuCw = 0;
    std::uint16_t lastObservedFpuCw = 0;
    std::uint32_t fpuMismatchCount = 0;
    std::uint64_t localFrame = 0;
};

bool Initialize();
void Shutdown();

std::uint64_t OnEngineFrameBegin();
void RepinFpu();
void ResetFpuForSimulation();

void CaptureFpuState(std::uint8_t* dest);
void RestoreFpuState(const std::uint8_t* src);

void CanonicalizeFpuStateForHash(const std::uint8_t* src, std::uint8_t* dest);

std::uint64_t GetLocalFrame();
std::uint16_t GetCapturedFpuCw();
Status GetStatus();

}
