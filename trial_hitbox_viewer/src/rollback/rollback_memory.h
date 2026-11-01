#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace thv::rollback::memory
{

bool CaptureBattle(std::vector<std::uint8_t>* out);
bool CaptureBattleSim(std::vector<std::uint8_t>* out);
bool RestoreBattle(const void* src, const std::size_t size, const char* context = nullptr);
bool RestoreBattleSelective(const void* src, const std::size_t size, const char* context = nullptr);
bool WriteRegion(
    void* dst,
    const void* src,
    std::size_t size,
    const char* context = nullptr,
    const char* label = nullptr);

void CaptureDeterminism(std::uint8_t* fpuOut, std::uint32_t* rngOut);
void RestoreDeterminism(const std::uint8_t* fpuIn, std::uint32_t rng);

void CaptureShakeAux(std::uint8_t* out);
void RestoreShakeAux(const std::uint8_t* in);

// Cosmetic state, locally rewound but excluded from peer hashes.
void CaptureVisualEffects(std::uint8_t* out);
void RestoreVisualEffects(const std::uint8_t* in);

}
