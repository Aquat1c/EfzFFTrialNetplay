#pragma once

#include "caster_session.h"
#include "spectator_protocol.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace thv::spectator::client
{

struct Status
{
    bool active = false;
    bool connected = false;
    bool assistedByAutopunch = false;
    bool waitingForMatch = false;
    bool redirected = false;
    bool redirectFallbackUsed = false;
    bool terminal = false;
    bool fatal = false;
    std::uint32_t sessionTag = 0u;
    std::uint32_t relayEpoch = 0u;
    std::uint32_t redirectGrantId = 0u;
    std::uint32_t failedRedirectGrantId = 0u;
    std::uint32_t receivingGameId = 0u;
    std::uint32_t receivingRecordIndex = 0u;
    std::uint32_t playingGameId = 0u;
    std::uint32_t playingRecordIndex = 0u;
    std::uint8_t targetFps = 0u;
    std::size_t queuedGames = 0u;
    std::size_t queuedFrames = 0u;
    std::uint64_t receivedInputDigest = protocol::kInputDigestSeed;
    std::uint64_t playedInputDigest = protocol::kInputDigestSeed;
    protocol::TerminalReason terminalReason = protocol::TerminalReason::SessionEnded;
    std::uint32_t terminalGameId = 0u;
    std::uint32_t terminalRecordIndex = 0u;
    std::uint64_t terminalInputDigest = protocol::kInputDigestSeed;
    std::uint64_t terminalTotalConfirmedFrames = 0u;
    std::string error;
};

bool Initialize(const session::LauncherSession& launcherSession);
void Shutdown();
void Poll();

// Network receive must never mutate the driver's current record.
bool PeekGameStart(protocol::GameStart* outStart);
bool MarkGameStarted(std::uint32_t gameId);
bool PeekFrame(protocol::ConfirmedInput* outFrame);
std::size_t ContiguousFramesForScreen(std::uint8_t screen, std::size_t maximum);
bool ConsumeFrame(std::uint32_t gameId, std::uint32_t recordIndex);
bool AdvanceCompletedGame();

Status GetStatus();

}
