#pragma once

#include <cstdint>

namespace thv
{

enum class SavestateBank : std::uint8_t
{
    Practice = 0,
    Netplay = 1,
    Debug = 2,
};

constexpr int kPracticeMinSlot = 0;
constexpr int kPracticeMaxSlot = 7;
constexpr int kNetplayMinSlot = 0;
constexpr int kNetplayMaxSlot = 7;

bool InitializeSavestate();
void ShutdownSavestate();

void PollSavestateHotkeys();

// Called from the battle-tick hook at frame boundaries.
void ProcessSavestateAtFrameBoundary();

bool SaveState(SavestateBank bank, int slot);
bool LoadState(SavestateBank bank, int slot);

int GetActivePracticeSlot();
bool HasPracticeSlot(int slot);
bool HasDebugSavestate();

unsigned int GetSavestateCount();
unsigned int GetLoadstateCount();

}
