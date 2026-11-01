#pragma once

#include "trial_protocol.h"

#include <cstdint>
#include <vector>

namespace thv::netplay::lockstep
{

struct Stats
{
    std::uint32_t localFrames = 0;
    std::uint32_t remoteFrames = 0;
    std::uint32_t simFrame = 0;
    std::uint32_t packetsApplied = 0;
    std::uint32_t stallCount = 0;
    std::uint32_t lastRemoteLead = 0;
    std::uint32_t effectiveDelay = 0;
    std::uint32_t peerAckFrame = 0;
};

enum class HandoffBlockReason : std::uint8_t
{
    None = 0,
    EmptyHistory,
    MissingFrameInputs,
    WaitRemoteStalled,
    AsymmetricHistory,
};

struct HandoffStatus
{
    bool ready = false;
    HandoffBlockReason blockReason = HandoffBlockReason::None;
    std::uint32_t simFrame = 0;
    std::size_t localSize = 0;
    std::size_t remoteSize = 0;
    bool hasFrameInputs = false;
    bool waitStalled = false;
    int waitIterations = 0;
    int needRemote = -1; // localLen - delay - 1; -1 during delay warmup
    int effectiveDelay = 0;
};

const char* HandoffBlockReasonName(HandoffBlockReason reason);
HandoffStatus EvaluateHandoff();
void LogHandoffTransition(const HandoffStatus& status);

void Reset();
void ResetFrontendEpoch(const char* reason);
void OnScreenTransition(std::uint8_t prevMode, std::uint8_t nextMode);
bool HasWireInputsForFrame(std::uint32_t frame);
bool IsWaitRemoteStalled();
bool IsHandoffReady();
void AlignForBattleHandoff();
void TrimForBattleEntry(std::uint32_t frame);

void SetPingMs(std::uint32_t pingMs);
std::uint32_t GetPingMs();
std::uint32_t GetWaitLoopDelayFrames();
std::uint32_t GetLocalHistorySize();
std::uint32_t GetRemoteHistorySize();
std::uint32_t GetPeerAckFrame();

void OnLocalInput(const proto::PlayerInputRecord& record);
void FillOutgoingPacket(proto::SyncDataPacket* out);
bool ApplyIncomingPacket(const proto::SyncDataPacket& packet);

// 0 = stall; >0 = may capture local input this tick.
int WaitRemoteIterations();
// Post-capture sim steps; 0 = append only.
int SimIterationsAfterCapture(int preLocalSize);

bool GetRemoteInput(proto::PlayerInputRecord* out);
bool GetLocalInput(proto::PlayerInputRecord* out);
std::uint32_t ActiveWireFrameIndex();
std::uint8_t GetMergedSystemState();

void ExportWireHistories(std::vector<std::uint8_t>* localOut, std::vector<std::uint8_t>* remoteOut);
std::uint32_t GetSimFrame();

void NotifySimAdvanced();
void BeginSpectatorGame(std::uint32_t sourceFrame);

bool IsWaitLoopWarmup();

Stats GetStats();

}
