#pragma once

#include <cstdint>

namespace thv
{

bool InitializeRenderGate();
void ShutdownRenderGate();

// While true, present and SFX Play are skipped.
void SetPresentSuppressed(bool suppressed);
bool IsPresentSuppressed();

std::uint64_t GetPresentedFrameCount();

void SetRollbackAudioFrame(std::uint32_t frame, bool active);
void ResetRollbackAudioDedup();

}
