#pragma once

#include <cstddef>
#include <cstdint>

namespace thv::codec
{

// Slot order: Up, Down, Left, Right, A, B, C, D (the game's input reader order).
constexpr int kSlotCount = 8;

std::uint8_t PackWireMask(const std::uint32_t slots[kSlotCount]);
void UnpackWireMask(std::uint8_t mask, std::uint32_t slots[kSlotCount]);

std::uint8_t PackWireMaskFromPlayer(const void* playerBase);
std::uint8_t SampleSdlWireMask();
std::uint8_t SampleSdlWireMaskForPlayer(int playerIndex);

void ApplyWireMaskToPlayer(void* playerBase, std::uint8_t wireMask);

}
