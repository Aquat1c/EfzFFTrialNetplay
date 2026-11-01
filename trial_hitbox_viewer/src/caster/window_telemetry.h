#pragma once

#include <windows.h>

namespace thv::window_telemetry
{

// Never writes simulation memory or participates in snapshots.
void Poll(HWND gameWindow);

// Call only from a non-reentrant top-level screen hook.
bool QuiesceGameThreadForOnlineExit();

// Mod-thread maintenance must not mutate session state while this is true.
bool IsOnlineExitPending();

void Shutdown();

}
