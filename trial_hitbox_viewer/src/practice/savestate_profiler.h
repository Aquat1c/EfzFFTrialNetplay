#pragma once

#include <cstddef>
#include <cstdint>

namespace thv::savestate_profiler
{

void SetEnabled(bool enabled);
bool IsEnabled();

void OnFrameCaptured(const std::uint8_t* data, std::size_t size, std::uint32_t baseVa);

void SampleLiveRegion();

void Dump(const char* reason);

void Reset();

}
