#pragma once

#include <SDL3/SDL_scancode.h>

#include <cstddef>
#include <cstdint>

namespace thv::input
{

constexpr int kFightSlotCount = 8;

bool Initialize();
void Shutdown();

void Poll();

// SDL requires event pumping on the thread that owns the game window.
void PumpControllerEventsOnGameThread();

void SyncKeyboardFromSdl();

bool IsDown(SDL_Scancode scancode);
bool WasPressed(SDL_Scancode scancode);
bool IsDownNow(SDL_Scancode scancode);

// Slots: Up, Down, Left, Right, A, B, C, D.
bool IsSlotDownForPlayer(int slot, int playerIndex);

bool IsGamepadSlotDown(int slot);
bool IsGamepadSlotDownForPlayer(int playerIndex, int slot);

// Never combines keyboard and controller.
bool IsConfiguredSlotDownForPlayer(int playerIndex, int slot);

void FillDiKeyboardBuffer(std::uint8_t* out256, int playerIndex);

void FillDiKeyboardBufferBothPlayers(std::uint8_t* out256);

void MergeLocalNetplayBindingsIntoDiBuffer(std::uint8_t* out256);

void FillDiKeyboardBufferNetplay(
    std::uint8_t* out256,
    std::uint8_t localWireMask,
    std::uint8_t remoteWireMask,
    bool hasRemoteInput);

void OverlayDiKeyboardBuffer(std::uint8_t* out256, int playerIndex);
void OverlayDiKeyboardBufferBothPlayers(std::uint8_t* out256);
void ReplacePlayerInDiBuffer(
    std::uint8_t* out256,
    int playerIndex,
    std::uint8_t selectedWireMask);
void ReplaceBothPlayersInDiBuffer(
    std::uint8_t* out256,
    std::uint8_t p1SelectedWireMask,
    std::uint8_t p2SelectedWireMask);
void OverlayDiKeyboardBufferNetplay(
    std::uint8_t* out256,
    std::uint8_t localWireMask,
    std::uint8_t remoteWireMask,
    bool hasRemoteInput);

void MergeWireMaskIntoDiBuffer(std::uint8_t* out256, int playerIndex, std::uint8_t wireMask);

// F4/F5 are applied at the frame boundary, not injected here.
void SetMergedSystemState(std::uint8_t mergedState);
void ApplySystemStateToDiBuffer(std::uint8_t* out256);
void ApplyOfflinePracticeBindingsToDiBuffer(std::uint8_t* out256);

bool HasGamepad();
const char* GamepadName();

}
