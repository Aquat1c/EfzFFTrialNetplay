#pragma once

#include "caster_session.h"
#include "netplay_lockstep.h"
#include "netplay_navigation.h"

#include <cstdint>

namespace thv::netplay
{

// Call from the mod worker, never a global constructor or DllMain.
bool InitializeSocketRuntime();
int SocketRuntimeError();

bool InitializeFromSession(const session::LauncherSession& session);
void CloseSync();

void Poll();

bool LaunchGoReceived();
void SendLaunchGo();
void SetLaunchGoRetransmit(bool enabled);

bool IsRollbackNegotiated();
bool IsRollbackRequired();
bool IsDeterminismSynced();
// -1 until a LaunchGo has been sent (host) or received (client).
int GetNegotiatedInputDelay();
int GetNegotiatedRollbackFrames();
// -1 until the player contract has been acknowledged/committed.
int GetNegotiatedTargetFps();
bool IsNetplayAborted();
// Bootstrap must not close the process while this is true.
bool IsTerminalHandoffInProgress();
void AbortNetplaySession(const char* reason);
void CompleteHarnessSession();
void SendQuit();
void NotifySpectatorsSourceEnded(bool playerDisconnected);
const char* GetNetplayAbortReason();
std::uint32_t GetSyncedRngSeed();
bool IsSyncTransportActive();
std::uint32_t GetRemoteSyncFrontier();
// Advances at confirmed battle exits and agreed entry commits.
std::uint32_t GetSyncGeneration();
// A fresh generation stops delayed prior-screen packets mutating the new epoch.
void AdvanceSyncTimeline();

void SendLocalSyncFrame();

void SendFrameHash(std::uint32_t frame, const std::uint32_t* regionHashes, int count);

// epoch: UINT32_MAX = round-1 armed handoff, transport-owned generation otherwise.
void SendBattleEntry(std::uint32_t frame, bool ready, std::uint32_t epoch, bool force = false);
bool GetPeerBattleEntry(std::uint32_t* outFrame, bool* outReady, std::uint32_t epoch);
void CommitBattleEntry(std::uint32_t frame, std::uint32_t epoch);
void ResetBattleEntryExchange();
nav::FlowStats GetPacketFlowStats();
lockstep::Stats GetLockstepStats();

void LogDiagnosticHeartbeat();

}
