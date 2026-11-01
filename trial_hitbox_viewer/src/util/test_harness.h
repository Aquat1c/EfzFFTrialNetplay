#pragma once

#include <cstdint>

// Configured from the [test] section of the session ini.
namespace thv::harness
{

struct NetShimDecision
{
    bool drop = false;
    std::uint32_t delayMs = 0;
};

bool IsEnabled();
bool ScriptedInputEnabled();
bool NetShimEnabled();

std::uint8_t ScriptedLocalMask(std::uint32_t frameIndex, int localSide);

std::uint8_t ScriptedCharSelectMask(std::uint32_t frameIndex);

std::uint8_t ScriptedSystemState(
    std::uint32_t currentFrame,
    std::uint32_t captureFrame,
    std::uint32_t matchStartFrame,
    int localSide);

int ConsumeScriptedLiveDelayDirection(int localSide);

NetShimDecision DecideOutgoing();

// Forbidden in spectator-fidelity runs: this mutation isn't in the input recording.
void MaybeForceLifecycleProgress(std::uint32_t currentFrame, std::uint32_t matchStartFrame);

void MaybeEmitVerdict();

}
