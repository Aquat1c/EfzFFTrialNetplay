#include "netplay_frame_gate.h"

#include "caster_input.h"
#include "debug_log.h"
#include "lifecycle_log.h"
#include "game_pacing.h"
#include "menu_nav.h"
#include "netplay_lockstep.h"
#include "netplay_sync.h"
#include "rollback.h"
#include "trial_protocol.h"

namespace thv::netplay
{

bool ShouldStallLockstepFrame()
{
    if (!caster::IsNetplayActive() || rollback::IsEnabled())
    {
        return false;
    }

    if (!menu::IsSyncEligibleNow())
    {
        return false;
    }

    if (lockstep::WaitRemoteIterations() <= 0)
    {
        static std::uint32_t s_stallLog = 0;
        if ((++s_stallLog % 60u) == 1u)
        {
            const char* phase = menu::IsFightInputAllowedNow() ? "lockstep_fight" : "lockstep_frontend";
            lifecycle::Stall(phase, "wait_remote");
        }
        return true;
    }
    return false;
}

bool ShouldBlockBattleSimForHandoff()
{
    return false;
}

bool ProcessBattleHandoffSyncTick()
{
    return false;
}

void WaitUntilLaunchAt(const std::uint32_t launchAtMs)
{
    pacing::WaitUntilMs(launchAtMs, &Poll);
}

void WaitLaunchLeadMs(const std::uint32_t leadMs)
{
    if (leadMs == 0u)
    {
        return;
    }
    WaitUntilLaunchAt(static_cast<std::uint32_t>(GetTickCount()) + leadMs);
}

}
