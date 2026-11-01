#pragma once

namespace thv
{

bool InitializeControlSwap();
void ShutdownControlSwap();

void PollControlSwapHotkeys();

int GetHumanSide();
void SetHumanSide(int side);

bool BothPlayersUseHumanInput();

void ConfigureNetplaySides(int localSide);
void MaintainNetplayCpuFlags();
void ResetNetplaySideConfig();

void RestorePracticeBothHuman(int humanSide);

}
