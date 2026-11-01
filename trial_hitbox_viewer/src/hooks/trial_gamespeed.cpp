#include "trial_gamespeed.h"

#include "debug_log.h"
#include "mem_write_log.h"
#include "trial_addresses.h"

namespace thv::gamespeed
{

SavedState PushResimSpeed(const int speed)
{
    SavedState saved{};
    auto* const substeps = static_cast<int*>(::trial::ResolveVa(::trial::kVaFrameSubsteps));
    if (substeps == nullptr)
    {
        return saved;
    }

    saved.substeps = *substeps;
    if (speed > 0 && *substeps != speed)
    {
        memlog::LogScalarWrite(
            "gamespeed_push",
            trial::kVaFrameSubsteps,
            "frame_substeps",
            static_cast<std::uint64_t>(*substeps),
            static_cast<std::uint64_t>(speed),
            4);
        *substeps = speed;
        log::Debug("gamespeed", "resim boost %d -> %d", saved.substeps, speed);
    }
    return saved;
}

void PopResimSpeed(const SavedState& saved)
{
    auto* const substeps = static_cast<int*>(::trial::ResolveVa(::trial::kVaFrameSubsteps));
    if (substeps == nullptr)
    {
        return;
    }

    if (*substeps != saved.substeps)
    {
        memlog::LogScalarWrite(
            "gamespeed_pop",
            trial::kVaFrameSubsteps,
            "frame_substeps",
            static_cast<std::uint64_t>(*substeps),
            static_cast<std::uint64_t>(saved.substeps),
            4);
        *substeps = saved.substeps;
        log::Debug("gamespeed", "resim restore -> %d", saved.substeps);
    }
}

}
