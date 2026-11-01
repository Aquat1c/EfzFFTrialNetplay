#pragma once

#include <cstdint>

namespace thv
{

constexpr std::uint8_t kLayerHit = 1u;
constexpr std::uint8_t kLayerHurt = 2u;
constexpr std::uint8_t kLayerCollision = 4u;
constexpr std::uint8_t kAllCollisionLayers = kLayerHit | kLayerHurt | kLayerCollision;

bool Initialize();
void Shutdown();

void PollHotkeys();

std::uint8_t GetLayerMask();
void SetLayerMask(std::uint8_t mask);
void ToggleLayer(std::uint8_t bit);
void SetDebugDrawFlag(bool enabled);
void ApplyDebugPaletteColors();

int GetLayerPaletteIndex(std::uint8_t layerBit);
void SetLayerPaletteIndex(std::uint8_t layerBit, int index);

void DrawPaletteDebugPanel();

void DrawPlayerCollisionBoxes(int player, int camX, int camY, std::uint8_t layerMask);

}
