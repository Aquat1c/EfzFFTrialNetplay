#include "netplay_delay.h"

#include <algorithm>
#include <cmath>

namespace thv::netplay
{
namespace
{

std::uint8_t NormalizeTargetFps(const std::uint8_t targetFps)
{
    return targetFps == 64u ? 64u : 60u;
}

double PingHalfPeriodFrames(const std::uint32_t pingMs, const std::uint8_t targetFps)
{
    return static_cast<double>(pingMs) * static_cast<double>(NormalizeTargetFps(targetFps))
        / 2000.0;
}

}

std::uint32_t PingHalfPeriodCeilFrames(
    const std::uint32_t pingMs,
    const std::uint8_t targetFps)
{
    if (pingMs == 0u)
    {
        return 0u;
    }

    const double halfPeriodFrames = PingHalfPeriodFrames(pingMs, targetFps);
    if (halfPeriodFrames <= 0.0)
    {
        return 0u;
    }
    return static_cast<std::uint32_t>(std::ceil(halfPeriodFrames - 1e-9));
}

std::uint32_t PingHalfPeriodFloorFrames(
    const std::uint32_t pingMs,
    const std::uint8_t targetFps)
{
    // Integer arithmetic: frame boundaries must not depend on the FPU rounding mode.
    const std::uint64_t scaled =
        static_cast<std::uint64_t>(pingMs) * NormalizeTargetFps(targetFps);
    return static_cast<std::uint32_t>(scaled / 2000u);
}

bool ShouldRepeatLocalInputForRemoteAhead(
    const int lead,
    const std::uint32_t pingMs,
    const std::uint8_t targetFps)
{
    const int remoteAheadThreshold =
        static_cast<int>(PingHalfPeriodFloorFrames(pingMs, targetFps)) - 1;
    return lead < remoteAheadThreshold;
}

int MinimumLiveInputDelay(
    const std::uint32_t pingMs,
    const int activeRollbackFrames,
    const std::uint8_t targetFps)
{
    const int rollback = std::max(0, activeRollbackFrames);
    // Rounding to nearest can permit one frame too few on a fractional half-RTT.
    const int pingFrames = static_cast<int>(PingHalfPeriodCeilFrames(pingMs, targetFps));
    return std::clamp(pingFrames - rollback, 0, 20);
}

int AdjustLiveInputDelay(
    const int currentDelay,
    const int direction,
    const std::uint32_t pingMs,
    const int activeRollbackFrames,
    const std::uint8_t targetFps)
{
    const int current = std::clamp(currentDelay, 0, 20);
    if (direction > 0)
    {
        return std::min(current + 1, 20);
    }
    if (direction < 0)
    {
        return std::max(
            current - 1,
            MinimumLiveInputDelay(pingMs, activeRollbackFrames, targetFps));
    }
    return current;
}

}
