#pragma once

#include <windows.h>

namespace thv
{

bool InitializeFramestep();
void ShutdownFramestep();

void PollFramestepHotkeys();

bool IsFramestepPaused();
bool ConsumeFramestepRequest();
void RequestFramestepPause();

void RequestFramestepAdvance();

HWND GetLastGameWindow();
void SetGameWindow(HWND hwnd);
HWND ResolveGameWindow();

}
