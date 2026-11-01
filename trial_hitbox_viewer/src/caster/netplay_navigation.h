#pragma once

#include <cstdint>

namespace thv::netplay::nav
{

enum class Phase : std::uint8_t
{
    Idle = 0,
    AwaitDeterminism,
    AwaitCasterLaunchGo,
    AwaitTitle,
    EnterVsHuman,
    CharSelectLockstep,
    LoadingSync,
    BattleLockstep,
    PostMatch,
    Active,
};

enum class PacketStage : std::uint8_t
{
    HandshakeDone = 0,
    SeedExchange,
    LaunchGo,
    NavTitle,
    NavCharSelect,
    NavLoading,
    BattleSyncData,
    Quit,
};

struct FlowStats
{
    std::uint32_t seedPacketsSent = 0;
    std::uint32_t seedPacketsRecv = 0;
    std::uint32_t launchGoSent = 0;
    std::uint32_t launchGoRecv = 0;
    std::uint32_t syncPacketsSent = 0;
    std::uint32_t syncPacketsRecv = 0;
    std::uint32_t syncFramesApplied = 0;
    std::uint32_t screenTransitions = 0;
};

const char* PhaseName(Phase phase);
const char* PacketStageName(PacketStage stage);
const char* TrialScreenName(std::uint8_t mode);

void Reset();
void SetPhase(Phase phase);
Phase GetPhase();

void SetPacketStage(PacketStage stage);
PacketStage GetPacketStage();

void OnTrialScreenMode(std::uint8_t prev, std::uint8_t next);
FlowStats GetStats();

void NoteSeedSent();
void NoteSeedRecv();
void NoteLaunchGoSent();
void NoteLaunchGoRecv();
void NoteSyncSent();
void NoteSyncRecv();
void NoteSyncApplied();

}
