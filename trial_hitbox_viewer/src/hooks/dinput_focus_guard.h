#pragma once

namespace thv
{

// Loader-lock safe. Call from DLL_PROCESS_ATTACH.
bool InstallDinputFocusGuard();

// Call from the mod thread before the game's input init.
void ReassertDinputFocusGuard();

unsigned DinputAcquireRescues();

void ShutdownDinputFocusGuard();

}
