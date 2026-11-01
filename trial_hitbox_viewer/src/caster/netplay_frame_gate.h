#pragma once

#include <cstdint>

namespace thv::netplay
{

bool ShouldStallLockstepFrame();

bool ShouldBlockBattleSimForHandoff();

bool ProcessBattleHandoffSyncTick();

void WaitUntilLaunchAt(std::uint32_t launchAtMs);

void WaitLaunchLeadMs(std::uint32_t leadMs);

}
