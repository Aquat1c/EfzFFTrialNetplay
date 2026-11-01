#pragma once

#include <cstdint>

namespace thv::pacing
{

bool ConfigureSessionTargetFps(std::uint8_t targetFps);
void ResetSessionTargetFps();
std::uint8_t GetSessionTargetFps();

// Capture and restore the FPS HUD globals together around each hidden tick.
struct NativeFpsAccountingState
{
    std::int32_t displayedSubsteps = 0;
    std::int32_t pendingSubsteps = 0;
    double sampleAnchorMs = 0.0;
    bool valid = false;
};

NativeFpsAccountingState CaptureNativeFpsAccounting();
void RestoreNativeFpsAccounting(const NativeFpsAccountingState& state);

class ScopedNativeFpsAccountingIsolation final
{
public:
    explicit ScopedNativeFpsAccountingIsolation(bool hidden);
    ~ScopedNativeFpsAccountingIsolation();

    ScopedNativeFpsAccountingIsolation(const ScopedNativeFpsAccountingIsolation&) = delete;
    ScopedNativeFpsAccountingIsolation& operator=(
        const ScopedNativeFpsAccountingIsolation&) = delete;

private:
    NativeFpsAccountingState state_{};
};

// Call once at the start of each outer handler.
void PaceVisibleFrame();

void ConsumeActiveScreenFrameSlot();

void ConsumeBattleFrameSlot();

void BypassNextNativeFrameWait();

// Restore before the visible tick so replay time counts toward the slot.
std::uint32_t CaptureNativeFrameWaitAnchor();
void RestoreNextNativeFrameWaitAnchor(std::uint32_t anchorMs);

void WaitUntilMs(std::uint32_t deadlineMs, void (*pollFn)());

}
