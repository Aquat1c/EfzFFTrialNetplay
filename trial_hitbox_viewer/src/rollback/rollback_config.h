#pragma once

namespace thv::rollback
{

constexpr bool kCompiledIn = true;

constexpr bool kDefaultEnabled = false;

constexpr int kFrameRingSize = 60;

// Hitting this bound wedges the sim: WaitRemote's safe frame can no longer advance.
constexpr int kMaxInputHistory = 1 << 20;

// (1 + inputDelay) + window must stay below proto::kMaxQueueSlot.
constexpr int kMaxPredictionFrames = 20;

}
