#pragma once

#include <cstdint>

namespace thv::telemetry
{

struct PlayerInputSnapshot
{
    int side = 0;
    std::uint8_t charId = 0;
    int cpuFlag = 0;
    std::int8_t facing = 0;
    std::uint8_t horiz = 0;
    std::uint8_t vert = 0;
    std::uint8_t buttonA = 0;
    std::uint8_t buttonB = 0;
    std::uint8_t buttonC = 0;
    std::uint8_t buttonD = 0;
    std::uint16_t historyIndex = 0;
    std::uint16_t lastPackedWord = 0;
    std::uint8_t wireMask = 0;
};

struct RuntimeSnapshot
{
    std::uint8_t screenMode = 0;
    std::uint32_t screenSubState = 0;
    bool diActive = false;
    bool battleActive = false;
    int humanSide = 0;
    bool framestepPaused = false;
    std::uint64_t localFrame = 0;
    std::uint16_t fpuCapturedCw = 0;
    std::uint16_t fpuLastObservedCw = 0;
    std::uint32_t fpuMismatchCount = 0;
    bool rngIatRand = false;
    bool rngIatSrand = false;
    bool rngStaticRand = false;
    bool rngStaticSrand = false;
    std::uint32_t rngShadow = 0;
    std::uint32_t rngLastSeed = 0;
    std::uint32_t rngSrandCalls = 0;
    std::uint32_t rngRandCalls = 0;
    int saveSlot = 0;
    unsigned int saveCount = 0;
    unsigned int loadCount = 0;
    PlayerInputSnapshot p1{};
    PlayerInputSnapshot p2{};
};

bool Initialize();
void Shutdown();

void PollHotkeys();
void OnBattleFrameBegin();
void OnInputPolled(void* player, unsigned int requestedIndex, unsigned int effectiveIndex);

bool VerboseInputLogging();
void SetVerboseInputLogging(bool enabled);
void DumpFullSnapshot(const char* reason);

const char* ScreenModeName(std::uint8_t mode);
std::uint8_t PackWireMaskFromPlayer(const void* playerBase);
bool ReadPlayerInputSnapshot(int side, PlayerInputSnapshot* out);
RuntimeSnapshot CaptureRuntimeSnapshot();

}
