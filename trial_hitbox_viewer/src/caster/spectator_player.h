#pragma once

#include "caster_session.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace thv::spectator::player
{

using ScreenTickFn = int(__cdecl*)(HWND);

inline constexpr bool IsHiddenCatchUpTick(
    const std::size_t index,
    const std::size_t batchSize)
{
    return index < batchSize && index + 1u < batchSize;
}

bool Initialize(const session::LauncherSession& launcherSession);
void Shutdown();
// Worker-thread transport only; playback state is owned by DriveScreenTick.
void OnModTick(HWND gameWindow);
bool IsActive();

// When true, the caller must return outResult and run no native path.
bool DriveScreenTick(
    HWND gameWindow,
    std::uint8_t hookedScreen,
    ScreenTickFn nativeTick,
    int* outResult);

}
