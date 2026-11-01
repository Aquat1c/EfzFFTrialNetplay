#pragma once

namespace thv::netplay_input
{

void SyncCleanupPairForLocalSide(int localSide);
void RestoreCleanupPair();

bool IsCleanupPairSwapped();

}
