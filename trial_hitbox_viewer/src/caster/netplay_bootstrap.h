#pragma once

#include "caster_session.h"

#include <windows.h>

namespace thv::netplay
{

void BeginSession(const session::LauncherSession& session);
void BeginDebugVsHuman(int localSide);
void Shutdown();
void OnModTick(HWND gameWindow);

const char* GetBootstrapPhaseName();
bool IsSessionActive();
void ForceEnterVsHuman();

}
