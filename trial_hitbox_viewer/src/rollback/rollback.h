#pragma once

#include "rollback_config.h"
#include "rollback_types.h"
#include "trial_protocol.h"

#include <windows.h>

namespace thv::rollback
{

bool IsEnabled();
void SetEnabled(bool enabled);

bool IsArmedForNetplay();
void ArmForNetplay();
void DisarmNetplay();
// Must be armed; call before the loading tick simulates the first intro frame.
void StartNetplaySession();
bool IsBattleIntroBoundaryPending();
bool ShouldDriveFrontendTick();
bool ShouldDriveBattleTick();
// Commit before the native char-select transition loads BGxx.DAT.
void PrepareStageForNativeLoad();

bool Initialize();
void Shutdown();

void OnModTick();
void RunBattleTick(HWND gameWindow = nullptr);
void RunFrontendRollbackTick(HWND gameWindow = nullptr);
void OnBattleFrameBoundary(HWND gameWindow = nullptr);

void SetInputDelay(int frames);
void SetPingMs(std::uint32_t pingMs);
void SetPredictionWindow(int frames);
void SetLocalSide(int side);
void SetRngSeed(std::uint32_t seed);

void PushLocalInput(std::uint8_t wireMask);
void PushRemoteInput(std::uint8_t wireMask);

void SetHostCallbacks(
    int(__cdecl* advanceBattleTick)(HWND hWnd),
    int(__cdecl* advanceLoadingTick)(HWND hWnd),
    int(__cdecl* advanceCharSelectTick)(HWND hWnd),
    int(__cdecl* advanceMenuTick)(HWND hWnd),
    int(__cdecl* advanceTitleTick)(HWND hWnd),
    HWND gameWindow);

Status GetStatus();

int GetMemorialScreenIndex();
int MapMemorialScreenIndexFromGame();
bool IsPostMatchScreen();
int GetLastMainTickIterations();

bool IsInputAuthorityActive();
bool ShouldEngineDriveBattleTick();
bool TryGetWireMaskForPlayerSide(int playerSide, std::uint8_t* outMask);
std::uint8_t GetMergedSystemState();
void AcknowledgeSystemStateApplied(std::uint8_t state);

void FillSyncDataPacket(proto::SyncDataPacket* out);
bool ApplySyncDataPacket(const proto::SyncDataPacket& packet);

}
