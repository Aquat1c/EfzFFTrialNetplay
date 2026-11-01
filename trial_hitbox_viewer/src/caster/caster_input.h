#pragma once

#include "trial_protocol.h"

#include <cstdint>

namespace thv::caster
{

enum class TransportMode : std::uint8_t
{
    Offline = 0,
    LocalLoopback = 1,
    Netplay = 2,
    Spectator = 3,
};

bool Initialize();
void Shutdown();

void OnInputTick();

void OnBattleFrameInput();
void OnNetplayPollFrame();
void OnScreenInputFrame();
void OnDiInputFrame();

bool IsBridgeActive();
TransportMode GetTransportMode();
void SetTransportMode(TransportMode mode);

bool IsNetplayActive();
bool IsSpectatorActive();

int GetLocalSide();
int GetRemoteSide();
void SetLocalSide(int side);

int LocalBindingPlayerIndex();

std::uint8_t GetLocalWireMask();
std::uint8_t SampleSyncWireMaskForCapture();
std::uint8_t GetRemoteWireMask();
bool HasRemoteInput();

proto::PlayerInputRecord GetLocalRecord();
proto::PlayerInputRecord GetRemoteRecord();

const proto::InputQueue& GetLocalQueue();

void PushRemoteRecord(const proto::PlayerInputRecord& record);
void RefreshLockstepWireInject();
void RefreshNetplayWireInject();
void EnqueueLocalForSync(std::uint32_t frame);

bool TryGetWireMaskForPlayerSide(int playerSide, std::uint8_t* outMask);

void SetSpectatorInputWords(std::uint16_t p1Word, std::uint16_t p2Word);

}
