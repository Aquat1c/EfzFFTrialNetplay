#include "spectator_checkpoint.h"
#include "spectator_player.h"
#include "spectator_relay.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace thv::netplay
{
std::uint32_t GetSyncedRngSeed() { return 0x13572468u; }
}

namespace thv::det
{
std::uint16_t GetCapturedFpuCw() { return 0x027Fu; }
}

namespace thv::log
{
bool EnableFileLogging() { return true; }
void Info(const char*, const char*, ...) {}
void Error(const char*, const char*, ...) {}
}

namespace
{

thv::spectator::Checkpoint MakeCheckpoint()
{
    thv::spectator::Checkpoint checkpoint{};
    checkpoint.generation = 17u;
    checkpoint.frame = 12345u;
    checkpoint.simFrame = 54321u;
    checkpoint.rngShadow = 0x13572468u;
    checkpoint.visualRngShadow = 0x24681357u;
    checkpoint.p1CharacterId = 2u;
    checkpoint.p2CharacterId = 7u;
    checkpoint.stageId = 4u;
    checkpoint.battle.resize(trial::kDynamicBattleSize);
    checkpoint.visualEffects.resize(trial::kEffectDynamicStateSize);
    for (std::size_t i = 0; i < checkpoint.battle.size(); ++i)
    {
        checkpoint.battle[i] = static_cast<std::uint8_t>((i * 37u + 11u) & 0xFFu);
    }
    for (std::size_t i = 0; i < checkpoint.visualEffects.size(); ++i)
    {
        checkpoint.visualEffects[i] = static_cast<std::uint8_t>((i * 13u + 5u) & 0xFFu);
    }
    for (std::size_t i = 0; i < checkpoint.fpu.size(); ++i)
    {
        checkpoint.fpu[i] = static_cast<std::uint8_t>((i * 3u + 1u) & 0xFFu);
    }
    for (std::size_t i = 0; i < checkpoint.shakeAux.size(); ++i)
    {
        checkpoint.shakeAux[i] = static_cast<std::uint8_t>((i * 9u + 2u) & 0xFFu);
    }
    std::string error;
    if (!thv::spectator::ComputeCheckpointRegionHashes(
            checkpoint,
            &checkpoint.regionHashes,
            &error))
    {
        return {};
    }
    return checkpoint;
}

bool Equal(const thv::spectator::Checkpoint& left, const thv::spectator::Checkpoint& right)
{
    return left.generation == right.generation
        && left.frame == right.frame
        && left.simFrame == right.simFrame
        && left.rngShadow == right.rngShadow
        && left.visualRngShadow == right.visualRngShadow
        && left.p1CharacterId == right.p1CharacterId
        && left.p2CharacterId == right.p2CharacterId
        && left.stageId == right.stageId
        && left.fpu == right.fpu
        && left.shakeAux == right.shakeAux
        && left.battle == right.battle
        && left.visualEffects == right.visualEffects
        && left.regionHashes == right.regionHashes;
}

}

int main()
{
    using namespace thv::spectator;

    if (!protocol::MatchesTargetFpsContract(0u, 60u)
        || !protocol::MatchesTargetFpsContract(0u, 64u)
        || !protocol::MatchesTargetFpsContract(64u, 64u)
        || protocol::MatchesTargetFpsContract(64u, 60u)
        || protocol::MatchesTargetFpsContract(0u, 0u)
        || !player::IsHiddenCatchUpTick(0u, 4u)
        || !player::IsHiddenCatchUpTick(2u, 4u)
        || player::IsHiddenCatchUpTick(3u, 4u)
        || player::IsHiddenCatchUpTick(0u, 1u))
    {
        return 57;
    }

    const std::vector<std::uint8_t> localSystem{0x01u, 0x08u, 0x00u, 0x10u, 0x08u};
    const std::vector<std::uint8_t> remoteSystem{0x00u, 0x10u, 0x08u, 0x00u, 0x10u};
    constexpr std::uint32_t sourceFrame = 4u;
    if (ConfirmedSystemStateForCanonicalSideAt(
            localSystem, remoteSystem, 0, 0, sourceFrame) != localSystem[sourceFrame]
        || ConfirmedSystemStateForCanonicalSideAt(
            localSystem, remoteSystem, 0, 1, sourceFrame) != remoteSystem[sourceFrame]
        || ConfirmedSystemStateForCanonicalSideAt(
            localSystem, remoteSystem, 1, 0, sourceFrame) != remoteSystem[sourceFrame]
        || ConfirmedSystemStateForCanonicalSideAt(
            localSystem, remoteSystem, 1, 1, sourceFrame) != localSystem[sourceFrame])
    {
        return 53;
    }

    Checkpoint source = MakeCheckpoint();
    std::string error;
    if (source.battle.empty() || !ValidateCheckpoint(source, &error))
    {
        return 1;
    }

    std::vector<std::uint8_t> encoded;
    if (!SerializeCheckpoint(source, &encoded, &error) || encoded.empty())
    {
        return 2;
    }
    Checkpoint decoded{};
    if (!DeserializeCheckpoint(encoded.data(), encoded.size(), &decoded, &error)
        || !Equal(source, decoded))
    {
        return 3;
    }

    protocol::Bootstrap bootstrap{};
    std::vector<protocol::BootstrapChunk> chunks;
    if (!BuildBootstrapTransfer(source, 0xAABBCCDDu, 100u, 2u, 12u, 64u,
            &bootstrap, &chunks, &error)
        || chunks.size() < 2u
        || bootstrap.chunkCount != chunks.size()
        || bootstrap.targetFps != 64u)
    {
        return 4;
    }
    std::reverse(chunks.begin(), chunks.end());
    Checkpoint reassembled{};
    if (!ReassembleBootstrapTransfer(bootstrap, chunks, &reassembled, &error)
        || !Equal(source, reassembled))
    {
        return 5;
    }
    protocol::Bootstrap invalidRateBootstrap = bootstrap;
    invalidRateBootstrap.targetFps = 0u;
    if (ReassembleBootstrapTransfer(invalidRateBootstrap, chunks, &reassembled, &error)
        || BuildBootstrapTransfer(
            source, 0xAABBCCDDu, 100u, 2u, 12u, 0u,
            &invalidRateBootstrap, &chunks, &error))
    {
        return 54;
    }

    auto corruptChunks = chunks;
    corruptChunks.front().payload[0] ^= 0x80u;
    if (ReassembleBootstrapTransfer(bootstrap, corruptChunks, &reassembled, &error))
    {
        return 6;
    }
    auto missingChunks = chunks;
    missingChunks.pop_back();
    if (ReassembleBootstrapTransfer(bootstrap, missingChunks, &reassembled, &error))
    {
        return 7;
    }

    source.regionHashes[0] ^= 1u;
    if (ValidateCheckpoint(source, &error))
    {
        return 8;
    }

    source = MakeCheckpoint();
    thv::session::LauncherSession hostSession{};
    hostSession.mode = thv::session::LauncherMode::Host;
    hostSession.isHost = true;
    hostSession.inputDelay = 2u;
    hostSession.rollbackFrames = 12u;
    hostSession.targetFps = 64u;

    BeginHostArchive(hostSession);
    GameStartInfo duplicateStart{};
    duplicateStart.sourceGeneration = source.generation;
    duplicateStart.sourceFrame = source.frame;
    duplicateStart.rngSeed = source.rngShadow;
    duplicateStart.frontendRngState = 0x10203040u;
    duplicateStart.frontendVisualRngState = 0x50607080u;
    duplicateStart.screen = trial::kScreenCharSelect;
    duplicateStart.p1CharacterId = source.p1CharacterId;
    duplicateStart.p2CharacterId = source.p2CharacterId;
    duplicateStart.p1PaletteId = 1u;
    duplicateStart.p2PaletteId = 4u;
    duplicateStart.stageId = source.stageId;
    if (!BeginGame(duplicateStart) || !BeginGame(duplicateStart))
    {
        return 38;
    }

    const auto rejectConflictingStart = [&](const GameStartInfo& conflicting) {
        return !BeginGame(conflicting)
            && GetStatus().retainedGames == 1u
            && GetStatus().gameId == 1u;
    };
    GameStartInfo conflictingStart = duplicateStart;
    conflictingStart.rngSeed ^= 1u;
    if (!rejectConflictingStart(conflictingStart))
    {
        return 39;
    }
    conflictingStart = duplicateStart;
    conflictingStart.frontendRngState ^= 1u;
    if (!rejectConflictingStart(conflictingStart))
    {
        return 40;
    }
    conflictingStart = duplicateStart;
    conflictingStart.frontendVisualRngState ^= 1u;
    if (!rejectConflictingStart(conflictingStart))
    {
        return 41;
    }
    conflictingStart = duplicateStart;
    conflictingStart.screen ^= 1u;
    if (!rejectConflictingStart(conflictingStart))
    {
        return 42;
    }
    conflictingStart = duplicateStart;
    conflictingStart.p1CharacterId ^= 1u;
    if (!rejectConflictingStart(conflictingStart))
    {
        return 43;
    }
    conflictingStart = duplicateStart;
    conflictingStart.p2CharacterId ^= 1u;
    if (!rejectConflictingStart(conflictingStart))
    {
        return 44;
    }
    conflictingStart = duplicateStart;
    conflictingStart.p1PaletteId ^= 1u;
    if (!rejectConflictingStart(conflictingStart))
    {
        return 51;
    }
    conflictingStart = duplicateStart;
    conflictingStart.p2PaletteId ^= 1u;
    if (!rejectConflictingStart(conflictingStart))
    {
        return 52;
    }
    conflictingStart = duplicateStart;
    conflictingStart.stageId ^= 1u;
    if (!rejectConflictingStart(conflictingStart) || !BeginGame(duplicateStart))
    {
        return 45;
    }

    const std::uint16_t duplicateP1 = PackInputWord(0x12u, 0x34u);
    const std::uint16_t duplicateP2 = PackInputWord(0x56u, 0x78u);
    if (!PublishConfirmedFrame(
            duplicateStart.sourceGeneration,
            duplicateStart.sourceFrame,
            trial::kScreenBattle,
            duplicateP1,
            duplicateP2,
            source.regionHashes.data(),
            source.regionHashes.size())
        || !PublishConfirmedFrame(
            duplicateStart.sourceGeneration,
            duplicateStart.sourceFrame,
            trial::kScreenBattle,
            duplicateP1,
            duplicateP2,
            source.regionHashes.data(),
            source.regionHashes.size()))
    {
        return 46;
    }

    auto conflictingHashes = source.regionHashes;
    conflictingHashes[0] ^= 1u;
    if (PublishConfirmedFrame(
            duplicateStart.sourceGeneration,
            duplicateStart.sourceFrame,
            static_cast<std::uint8_t>(trial::kScreenBattle + 1u),
            duplicateP1,
            duplicateP2,
            source.regionHashes.data(),
            source.regionHashes.size())
        || PublishConfirmedFrame(
            duplicateStart.sourceGeneration,
            duplicateStart.sourceFrame,
            trial::kScreenBattle,
            duplicateP1,
            duplicateP2,
            conflictingHashes.data(),
            conflictingHashes.size()))
    {
        return 47;
    }

    const std::uint32_t shapeFrame = duplicateStart.sourceFrame + 1u;
    if (PublishConfirmedFrame(
            duplicateStart.sourceGeneration,
            shapeFrame,
            trial::kScreenBattle,
            duplicateP1,
            duplicateP2,
            nullptr,
            source.regionHashes.size())
        || PublishConfirmedFrame(
            duplicateStart.sourceGeneration,
            shapeFrame,
            trial::kScreenBattle,
            duplicateP1,
            duplicateP2,
            source.regionHashes.data(),
            0u)
        || PublishConfirmedFrame(
            duplicateStart.sourceGeneration,
            shapeFrame,
            trial::kScreenBattle,
            duplicateP1,
            duplicateP2,
            source.regionHashes.data(),
            source.regionHashes.size() - 1u)
        || !PublishConfirmedFrame(
            duplicateStart.sourceGeneration,
            shapeFrame,
            trial::kScreenBattle,
            duplicateP1,
            duplicateP2,
            nullptr,
            0u))
    {
        return 48;
    }

    std::array<std::uint32_t, thv::proto::kFrameHashRegions> zeroHashes{};
    const std::uint32_t zeroHashFrame = shapeFrame + 1u;
    if (!PublishConfirmedFrame(
            duplicateStart.sourceGeneration,
            zeroHashFrame,
            trial::kScreenBattle,
            duplicateP1,
            duplicateP2,
            zeroHashes.data(),
            zeroHashes.size())
        || !PublishConfirmedFrame(
            duplicateStart.sourceGeneration,
            zeroHashFrame,
            trial::kScreenBattle,
            duplicateP1,
            duplicateP2,
            zeroHashes.data(),
            zeroHashes.size()))
    {
        return 49;
    }
    const Status duplicateStatus = GetStatus();
    if (duplicateStatus.retainedGames != 1u
        || duplicateStatus.confirmedFrames != 3u
        || duplicateStatus.rejectedGaps != 12u
        || duplicateStatus.duplicateFrames != 4u)
    {
        return 50;
    }
    EndSession();

    thv::session::LauncherSession clientPreference = hostSession;
    clientPreference.mode = thv::session::LauncherMode::Client;
    clientPreference.isHost = false;
    clientPreference.rollbackFrames = 3u;
    BeginHostArchive(clientPreference);
    if (!CommitNegotiatedRollbackFrames(12u)
        || GetStatus().rollbackFrames != 12u
        || !BeginGame(duplicateStart))
    {
        return 97;
    }
    protocol::GameStart negotiatedStart{};
    if (!BuildGameStartMessage(0u, 0xAA55AA55u, 1u, &negotiatedStart)
        || negotiatedStart.rollbackFrames != 12u
        || CommitNegotiatedRollbackFrames(11u))
    {
        return 98;
    }
    EndSession();

    BeginHostArchive(hostSession);

    GameStartInfo gameStart{};
    gameStart.sourceGeneration = source.generation;
    gameStart.sourceFrame = source.frame;
    gameStart.rngSeed = source.rngShadow;
    gameStart.screen = trial::kScreenCharSelect;
    gameStart.p1CharacterId = source.p1CharacterId;
    gameStart.p2CharacterId = source.p2CharacterId;
    gameStart.p1PaletteId = 2u;
    gameStart.p2PaletteId = 5u;
    gameStart.stageId = source.stageId;
    if (!BeginGame(gameStart))
    {
        return 9;
    }
    if (!NeedsCheckpoint(source.generation, source.frame)
        || !PublishCheckpoint(source)
        || NeedsCheckpoint(source.generation, source.frame))
    {
        return 10;
    }
    protocol::Bootstrap cachedBootstrap{};
    std::vector<protocol::BootstrapChunk> cachedChunks;
    if (!BuildLatestBootstrapTransfer(
            0x01020304u,
            500u,
            &cachedBootstrap,
            &cachedChunks)
        || cachedBootstrap.inputDelay != 2u
        || cachedBootstrap.rollbackFrames != 12u
        || cachedBootstrap.targetFps != 64u)
    {
        return 11;
    }
    protocol::GameStart wireStart{};
    if (!BuildGameStartMessage(0u, 0x01020304u, 600u, &wireStart)
        || wireStart.header.kind != protocol::MessageKind::GameStart
        || wireStart.sourceGeneration != gameStart.sourceGeneration
        || wireStart.sourceFrame != gameStart.sourceFrame
        || wireStart.rngSeed != gameStart.rngSeed
        || wireStart.inputDelay != 2u
        || wireStart.rollbackFrames != 12u
        || wireStart.targetFps != 64u
        || wireStart.p1CharacterId != gameStart.p1CharacterId
        || wireStart.p2CharacterId != gameStart.p2CharacterId
        || wireStart.p1PaletteId != gameStart.p1PaletteId
        || wireStart.p2PaletteId != gameStart.p2PaletteId
        || wireStart.stageId != gameStart.stageId)
    {
        return 12;
    }
    const std::uint64_t startFingerprint = protocol::GameStartFingerprint(wireStart);
    protocol::GameStart retransmittedStart = wireStart;
    retransmittedStart.header.sequence += 17u;
    retransmittedStart.header.sessionTag ^= 0xFFFFFFFFu;
    if (protocol::GameStartFingerprint(retransmittedStart) != startFingerprint)
    {
        return 34;
    }
    retransmittedStart.p1PaletteId ^= 1u;
    if (protocol::GameStartFingerprint(retransmittedStart) == startFingerprint)
    {
        return 35;
    }
    retransmittedStart = wireStart;
    retransmittedStart.p2PaletteId ^= 1u;
    if (protocol::GameStartFingerprint(retransmittedStart) == startFingerprint)
    {
        return 53;
    }
    retransmittedStart = wireStart;
    retransmittedStart.targetFps = 60u;
    if (protocol::GameStartFingerprint(retransmittedStart) == startFingerprint)
    {
        return 55;
    }
    protocol::Hello hello{};
    hello.requestedGameId = wireStart.gameId;
    hello.nextRecordIndex = 0u;
    hello.clientNonce = 0xCAFEBABEu;
    hello.relayEpoch = 0x10203040u;
    hello.grantId = 0x55667788u;
    hello.token[0] = 0xA5u;
    protocol::Redirect redirect{};
    redirect.clientNonce = hello.clientNonce;
    redirect.relayEpoch = hello.relayEpoch;
    redirect.grantId = hello.grantId;
    redirect.requestedGameId = hello.requestedGameId;
    redirect.nextRecordIndex = hello.nextRecordIndex;
    redirect.expectedDigest = protocol::kInputDigestSeed;
    redirect.targetIpv4 = 0x0100007Fu;
    redirect.targetInternalPort = 10800u;
    redirect.hop = 1u;
    redirect.token = hello.token;
    protocol::Ack ack{};
    ack.header.sessionTag = 0x01020304u;
    ack.gameId = wireStart.gameId;
    protocol::KeepAlive keepAlive{};
    keepAlive.gameId = wireStart.gameId;
    protocol::Detach detach{};
    protocol::Reject reject{};
    protocol::InitAck initAck{};
    protocol::Terminal terminal{};
    protocol::TerminalAck terminalAck{};
    protocol::Waiting waiting{};
    if (hello.header.kind != protocol::MessageKind::Hello
        || hello.simulationContractId != protocol::kSimulationContractId
        || redirect.header.kind != protocol::MessageKind::Redirect
        || redirect.clientNonce != hello.clientNonce
        || redirect.relayEpoch != hello.relayEpoch
        || redirect.grantId != hello.grantId
        || redirect.hop != 1u
        || redirect.token != hello.token
        || ack.header.kind != protocol::MessageKind::Ack
        || keepAlive.header.kind != protocol::MessageKind::KeepAlive
        || detach.header.kind != protocol::MessageKind::Detach
        || reject.header.kind != protocol::MessageKind::Reject
        || reject.serverVersion != protocol::kVersion
        || initAck.header.kind != protocol::MessageKind::InitAck
        || terminal.header.kind != protocol::MessageKind::Terminal
        || terminal.finalInputDigest != protocol::kInputDigestSeed
        || terminalAck.header.kind != protocol::MessageKind::TerminalAck
        || waiting.header.kind != protocol::MessageKind::Waiting)
    {
        return 28;
    }
    std::array<std::uint32_t, thv::proto::kFrameHashRegions> digestHashes{};
    digestHashes[0] = 0x11223344u;
    digestHashes.back() = 0x55667788u;
    const std::uint64_t authenticatedDigest = protocol::AdvanceInputDigest(
        protocol::kInputDigestSeed,
        7u,
        9u,
        11u,
        13u,
        trial::kScreenBattle,
        0x1234u,
        0x5678u,
        1u,
        digestHashes);
    auto changedHashes = digestHashes;
    changedHashes.back() ^= 1u;
    if (authenticatedDigest == protocol::AdvanceInputDigest(
            protocol::kInputDigestSeed,
            7u,
            9u,
            11u,
            13u,
            trial::kScreenBattle,
            0x1234u,
            0x5678u,
            1u,
            changedHashes)
        || authenticatedDigest == protocol::AdvanceInputDigest(
            protocol::kInputDigestSeed,
            7u,
            9u,
            11u,
            13u,
            trial::kScreenBattle,
            0x1234u,
            0x5678u,
            0u,
            digestHashes)
        || !protocol::HasCanonicalRegionHashes(1u, digestHashes)
        || protocol::HasCanonicalRegionHashes(0u, digestHashes)
        || protocol::HasCanonicalRegionHashes(2u, digestHashes))
    {
        return 54;
    }
    std::array<std::uint32_t, thv::proto::kFrameHashRegions> emptyHashes{};
    if (!protocol::HasCanonicalRegionHashes(0u, emptyHashes))
    {
        return 55;
    }
    for (std::uint32_t i = 0u; i < 40u; ++i)
    {
        if (!PublishConfirmedFrame(
                source.generation,
                source.frame + i,
                trial::kScreenBattle,
                PackInputWord(static_cast<std::uint8_t>(i), 0x10u),
                PackInputWord(static_cast<std::uint8_t>(i + 1u), 0x20u),
                source.regionHashes.data(),
                source.regionHashes.size()))
        {
            return 13;
        }
    }
    const std::uint32_t firstGameId = GetStatus().gameId;
    GameExtent firstExtent{};
    if (!GetGameExtent(firstGameId, &firstExtent)
        || firstExtent.frameCount != 40u
        || firstExtent.hasNextGame)
    {
        return 29;
    }
    protocol::ConfirmedInputBatch batch{};
    if (!BuildConfirmedInputBatch(
            firstGameId,
            0u,
            0x01020304u,
            700u,
            &batch)
        || batch.count != protocol::kBatchFrames
        || batch.gameId != firstGameId
        || batch.frames[0].recordIndex != 0u
        || batch.frames[0].sourceFrame != source.frame
        || batch.frames[0].p1Word != PackInputWord(0u, 0x10u)
        || batch.frames[0].p2Word != PackInputWord(1u, 0x20u)
        || batch.frames[protocol::kBatchFrames - 1u].recordIndex
            != protocol::kBatchFrames - 1u
        || batch.frames[protocol::kBatchFrames - 1u].sourceFrame
            != source.frame + protocol::kBatchFrames - 1u
        || protocol::ConfirmedInputBatchWireSize(batch.count) > 1200u
        || protocol::ConfirmedInputBatchWireSize(batch.count)
            != offsetof(protocol::ConfirmedInputBatch, frames)
                + batch.count * sizeof(protocol::ConfirmedInput)
        || BuildConfirmedInputBatch(firstGameId, 40u, 1u, 1u, &batch))
    {
        return 14;
    }
    const auto tail = CopyFrom(firstGameId, 32u, 8u);
    if (tail.size() != 8u || tail.front().recordIndex != 32u
        || tail.back().recordIndex != 39u)
    {
        return 15;
    }

    if (!PublishConfirmedFrame(
            source.generation,
            source.frame + 39u,
            trial::kScreenBattle,
            PackInputWord(39u, 0x10u),
            PackInputWord(40u, 0x20u),
            source.regionHashes.data(),
            source.regionHashes.size())
        || PublishConfirmedFrame(
            source.generation,
            source.frame + 41u,
            trial::kScreenBattle,
            PackInputWord(41u, 0x10u),
            PackInputWord(42u, 0x20u),
            source.regionHashes.data(),
            source.regionHashes.size()))
    {
        return 16;
    }
    const Status status = GetStatus();
    if (!status.gameStartAvailable || !status.gameActive
        || status.checkpointFrame != source.frame
        || status.confirmedFrames != 40u
        || status.duplicateFrames != 1u
        || status.rejectedGaps != 1u)
    {
        return 17;
    }

    GameStartInfo nextStart = gameStart;
    nextStart.sourceGeneration += 1u;
    nextStart.sourceFrame += 1000u;
    if (!BeginGame(nextStart))
    {
        return 18;
    }
    const Status nextStatus = GetStatus();
    if (!GetGameExtent(firstGameId, &firstExtent)
        || firstExtent.frameCount != 40u
        || !firstExtent.hasNextGame)
    {
        return 30;
    }
    if (nextStatus.gameId == firstGameId || nextStatus.confirmedFrames != 0u
        || nextStatus.retainedGames != 2u
        || !BuildConfirmedInputBatch(firstGameId, 0u, 1u, 1u, &batch))
    {
        return 19;
    }

    constexpr std::uint32_t kLongTailFrames = 8192u;
    for (std::uint32_t i = 0u; i < kLongTailFrames; ++i)
    {
        const std::uint32_t generation = nextStart.sourceGeneration + i / 997u;
        if (!PublishConfirmedFrame(
                generation,
                nextStart.sourceFrame + i,
                i < 120u ? trial::kScreenLoading : trial::kScreenBattle,
                PackInputWord(static_cast<std::uint8_t>(i), 2u),
                PackInputWord(static_cast<std::uint8_t>(i + 3u), 4u),
                i < 120u ? nullptr : source.regionHashes.data(),
                i < 120u ? 0u : source.regionHashes.size()))
        {
            return 20;
        }
    }
    const auto consumeBatch = [&](std::uint32_t& consumerCursor,
                                  std::uint64_t& digest,
                                  const std::uint32_t sessionTag) {
        if (!BuildConfirmedInputBatch(
                nextStatus.gameId,
                consumerCursor,
                sessionTag,
                consumerCursor + 1u,
                &batch)
            || batch.startRecordIndex != consumerCursor || batch.count == 0u)
        {
            return false;
        }
        for (std::uint32_t i = 0u; i < batch.count; ++i)
        {
            const auto& frame = batch.frames[i];
            if (frame.recordIndex != consumerCursor + i
                || frame.sourceFrame != nextStart.sourceFrame + consumerCursor + i
                || frame.p1Word != PackInputWord(
                    static_cast<std::uint8_t>(consumerCursor + i), 2u)
                || frame.p2Word != PackInputWord(
                    static_cast<std::uint8_t>(consumerCursor + i + 3u), 4u))
            {
                return false;
            }
            digest = protocol::AdvanceInputDigest(
                digest,
                nextStatus.gameId,
                frame.recordIndex,
                frame.sourceGeneration,
                frame.sourceFrame,
                frame.screen,
                frame.p1Word,
                frame.p2Word,
                frame.hasRegionHashes,
                frame.regionHashes);
            if (frame.inputDigest != digest)
            {
                return false;
            }
        }
        consumerCursor += batch.count;
        return true;
    };

    GameExtent longExtent{};
    if (!GetGameExtent(nextStatus.gameId, &longExtent))
    {
        return 33;
    }
    std::uint32_t fastCursor = 0u;
    std::uint32_t lateCursor = 0u;
    std::uint64_t fastDigest = longExtent.startInputDigest;
    std::uint64_t lateDigest = longExtent.startInputDigest;
    std::uint32_t fastBatches = 0u;
    while (fastCursor < kLongTailFrames)
    {
        if (!consumeBatch(fastCursor, fastDigest, 0x11111111u))
        {
            return 21;
        }
        ++fastBatches;
        if ((fastBatches % 7u) == 0u && lateCursor < kLongTailFrames
            && !consumeBatch(lateCursor, lateDigest, 0x22222222u))
        {
            return 22;
        }
    }
    while (lateCursor < kLongTailFrames)
    {
        if (!consumeBatch(lateCursor, lateDigest, 0x22222222u))
        {
            return 31;
        }
    }
    if (fastCursor != kLongTailFrames || lateCursor != kLongTailFrames
        || fastDigest != lateDigest)
    {
        return 32;
    }
    if (GetStatus().confirmedFrames != kLongTailFrames
        || CopyFrom(nextStatus.gameId, kLongTailFrames - 2u, 8u).size() != 2u
        || BuildConfirmedInputBatch(nextStatus.gameId, kLongTailFrames, 1u, 1u, &batch))
    {
        return 23;
    }

    constexpr std::uint32_t kSetGames = 10u;
    constexpr std::uint32_t kShortGameFrames = 257u;
    std::vector<std::uint32_t> gameIds{firstGameId, nextStatus.gameId};
    GameStartInfo setStart = nextStart;
    setStart.sourceFrame += kLongTailFrames;
    for (std::uint32_t gameIndex = 2u; gameIndex < kSetGames; ++gameIndex)
    {
        setStart.sourceGeneration += 2u;
        setStart.sourceFrame += 17u;
        setStart.p1CharacterId = static_cast<std::uint8_t>(gameIndex);
        setStart.p2CharacterId = static_cast<std::uint8_t>(gameIndex + 1u);
        setStart.p1PaletteId = static_cast<std::uint8_t>(gameIndex % 6u);
        setStart.p2PaletteId = static_cast<std::uint8_t>((gameIndex + 3u) % 6u);
        if (!BeginGame(setStart))
        {
            return 24;
        }
        const std::uint32_t gameId = GetStatus().gameId;
        gameIds.push_back(gameId);
        for (std::uint32_t i = 0u; i < kShortGameFrames; ++i)
        {
            if (!PublishConfirmedFrame(
                    setStart.sourceGeneration,
                    setStart.sourceFrame + i,
                    i < 32u ? trial::kScreenLoading : trial::kScreenBattle,
                    PackInputWord(static_cast<std::uint8_t>(gameIndex + i), 1u),
                    PackInputWord(static_cast<std::uint8_t>(gameIndex + i + 1u), 2u),
                    nullptr,
                    0u))
            {
                return 25;
            }
        }
        setStart.sourceFrame += kShortGameFrames;
    }
    const Status setStatus = GetStatus();
    const std::size_t expectedSetFrames =
        40u + kLongTailFrames + (kSetGames - 2u) * kShortGameFrames;
    if (gameIds.size() != kSetGames
        || setStatus.retainedGames != kSetGames
        || setStatus.firstGameId != firstGameId
        || setStatus.retainedFrames != expectedSetFrames
        || setStatus.targetFps != 64u
        || !BuildConfirmedInputBatch(firstGameId, 0u, 1u, 1u, &batch)
        || !BuildGameStartMessage(firstGameId, 1u, 1u, &wireStart)
        || wireStart.gameId != firstGameId
        || !BuildGameStartMessage(gameIds.back(), 1u, 1u, &wireStart)
        || wireStart.gameId != gameIds.back()
        || wireStart.p1CharacterId != setStart.p1CharacterId
        || wireStart.p2CharacterId != setStart.p2CharacterId
        || wireStart.p1PaletteId != setStart.p1PaletteId
        || wireStart.p2PaletteId != setStart.p2PaletteId
        || wireStart.stageId != setStart.stageId)
    {
        return 26;
    }
    std::uint64_t chainedDigest = protocol::kInputDigestSeed;
    for (std::size_t gameIndex = 0u; gameIndex < gameIds.size(); ++gameIndex)
    {
        GameExtent extent{};
        if (!GetGameExtent(gameIds[gameIndex], &extent)
            || extent.gameId != gameIds[gameIndex]
            || extent.startInputDigest != chainedDigest
            || extent.hasNextGame != (gameIndex + 1u < gameIds.size()))
        {
            return 36;
        }
        chainedDigest = extent.finalInputDigest;
    }
    if (chainedDigest != setStatus.latestInputDigest)
    {
        return 37;
    }
    EndSession();

    thv::session::LauncherSession clientSession{};
    clientSession.mode = thv::session::LauncherMode::Client;
    clientSession.isHost = false;
    clientSession.localIsP1 = false;
    clientSession.playerName = L"ClientName";
    clientSession.peerName = L"HostName";
    clientSession.targetFps = 64u;
    BeginHostArchive(clientSession);
    if (!BeginGame(gameStart) || !GetStatus().gameActive)
    {
        return 27;
    }
    protocol::GameStart clientStart{};
    if (!BuildGameStartMessage(0u, 0x12345678u, 1u, &clientStart)
        || clientStart.localSide != 0u
        || clientStart.targetFps != 64u
        || std::string(clientStart.p1Name.data()) != "HostName"
        || std::string(clientStart.p2Name.data()) != "ClientName")
    {
        return 51;
    }
    EndSession();

    thv::session::LauncherSession sixtyFpsSession = hostSession;
    sixtyFpsSession.targetFps = 60u;
    BeginHostArchive(sixtyFpsSession);
    protocol::GameStart sixtyFpsStart{};
    if (!BeginGame(gameStart)
        || !BuildGameStartMessage(0u, 0x12345678u, 2u, &sixtyFpsStart)
        || sixtyFpsStart.targetFps != 60u
        || protocol::GameStartFingerprint(sixtyFpsStart) == startFingerprint)
    {
        return 58;
    }
    EndSession();

    thv::session::LauncherSession legacyRateSession{};
    legacyRateSession.mode = thv::session::LauncherMode::Host;
    legacyRateSession.isHost = true;
    legacyRateSession.targetFps = 0u;
    BeginHostArchive(legacyRateSession);
    if (GetStatus().hostArchiveActive || BeginGame(gameStart))
    {
        return 56;
    }
    EndSession();

    thv::session::LauncherSession offlineSession{};
    BeginHostArchive(offlineSession);
    if (BeginGame(gameStart) || GetStatus().gameActive)
    {
        return 52;
    }
    EndSession();
    return 0;
}
