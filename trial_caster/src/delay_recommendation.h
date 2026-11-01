#pragma once

#include "protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace trial_caster::timing
{

inline int RecommendedInitialDelay(
    const std::uint32_t roundTripMs,
    const int currentDelay,
    const std::uint8_t targetFps)
{
    const std::uint8_t effectiveTargetFps = proto::IsSupportedTargetFps(targetFps)
        ? targetFps
        : proto::kDefaultTargetFps;
    const double frameMs = 1000.0 / static_cast<double>(effectiveTargetFps);
    const double framesNeeded =
        std::ceil(static_cast<double>(roundTripMs) / (frameMs * 2.0));
    const int delayFloor = std::max(0, static_cast<int>(framesNeeded) - currentDelay);
    const int minimum = std::min(3, currentDelay);
    int recommended = std::max(
        delayFloor,
        static_cast<int>(std::ceil(framesNeeded - static_cast<double>(minimum))));
    if (currentDelay == minimum)
    {
        ++recommended;
    }
    return std::clamp(recommended, 0, 20);
}

}
