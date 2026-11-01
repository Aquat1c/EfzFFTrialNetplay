#pragma once

#include <cstdint>

namespace thv::det::rng
{

struct Status
{
    bool iatSrandHooked = false;
    bool iatRandHooked = false;
    bool staticRandHooked = false;
    bool staticSrandHooked = false;
    std::uint32_t shadowState = 0;
    std::uint32_t srandCalls = 0;
    std::uint32_t randCalls = 0;
    std::uint32_t visualRandCalls = 0;
    std::uint32_t lastSeed = 0;
};

bool Initialize();
void Shutdown();

std::uint32_t Snapshot();
void Restore(std::uint32_t state);
void SetSeed(std::uint32_t seed);

void PushVisualRand();
void PopVisualRand();
std::uint32_t VisualSnapshot();
void VisualRestore(std::uint32_t state);

int PredictCanary(std::uint32_t seed);

Status GetStatus();

}
