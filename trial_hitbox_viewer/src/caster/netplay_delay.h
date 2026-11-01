#pragma once

#include <cstdint>

namespace thv::netplay
{

std::uint32_t PingHalfPeriodCeilFrames(std::uint32_t pingMs, std::uint8_t targetFps);

// Truncated: fractional ping must not add an extra local frame.
std::uint32_t PingHalfPeriodFloorFrames(std::uint32_t pingMs, std::uint8_t targetFps);

bool ShouldRepeatLocalInputForRemoteAhead(
    int lead,
    std::uint32_t pingMs,
    std::uint8_t targetFps);

int MinimumLiveInputDelay(
    std::uint32_t pingMs,
    int activeRollbackFrames,
    std::uint8_t targetFps);
int AdjustLiveInputDelay(
    int currentDelay,
    int direction,
    std::uint32_t pingMs,
    int activeRollbackFrames,
    std::uint8_t targetFps);

}
