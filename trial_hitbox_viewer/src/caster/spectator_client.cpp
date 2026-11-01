#include "spectator_client.h"

#include "autopunch_client.h"
#include "autopunch_protocol.h"
#include "debug_log.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace thv::spectator::client
{
namespace
{

constexpr DWORD kHelloIntervalMs = 250u;
constexpr DWORD kKeepAliveIntervalMs = 1000u;
constexpr DWORD kRedirectTimeoutMs = 12000u;
constexpr DWORD kServerTimeoutMs = 60000u;
constexpr std::size_t kReceiveHighWaterFrames = 131072u;
constexpr std::size_t kMaximumReorderFrames = protocol::kBatchFrames * 32u;
constexpr int kReceiveDatagramBudget = 256;

struct ReceivedGame
{
    protocol::GameStart start{};
    std::deque<protocol::ConfirmedInput> frames{};
    std::uint32_t nextReceiveRecordIndex = 0u;
    std::uint32_t nextPlayRecordIndex = 0u;
    std::uint64_t receiveDigest = protocol::kInputDigestSeed;
    std::uint64_t playDigest = protocol::kInputDigestSeed;
    std::map<std::uint32_t, std::vector<protocol::ConfirmedInput>> pending{};
    bool playbackStarted = false;
    bool complete = false;
};

SOCKET g_socket = INVALID_SOCKET;
sockaddr_in g_originRoute{};
sockaddr_in g_originSource{};
sockaddr_in g_currentRoute{};
sockaddr_in g_currentSource{};
std::uint16_t g_localPort = 0u;
trialnet::autopunch::Client g_autopunch{};
std::deque<ReceivedGame> g_games;
std::mutex g_mutex;
Status g_status{};
DWORD g_startedMs = 0u;
DWORD g_lastReceiveMs = 0u;
DWORD g_lastSendMs = 0u;
DWORD g_redirectStartedMs = 0u;
std::uint32_t g_nextSequence = 1u;
std::uint32_t g_clientNonce = 0u;
std::uint32_t g_ticketRelayEpoch = 0u;
std::uint32_t g_ticketGrantId = 0u;
std::uint32_t g_ticketFailedGrantId = 0u;
std::array<std::uint8_t, 16> g_ticketToken{};
std::size_t g_pendingFrames = 0u;
bool g_streamInitialized = false;
bool g_redirectConsumed = false;
bool g_redirectActive = false;
bool g_terminalWireSet = false;
protocol::Terminal g_terminalWire{};

bool SameEndpoint(const sockaddr_in& left, const sockaddr_in& right)
{
    return left.sin_family == AF_INET && right.sin_family == AF_INET
        && left.sin_addr.S_un.S_addr == right.sin_addr.S_un.S_addr
        && left.sin_port == right.sin_port;
}

bool SameRouteAddress(const sockaddr_in& left, const sockaddr_in& right)
{
    return left.sin_family == AF_INET && right.sin_family == AF_INET
        && left.sin_addr.S_un.S_addr == right.sin_addr.S_un.S_addr;
}

bool HasTicket(const std::array<std::uint8_t, 16>& token)
{
    return std::any_of(token.begin(), token.end(), [](const std::uint8_t byte) {
        return byte != 0u;
    });
}

std::string FormatEndpoint(const sockaddr_in& endpoint)
{
    char address[INET_ADDRSTRLEN] = {};
    if (InetNtopA(
            AF_INET,
            const_cast<IN_ADDR*>(&endpoint.sin_addr),
            address,
            static_cast<DWORD>(sizeof(address))) == nullptr)
    {
        std::strcpy(address, "?");
    }
    return std::string(address) + ":" + std::to_string(ntohs(endpoint.sin_port));
}

std::string Narrow(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
    {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        CP_ACP,
        0,
        text.c_str(),
        -1,
        out.data(),
        needed,
        nullptr,
        nullptr);
    if (written <= 1)
    {
        return {};
    }
    out.resize(static_cast<std::size_t>(written - 1));
    return out;
}

void AutopunchLog(void*, const char* const message)
{
    if (message != nullptr)
    {
        log::Info("spectator", "autopunch %s", message);
    }
}

void SetFatalLocked(const char* const error)
{
    if (!g_status.fatal)
    {
        const bool integrityFailure = error != nullptr
            && (std::strstr(error, "mismatch") != nullptr
                || std::strstr(error, "digest") != nullptr
                || std::strstr(error, "non-canonical") != nullptr
                || std::strstr(error, "metadata changed") != nullptr
                || std::strstr(error, "discontinuous") != nullptr
                || std::strstr(error, "record gap") != nullptr
                || std::strstr(error, "alignment") != nullptr);
        if (integrityFailure)
        {
            (void)log::EnableFileLogging();
        }
        g_status.fatal = true;
        g_status.error = error != nullptr ? error : "spectator transport failed";
        log::Error("spectator", "%s", g_status.error.c_str());
    }
}

bool SendRaw(const void* const data, const std::size_t size)
{
    if (g_socket == INVALID_SOCKET || data == nullptr || size == 0u || size > 1200u)
    {
        return false;
    }
    const int sent = sendto(
        g_socket,
        static_cast<const char*>(data),
        static_cast<int>(size),
        0,
        reinterpret_cast<const sockaddr*>(&g_currentSource),
        sizeof(g_currentSource));
    if (sent == static_cast<int>(size))
    {
        g_lastSendMs = GetTickCount();
        return true;
    }
    return false;
}

void SendHello()
{
    protocol::Hello hello{};
    hello.header.sequence = g_nextSequence++;
    // Resume at the first needed record, never at 0:0.
    hello.requestedGameId = g_status.receivingGameId;
    hello.nextRecordIndex = g_status.receivingRecordIndex;
    hello.clientNonce = g_clientNonce;
    hello.simulationContractId = protocol::kSimulationContractId;
    hello.relayEpoch = g_ticketRelayEpoch;
    hello.grantId = g_ticketGrantId;
    hello.failedGrantId = g_ticketFailedGrantId;
    hello.token = g_ticketToken;
    (void)SendRaw(&hello, sizeof(hello));
}

void SendInitAck(const protocol::GameStart& start, const std::uint32_t nextRecordIndex)
{
    protocol::InitAck ack{};
    ack.header.sessionTag = g_status.sessionTag;
    ack.header.sequence = g_nextSequence++;
    ack.gameId = start.gameId;
    ack.nextRecordIndex = nextRecordIndex;
    ack.clientNonce = g_clientNonce;
    ack.initFingerprint = protocol::GameStartFingerprint(start);
    (void)SendRaw(&ack, sizeof(ack));
}

void SendAck(const std::uint32_t gameId, const std::uint32_t nextRecordIndex)
{
    protocol::Ack ack{};
    ack.header.sessionTag = g_status.sessionTag;
    ack.header.sequence = g_nextSequence++;
    ack.gameId = gameId;
    ack.nextRecordIndex = nextRecordIndex;
    (void)SendRaw(&ack, sizeof(ack));
}

void SendKeepAlive()
{
    if (g_status.sessionTag == 0u)
    {
        SendHello();
        return;
    }
    protocol::KeepAlive keepAlive{};
    keepAlive.header.sessionTag = g_status.sessionTag;
    keepAlive.header.sequence = g_nextSequence++;
    keepAlive.gameId = g_status.receivingGameId;
    keepAlive.nextRecordIndex = g_status.receivingRecordIndex;
    (void)SendRaw(&keepAlive, sizeof(keepAlive));
}

void SendTerminalAck(const protocol::Terminal& terminal)
{
    protocol::TerminalAck ack{};
    ack.header.sessionTag = g_status.sessionTag;
    ack.header.sequence = g_nextSequence++;
    ack.finalGameId = terminal.finalGameId;
    ack.finalRecordIndex = terminal.finalRecordIndex;
    ack.finalInputDigest = terminal.finalInputDigest;
    (void)SendRaw(&ack, sizeof(ack));
}

void ResetAttachStateLocked(const DWORD now)
{
    g_status.connected = false;
    g_status.waitingForMatch = false;
    g_status.sessionTag = 0u;
    g_startedMs = now;
    g_lastReceiveMs = 0u;
    g_lastSendMs = 0u;
}

bool AttachAutopunchRouteLocked(const sockaddr_in& route)
{
    g_autopunch.Detach();
    return g_autopunch.Attach(
        g_socket,
        g_localPort,
        &route,
        true,
        &AutopunchLog,
        nullptr);
}

bool ReturnToOriginLocked(const char* const reason)
{
    if (!g_redirectActive || g_status.redirectFallbackUsed || g_streamInitialized)
    {
        return false;
    }

    const std::uint32_t failedGrantId = g_ticketGrantId;
    const DWORD now = GetTickCount();
    g_currentRoute = g_originRoute;
    g_currentSource = g_originSource;
    g_ticketGrantId = 0u;
    g_ticketFailedGrantId = failedGrantId;
    // Keep relayEpoch and the ticket: they authenticate the failed-grant report.
    g_redirectActive = false;
    g_redirectStartedMs = 0u;
    g_status.redirected = false;
    g_status.redirectFallbackUsed = true;
    g_status.failedRedirectGrantId = failedGrantId;
    ResetAttachStateLocked(now);

    if (!AttachAutopunchRouteLocked(g_currentRoute))
    {
        SetFatalLocked("spectator redirect fallback transport failed");
        return false;
    }

    log::Warn(
        "spectator",
        "SPECTATOR REDIRECT FALLBACK grant=%u epoch=%u reason=%s origin=%s cursor=%u:%u digest=%016llX",
        failedGrantId,
        g_ticketRelayEpoch,
        reason != nullptr && reason[0] != '\0' ? reason : "target unavailable",
        FormatEndpoint(g_originSource).c_str(),
        g_status.receivingGameId,
        g_status.receivingRecordIndex,
        static_cast<unsigned long long>(g_status.receivedInputDigest));
    SendHello();
    return true;
}

bool HandleRedirectLocked(
    const protocol::Redirect& redirect,
    const sockaddr_in& source)
{
    const auto reject = [&](const char* const reason) {
        log::Warn(
            "spectator",
            "ignored spectator redirect reason=%s grant=%u epoch=%u source=%s cursor=%u:%u",
            reason,
            redirect.grantId,
            redirect.relayEpoch,
            FormatEndpoint(source).c_str(),
            redirect.requestedGameId,
            redirect.nextRecordIndex);
        return false;
    };

    if (!SameEndpoint(source, g_originSource)
        || !SameEndpoint(source, g_currentSource))
    {
        return reject("not the exact original source");
    }
    if (g_streamInitialized || !g_games.empty())
    {
        return reject("deterministic stream already initialized");
    }
    if (g_redirectActive || g_redirectConsumed || g_status.redirectFallbackUsed)
    {
        return reject("one-hop redirect already consumed");
    }
    if (redirect.clientNonce != g_clientNonce)
    {
        return reject("client nonce mismatch");
    }
    if (g_status.sessionTag != 0u
        && redirect.header.sessionTag != g_status.sessionTag)
    {
        return reject("origin session tag mismatch");
    }
    if (redirect.hop != 1u || redirect.reserved != 0u
        || redirect.relayEpoch == 0u || redirect.grantId == 0u
        || redirect.grantId == g_ticketFailedGrantId || !HasTicket(redirect.token))
    {
        return reject("invalid or repeated redirect ticket");
    }
    if (redirect.requestedGameId != g_status.receivingGameId
        || redirect.nextRecordIndex != g_status.receivingRecordIndex
        || redirect.expectedDigest != g_status.receivedInputDigest)
    {
        return reject("source cursor or digest mismatch");
    }
    if (redirect.targetIpv4 == 0u || redirect.targetIpv4 == INADDR_NONE
        || redirect.targetInternalPort == 0u)
    {
        return reject("invalid target endpoint");
    }

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.S_un.S_addr = redirect.targetIpv4;
    target.sin_port = htons(redirect.targetInternalPort);
    if (SameEndpoint(target, g_originRoute))
    {
        return reject("target is the original route");
    }

    const DWORD now = GetTickCount();
    g_redirectConsumed = true;
    g_redirectActive = true;
    g_redirectStartedMs = now;
    g_currentRoute = target;
    g_currentSource = target;
    g_ticketRelayEpoch = redirect.relayEpoch;
    g_ticketGrantId = redirect.grantId;
    g_ticketFailedGrantId = 0u;
    g_ticketToken = redirect.token;
    g_status.redirected = true;
    g_status.redirectFallbackUsed = false;
    g_status.relayEpoch = redirect.relayEpoch;
    g_status.redirectGrantId = redirect.grantId;
    g_status.failedRedirectGrantId = 0u;
    ResetAttachStateLocked(now);

    if (!AttachAutopunchRouteLocked(g_currentRoute))
    {
        return ReturnToOriginLocked("could not attach target route");
    }

    log::Info(
        "spectator",
        "SPECTATOR REDIRECT ACCEPT grant=%u epoch=%u target=%s hop=%u cursor=%u:%u digest=%016llX timeout_ms=%u",
        redirect.grantId,
        redirect.relayEpoch,
        FormatEndpoint(target).c_str(),
        static_cast<unsigned>(redirect.hop),
        redirect.requestedGameId,
        redirect.nextRecordIndex,
        static_cast<unsigned long long>(redirect.expectedDigest),
        static_cast<unsigned>(kRedirectTimeoutMs));
    SendHello();
    return true;
}

ReceivedGame* FindGameLocked(const std::uint32_t gameId)
{
    const auto it = std::find_if(
        g_games.begin(),
        g_games.end(),
        [gameId](const ReceivedGame& game) { return game.start.gameId == gameId; });
    return it != g_games.end() ? &*it : nullptr;
}

void HandleGameStartLocked(const protocol::GameStart& start)
{
    if (start.header.sessionTag == 0u || start.clientNonce != g_clientNonce
        || start.compatibilityId != trialnet::compat::kCompatibilityId
        || start.simulationContractId != protocol::kSimulationContractId
        || start.inputDigestSeed == 0u || start.rngSeed == 0u)
    {
        SetFatalLocked("spectator deterministic initialization mismatch");
        return;
    }
    if (!protocol::MatchesTargetFpsContract(g_status.targetFps, start.targetFps))
    {
        SetFatalLocked(
            protocol::IsSupportedTargetFps(start.targetFps)
                ? "spectator target fps changed within retained session"
                : "spectator target fps is invalid");
        return;
    }
    if (g_status.sessionTag == 0u)
    {
        g_status.sessionTag = start.header.sessionTag;
    }
    if (start.header.sessionTag != g_status.sessionTag || start.gameId == 0u)
    {
        return;
    }

    ReceivedGame* existing = FindGameLocked(start.gameId);
    if (existing != nullptr
        && protocol::GameStartFingerprint(existing->start)
            != protocol::GameStartFingerprint(start))
    {
        SetFatalLocked("spectator retransmitted GameStart metadata changed");
        return;
    }
    if (existing != nullptr && g_status.receivingGameId != 0u
        && start.gameId < g_status.receivingGameId)
    {
        // An old GameStart must not move the receive owner backwards.
        SendAck(existing->start.gameId, existing->nextReceiveRecordIndex);
        return;
    }
    if (existing == nullptr)
    {
        if (!g_games.empty())
        {
            const std::uint32_t expectedGameId = g_games.back().start.gameId + 1u;
            if (start.gameId != expectedGameId)
            {
                SetFatalLocked("spectator GameStart sequence is discontinuous");
                return;
            }
            if (start.inputDigestSeed != g_games.back().receiveDigest)
            {
                SetFatalLocked("spectator game boundary input digest is discontinuous");
                return;
            }
            g_games.back().complete = true;
        }
        ReceivedGame game{};
        game.start = start;
        game.receiveDigest = start.inputDigestSeed;
        game.playDigest = start.inputDigestSeed;
        g_games.push_back(std::move(game));
        existing = &g_games.back();
        log::Info(
            "spectator",
            "received game start game=%u source=%u:%u rng=%08X fps=%u visuals=P1(c%u,p%u) P2(c%u,p%u) stage=%u queuedGames=%zu",
            start.gameId,
            start.sourceGeneration,
            start.sourceFrame,
            start.rngSeed,
            static_cast<unsigned>(start.targetFps),
            static_cast<unsigned>(start.p1CharacterId),
            static_cast<unsigned>(start.p1PaletteId),
            static_cast<unsigned>(start.p2CharacterId),
            static_cast<unsigned>(start.p2PaletteId),
            static_cast<unsigned>(start.stageId),
            g_games.size());
    }
    if (!g_streamInitialized && g_redirectActive)
    {
        log::Info(
            "spectator",
            "SPECTATOR REDIRECT ATTACHED grant=%u epoch=%u source=%s state=game-start",
            g_status.redirectGrantId,
            g_status.relayEpoch,
            FormatEndpoint(g_currentSource).c_str());
    }
    g_streamInitialized = true;
    g_redirectStartedMs = 0u;
    g_status.connected = true;
    g_status.waitingForMatch = false;
    g_status.targetFps = start.targetFps;
    g_status.receivingGameId = existing->start.gameId;
    g_status.receivingRecordIndex = existing->nextReceiveRecordIndex;
    g_status.queuedGames = g_games.size();
    g_status.receivedInputDigest = existing->receiveDigest;
    // The host releases no input batch until this fingerprint is authenticated.
    SendInitAck(existing->start, existing->nextReceiveRecordIndex);
}

bool AppendContiguousFramesLocked(ReceivedGame& game)
{
    bool appended = false;
    for (;;)
    {
        auto selected = game.pending.end();
        for (auto it = game.pending.begin(); it != game.pending.end(); ++it)
        {
            const std::uint64_t end = static_cast<std::uint64_t>(it->first) + it->second.size();
            if (it->first <= game.nextReceiveRecordIndex
                && end > game.nextReceiveRecordIndex)
            {
                selected = it;
                break;
            }
            if (it->first > game.nextReceiveRecordIndex)
            {
                break;
            }
        }
        if (selected == game.pending.end())
        {
            break;
        }

        const std::size_t skip = static_cast<std::size_t>(
            game.nextReceiveRecordIndex - selected->first);
        for (std::size_t index = skip; index < selected->second.size(); ++index)
        {
            const protocol::ConfirmedInput& frame = selected->second[index];
            if (frame.recordIndex != game.nextReceiveRecordIndex)
            {
                SetFatalLocked("spectator reordered batch contains a record gap");
                return false;
            }
            if (!protocol::HasCanonicalRegionHashes(
                    frame.hasRegionHashes,
                    frame.regionHashes))
            {
                SetFatalLocked("spectator input carries non-canonical region hashes");
                return false;
            }
            const std::uint64_t expectedDigest = protocol::AdvanceInputDigest(
                game.receiveDigest,
                game.start.gameId,
                frame.recordIndex,
                frame.sourceGeneration,
                frame.sourceFrame,
                frame.screen,
                frame.p1Word,
                frame.p2Word,
                frame.hasRegionHashes,
                frame.regionHashes);
            if (frame.inputDigest != expectedDigest)
            {
                log::Error(
                    "spectator",
                    "INPUT DIGEST MISMATCH receive game=%u record=%u source=%u expected=%016llX got=%016llX",
                    game.start.gameId,
                    frame.recordIndex,
                    frame.sourceFrame,
                    static_cast<unsigned long long>(expectedDigest),
                    static_cast<unsigned long long>(frame.inputDigest));
                SetFatalLocked("spectator confirmed input digest mismatch");
                return false;
            }
            game.receiveDigest = expectedDigest;
            game.frames.push_back(frame);
            ++game.nextReceiveRecordIndex;
            ++g_status.queuedFrames;
            appended = true;
            if (frame.recordIndex == 0u || ((frame.recordIndex + 1u) % 256u) == 0u)
            {
                log::Info(
                    "spectator",
                    "SPECTATOR INPUT DIGEST receive game=%u records=%u digest=%016llX source=%u",
                    game.start.gameId,
                    frame.recordIndex + 1u,
                    static_cast<unsigned long long>(frame.inputDigest),
                    frame.sourceFrame);
            }
        }
        g_pendingFrames -= selected->second.size();
        game.pending.erase(selected);
    }
    return appended;
}

void HandleBatchLocked(const protocol::ConfirmedInputBatch& batch)
{
    if (batch.header.sessionTag != g_status.sessionTag || batch.count == 0u
        || batch.count > batch.frames.size())
    {
        return;
    }
    ReceivedGame* const game = FindGameLocked(batch.gameId);
    if (game == nullptr || batch.gameId != g_status.receivingGameId)
    {
        return;
    }

    const std::uint32_t batchEnd =
        batch.startRecordIndex + static_cast<std::uint32_t>(batch.count);
    if (batchEnd <= game->nextReceiveRecordIndex)
    {
        SendAck(batch.gameId, game->nextReceiveRecordIndex);
        return;
    }
    if (batch.startRecordIndex > game->nextReceiveRecordIndex + kMaximumReorderFrames
        || g_status.queuedFrames + g_pendingFrames + batch.count > kReceiveHighWaterFrames)
    {
        SendAck(batch.gameId, game->nextReceiveRecordIndex);
        return;
    }

    for (std::size_t index = 0u; index < batch.count; ++index)
    {
        const protocol::ConfirmedInput& frame = batch.frames[index];
        const std::uint32_t expected =
            batch.startRecordIndex + static_cast<std::uint32_t>(index);
        if (frame.recordIndex != expected)
        {
            SetFatalLocked("spectator input batch contains a record gap");
            return;
        }
    }
    if (game->pending.find(batch.startRecordIndex) == game->pending.end())
    {
        std::vector<protocol::ConfirmedInput> frames;
        frames.assign(batch.frames.begin(), batch.frames.begin() + batch.count);
        g_pendingFrames += frames.size();
        game->pending.emplace(batch.startRecordIndex, std::move(frames));
    }
    if (g_status.fatal || !AppendContiguousFramesLocked(*game))
    {
        if (g_status.fatal)
        {
            return;
        }
    }
    g_status.receivingRecordIndex = game->nextReceiveRecordIndex;
    g_status.receivedInputDigest = game->receiveDigest;
    if (game->nextReceiveRecordIndex == batchEnd
        && (game->nextReceiveRecordIndex <= protocol::kBatchFrames
            || (game->nextReceiveRecordIndex % 256u) < batch.count))
    {
        log::Info(
            "spectator",
            "receive progress game=%u next=%u queued=%zu",
            batch.gameId,
            game->nextReceiveRecordIndex,
            g_status.queuedFrames);
    }
    SendAck(batch.gameId, game->nextReceiveRecordIndex);
}

void HandleDatagramLocked(
    const std::uint8_t* const data,
    const std::size_t size,
    const sockaddr_in& source)
{
    if (data == nullptr || size < sizeof(protocol::Header)
        || !SameEndpoint(source, g_currentSource))
    {
        return;
    }
    protocol::Header header{};
    std::memcpy(&header, data, sizeof(header));
    if (header.magic != protocol::kMagic || header.version != protocol::kVersion)
    {
        if (header.magic == protocol::kMagic && header.version != protocol::kVersion)
        {
            if (g_redirectActive && !g_streamInitialized)
            {
                (void)ReturnToOriginLocked("redirect target protocol version mismatch");
            }
            else
            {
                SetFatalLocked("spectator protocol version mismatch");
            }
        }
        return;
    }
    bool accepted = false;
    bool routeChanged = false;
    switch (header.kind)
    {
    case protocol::MessageKind::Redirect:
        if (size == sizeof(protocol::Redirect))
        {
            protocol::Redirect redirect{};
            std::memcpy(&redirect, data, sizeof(redirect));
            if (HandleRedirectLocked(redirect, source))
            {
                accepted = true;
                routeChanged = true;
            }
        }
        break;
    case protocol::MessageKind::GameStart:
        if (size == sizeof(protocol::GameStart))
        {
            protocol::GameStart start{};
            std::memcpy(&start, data, sizeof(start));
            if (start.clientNonce == g_clientNonce
                && (g_status.sessionTag == 0u || start.header.sessionTag == g_status.sessionTag))
            {
                accepted = true;
                if (g_redirectActive && !g_streamInitialized
                    && (start.header.sessionTag == 0u || start.gameId == 0u
                        || start.compatibilityId != trialnet::compat::kCompatibilityId
                        || start.simulationContractId != protocol::kSimulationContractId
                        || start.inputDigestSeed == 0u || start.rngSeed == 0u
                        || !protocol::IsSupportedTargetFps(start.targetFps)))
                {
                    routeChanged = ReturnToOriginLocked(
                        "redirect target deterministic initialization mismatch");
                    if (!routeChanged)
                    {
                        SetFatalLocked("spectator redirect initialization failed");
                    }
                    break;
                }
                HandleGameStartLocked(start);
            }
        }
        break;
    case protocol::MessageKind::ConfirmedInputBatch:
        if (size >= offsetof(protocol::ConfirmedInputBatch, frames)
            && size <= sizeof(protocol::ConfirmedInputBatch)
            && header.sessionTag != 0u && header.sessionTag == g_status.sessionTag)
        {
            protocol::ConfirmedInputBatch batch{};
            std::memcpy(&batch, data, size);
            if (batch.count != 0u && batch.count <= batch.frames.size()
                && size == protocol::ConfirmedInputBatchWireSize(batch.count))
            {
                accepted = true;
                HandleBatchLocked(batch);
            }
        }
        break;
    case protocol::MessageKind::KeepAlive:
        if (size == sizeof(protocol::KeepAlive) && header.sessionTag == g_status.sessionTag)
        {
            accepted = true;
            SendKeepAlive();
        }
        break;
    case protocol::MessageKind::Waiting:
        if (size == sizeof(protocol::Waiting))
        {
            protocol::Waiting waiting{};
            std::memcpy(&waiting, data, sizeof(waiting));
            if (waiting.clientNonce == g_clientNonce && waiting.header.sessionTag != 0u
                && (g_status.sessionTag == 0u
                    || waiting.header.sessionTag == g_status.sessionTag))
            {
                if (g_status.sessionTag == 0u)
                {
                    g_status.sessionTag = waiting.header.sessionTag;
                }
                g_status.connected = true;
                g_status.waitingForMatch = true;
                g_redirectStartedMs = 0u;
                accepted = true;
                if (g_redirectActive)
                {
                    log::Info(
                        "spectator",
                        "SPECTATOR REDIRECT ATTACHED grant=%u epoch=%u source=%s state=waiting",
                        g_status.redirectGrantId,
                        g_status.relayEpoch,
                        FormatEndpoint(g_currentSource).c_str());
                }
            }
        }
        break;
    case protocol::MessageKind::Terminal:
        if (size == sizeof(protocol::Terminal) && header.sessionTag == g_status.sessionTag)
        {
            protocol::Terminal terminal{};
            std::memcpy(&terminal, data, sizeof(terminal));
            if (g_terminalWireSet
                && (terminal.reason != g_terminalWire.reason
                    || terminal.finalGameId != g_terminalWire.finalGameId
                    || terminal.finalRecordIndex != g_terminalWire.finalRecordIndex
                    || terminal.finalInputDigest != g_terminalWire.finalInputDigest
                    || terminal.totalConfirmedFrames != g_terminalWire.totalConfirmedFrames))
            {
                SetFatalLocked("spectator terminal metadata changed");
                return;
            }
            g_terminalWire = terminal;
            g_terminalWireSet = true;
            SendTerminalAck(terminal);

            std::uint32_t effectiveGame = terminal.finalGameId;
            std::uint32_t effectiveRecord = terminal.finalRecordIndex;
            std::uint64_t effectiveDigest = terminal.finalInputDigest;
            // The ACK itself may have been lost: keep verified records rather than rewinding.
            if (terminal.reason == protocol::TerminalReason::ArchiveUnavailable
                && g_status.receivingGameId != 0u
                && (g_status.receivingGameId > effectiveGame
                    || (g_status.receivingGameId == effectiveGame
                        && g_status.receivingRecordIndex > effectiveRecord)))
            {
                effectiveGame = g_status.receivingGameId;
                effectiveRecord = g_status.receivingRecordIndex;
                effectiveDigest = g_status.receivedInputDigest;
            }
            std::size_t discardedPending = 0u;
            for (ReceivedGame& game : g_games)
            {
                for (const auto& batch : game.pending)
                {
                    discardedPending += batch.second.size();
                }
                game.pending.clear();
            }
            g_pendingFrames = discardedPending <= g_pendingFrames
                ? g_pendingFrames - discardedPending
                : 0u;
            g_status.terminal = true;
            g_status.terminalReason = terminal.reason;
            g_status.terminalGameId = effectiveGame;
            g_status.terminalRecordIndex = effectiveRecord;
            g_status.terminalInputDigest = effectiveDigest;
            g_status.terminalTotalConfirmedFrames = terminal.totalConfirmedFrames;
            accepted = true;
            log::Info(
                "spectator",
                "SPECTATOR SOURCE END reason=%u game=%u records=%u digest=%016llX effective=%u:%u/%016llX total=%llu",
                static_cast<unsigned>(terminal.reason),
                terminal.finalGameId,
                terminal.finalRecordIndex,
                static_cast<unsigned long long>(terminal.finalInputDigest),
                effectiveGame,
                effectiveRecord,
                static_cast<unsigned long long>(effectiveDigest),
                static_cast<unsigned long long>(terminal.totalConfirmedFrames));
        }
        break;
    case protocol::MessageKind::Reject:
        if (size == sizeof(protocol::Reject))
        {
            protocol::Reject reject{};
            std::memcpy(&reject, data, sizeof(reject));
            accepted = true;
            if (g_redirectActive && !g_streamInitialized)
            {
                log::Warn(
                    "spectator",
                    "redirect target rejected attach grant=%u epoch=%u reason=%u",
                    g_status.redirectGrantId,
                    g_status.relayEpoch,
                    static_cast<unsigned>(reject.reason));
                routeChanged = ReturnToOriginLocked("target rejected redirect ticket");
                break;
            }
            if (reject.reason == protocol::RejectReason::VersionMismatch)
            {
                SetFatalLocked("host rejected spectator protocol version");
            }
            else if (reject.reason == protocol::RejectReason::ServerFull)
            {
                SetFatalLocked("host spectator slots are full");
            }
            else
            {
                SetFatalLocked("host rejected spectator attach request");
            }
        }
        break;
    default:
        break;
    }
    if (accepted && !routeChanged)
    {
        g_lastReceiveMs = GetTickCount();
    }
}

}

bool Initialize(const session::LauncherSession& launcherSession)
{
    Shutdown();
    const std::string peer = Narrow(launcherSession.peerIp);
    if (peer.empty())
    {
        return false;
    }

    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET)
    {
        return false;
    }
    u_long nonBlocking = 1u;
    if (ioctlsocket(g_socket, FIONBIO, &nonBlocking) == SOCKET_ERROR)
    {
        log::Error("spectator", "FIONBIO failed error=%d", WSAGetLastError());
        Shutdown();
        return false;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = 0u;
    local.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
    if (bind(g_socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
    {
        Shutdown();
        return false;
    }

    sockaddr_in originRoute{};
    originRoute.sin_family = AF_INET;
    originRoute.sin_port = htons(launcherSession.peerPort != 0u
            ? launcherSession.peerPort
            : launcherSession.port);
    if (InetPtonA(AF_INET, peer.c_str(), &originRoute.sin_addr) != 1)
    {
        Shutdown();
        return false;
    }

    sockaddr_in bound{};
    int boundLength = sizeof(bound);
    if (getsockname(g_socket, reinterpret_cast<sockaddr*>(&bound), &boundLength) != 0
        || bound.sin_port == 0u
        || !g_autopunch.Attach(
            g_socket,
            ntohs(bound.sin_port),
            &originRoute,
            true,
            &AutopunchLog,
            nullptr))
    {
        Shutdown();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status = {};
        g_status.active = true;
        g_originRoute = originRoute;
        g_originSource = originRoute;
        g_currentRoute = originRoute;
        g_currentSource = originRoute;
        g_localPort = ntohs(bound.sin_port);
        g_startedMs = GetTickCount();
        g_lastReceiveMs = 0u;
        g_lastSendMs = 0u;
        g_redirectStartedMs = 0u;
        g_nextSequence = 1u;
        g_clientNonce = static_cast<std::uint32_t>(GetTickCount())
            ^ static_cast<std::uint32_t>(GetCurrentProcessId())
            ^ (static_cast<std::uint32_t>(bound.sin_port) << 16u);
        if (g_clientNonce == 0u)
        {
            g_clientNonce = 0x53504543u; // "SPEC"; zero is reserved as invalid on the host.
        }
        g_ticketRelayEpoch = 0u;
        g_ticketGrantId = 0u;
        g_ticketFailedGrantId = 0u;
        g_ticketToken = {};
        g_pendingFrames = 0u;
        g_streamInitialized = false;
        g_redirectConsumed = false;
        g_redirectActive = false;
        g_terminalWireSet = false;
        g_terminalWire = {};
    }
    SendHello();
    log::Info(
        "spectator",
        "client transport open local=%u host=%s:%u",
        static_cast<unsigned>(ntohs(bound.sin_port)),
        peer.c_str(),
        static_cast<unsigned>(ntohs(originRoute.sin_port)));
    return true;
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_socket != INVALID_SOCKET && g_status.sessionTag != 0u)
    {
        protocol::Detach detach{};
        detach.header.sessionTag = g_status.sessionTag;
        detach.header.sequence = g_nextSequence++;
        (void)SendRaw(&detach, sizeof(detach));
    }
    g_autopunch.Detach();
    if (g_socket != INVALID_SOCKET)
    {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }
    g_games.clear();
    g_originRoute = {};
    g_originSource = {};
    g_currentRoute = {};
    g_currentSource = {};
    g_localPort = 0u;
    g_status = {};
    g_startedMs = 0u;
    g_lastReceiveMs = 0u;
    g_lastSendMs = 0u;
    g_redirectStartedMs = 0u;
    g_ticketRelayEpoch = 0u;
    g_ticketGrantId = 0u;
    g_ticketFailedGrantId = 0u;
    g_ticketToken = {};
    g_pendingFrames = 0u;
    g_streamInitialized = false;
    g_redirectConsumed = false;
    g_redirectActive = false;
    g_terminalWireSet = false;
    g_terminalWire = {};
}

void Poll()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_status.active || g_socket == INVALID_SOCKET || g_status.fatal)
    {
        return;
    }

    const DWORD now = GetTickCount();
    if (!g_status.connected && now - g_startedMs >= trialnet::autopunch::kDirectAttemptMs)
    {
        if (!g_autopunch.IsActive())
        {
            g_autopunch.Start(now);
        }
        g_autopunch.Service(now);
    }

    int datagrams = 0;
    for (; datagrams < kReceiveDatagramBudget; ++datagrams)
    {
        sockaddr_in source{};
        int sourceLength = sizeof(source);
        std::uint8_t buffer[1200]{};
        const int received = recvfrom(
            g_socket,
            reinterpret_cast<char*>(buffer),
            sizeof(buffer),
            0,
            reinterpret_cast<sockaddr*>(&source),
            &sourceLength);
        if (received <= 0)
        {
            break;
        }

        sockaddr_in mapped{};
        const auto punch = g_autopunch.HandleInbound(
            buffer,
            static_cast<std::size_t>(received),
            source,
            &mapped);
        if ((punch == trialnet::autopunch::InboundResult::PeerMapped
                || punch == trialnet::autopunch::InboundResult::PeerObserved)
            && (!g_status.connected || g_autopunch.IsActive()))
        {
            if (!SameRouteAddress(mapped, g_currentRoute))
            {
                log::Warn(
                    "spectator",
                    "ignored AutoPunch mapping outside current route mapped=%s route=%s redirected=%d",
                    FormatEndpoint(mapped).c_str(),
                    FormatEndpoint(g_currentRoute).c_str(),
                    g_redirectActive ? 1 : 0);
                continue;
            }
            if (punch == trialnet::autopunch::InboundResult::PeerObserved
                && !SameEndpoint(mapped, g_currentSource))
            {
                // Only a coordinator mapping for the internal port may change the source port.
                log::Warn(
                    "spectator",
                    "ignored unpinned direct punch mapped=%s pinned=%s role=%s",
                    FormatEndpoint(mapped).c_str(),
                    FormatEndpoint(g_currentSource).c_str(),
                    g_redirectActive ? "redirect-target" : "origin");
                continue;
            }
            g_currentSource = mapped;
            if (!g_redirectActive && SameRouteAddress(g_currentRoute, g_originRoute))
            {
                g_originSource = mapped;
            }
            g_status.assistedByAutopunch = true;
            log::Info(
                "spectator",
                "spectator route mapped role=%s source=%s internal=%u",
                g_redirectActive ? "redirect-target" : "origin",
                FormatEndpoint(g_currentSource).c_str(),
                static_cast<unsigned>(ntohs(g_currentRoute.sin_port)));
            SendHello();
            continue;
        }
        if (punch == trialnet::autopunch::InboundResult::Consumed)
        {
            continue;
        }
        HandleDatagramLocked(buffer, static_cast<std::size_t>(received), source);
    }
    if (datagrams == kReceiveDatagramBudget)
    {
        log::Warn("spectator", "receive budget exhausted packets=%d", datagrams);
    }

    // Resample after the loop: a stale tick wraps DWORD elapsed time.
    const DWORD serviceNow = GetTickCount();

    if (g_redirectActive && !g_status.connected && !g_streamInitialized
        && serviceNow - g_redirectStartedMs >= kRedirectTimeoutMs)
    {
        if (!ReturnToOriginLocked("target attach timed out after 12 seconds"))
        {
            SetFatalLocked("spectator redirect target timed out");
            return;
        }
        // Do not compare serviceNow against that later start time (DWORD underflow).
        return;
    }

    if (g_status.terminal)
    {
        g_autopunch.Stop("spectator source terminal received");
        return;
    }

    if (g_status.connected)
    {
        // Stay pinned: inactive punch traffic must not retarget a live viewer.
        if (g_lastReceiveMs != 0u && serviceNow - g_lastReceiveMs > 3000u)
        {
            if (!g_autopunch.IsActive())
            {
                g_autopunch.Start(serviceNow);
            }
            g_autopunch.Service(serviceNow);
        }
        else
        {
            g_autopunch.Stop("spectator host reached");
        }
        if (g_lastReceiveMs != 0u && serviceNow - g_lastReceiveMs > kServerTimeoutMs)
        {
            SetFatalLocked("spectator host timed out");
            return;
        }
    }
    else if (serviceNow - g_startedMs > kServerTimeoutMs)
    {
        SetFatalLocked("spectator attach timed out");
        return;
    }

    if (g_lastSendMs == 0u
        || serviceNow - g_lastSendMs >= (g_status.connected ? kKeepAliveIntervalMs : kHelloIntervalMs))
    {
        SendKeepAlive();
    }
}

bool PeekGameStart(protocol::GameStart* const outStart)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (outStart == nullptr || g_games.empty() || g_games.front().playbackStarted)
    {
        return false;
    }
    *outStart = g_games.front().start;
    return true;
}

bool MarkGameStarted(const std::uint32_t gameId)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_games.empty() || g_games.front().start.gameId != gameId)
    {
        return false;
    }
    g_games.front().playbackStarted = true;
    g_status.playingGameId = gameId;
    g_status.playingRecordIndex = g_games.front().nextPlayRecordIndex;
    g_status.playedInputDigest = g_games.front().playDigest;
    return true;
}

bool PeekFrame(protocol::ConfirmedInput* const outFrame)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (outFrame == nullptr || g_games.empty() || !g_games.front().playbackStarted
        || g_games.front().frames.empty())
    {
        return false;
    }
    const auto& game = g_games.front();
    if (game.frames.front().recordIndex != game.nextPlayRecordIndex)
    {
        SetFatalLocked("spectator playback queue lost record alignment");
        return false;
    }
    *outFrame = game.frames.front();
    return true;
}

std::size_t ContiguousFramesForScreen(
    const std::uint8_t screen,
    const std::size_t maximum)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (maximum == 0u || g_games.empty() || !g_games.front().playbackStarted)
    {
        return 0u;
    }
    const ReceivedGame& game = g_games.front();
    std::size_t count = 0u;
    std::uint32_t expected = game.nextPlayRecordIndex;
    for (const protocol::ConfirmedInput& frame : game.frames)
    {
        if (count >= maximum || frame.recordIndex != expected || frame.screen != screen)
        {
            break;
        }
        ++count;
        ++expected;
    }
    return count;
}

bool ConsumeFrame(const std::uint32_t gameId, const std::uint32_t recordIndex)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_games.empty() || !g_games.front().playbackStarted
        || g_games.front().start.gameId != gameId || g_games.front().frames.empty()
        || g_games.front().frames.front().recordIndex != recordIndex
        || g_games.front().nextPlayRecordIndex != recordIndex)
    {
        return false;
    }
    ReceivedGame& game = g_games.front();
    const protocol::ConfirmedInput frame = game.frames.front();
    if (!protocol::HasCanonicalRegionHashes(frame.hasRegionHashes, frame.regionHashes))
    {
        SetFatalLocked("spectator playback input carries non-canonical region hashes");
        return false;
    }
    const std::uint64_t expectedDigest = protocol::AdvanceInputDigest(
        game.playDigest,
        gameId,
        recordIndex,
        frame.sourceGeneration,
        frame.sourceFrame,
        frame.screen,
        frame.p1Word,
        frame.p2Word,
        frame.hasRegionHashes,
        frame.regionHashes);
    if (expectedDigest != frame.inputDigest)
    {
        SetFatalLocked("spectator playback input digest mismatch");
        return false;
    }
    game.playDigest = expectedDigest;
    game.frames.pop_front();
    ++game.nextPlayRecordIndex;
    --g_status.queuedFrames;
    g_status.playingRecordIndex = game.nextPlayRecordIndex;
    g_status.playedInputDigest = game.playDigest;
    if (((frame.p1Word | frame.p2Word) & 0xFF00u) != 0u)
    {
        log::Info(
            "spectator",
            "SPECTATOR SYSTEM INPUT playback game=%u record=%u source=%u p1=%04X p2=%04X",
            gameId,
            recordIndex,
            frame.sourceFrame,
            static_cast<unsigned>(frame.p1Word),
            static_cast<unsigned>(frame.p2Word));
    }
    if (recordIndex == 0u || ((recordIndex + 1u) % 256u) == 0u)
    {
        log::Info(
            "spectator",
            "SPECTATOR INPUT DIGEST playback game=%u records=%u digest=%016llX source=%u",
            gameId,
            recordIndex + 1u,
            static_cast<unsigned long long>(game.playDigest),
            frame.sourceFrame);
    }
    return true;
}

bool AdvanceCompletedGame()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_games.empty() || !g_games.front().playbackStarted
        || !g_games.front().complete || !g_games.front().frames.empty()
        || g_games.size() < 2u)
    {
        return false;
    }
    g_games.pop_front();
    g_status.queuedGames = g_games.size();
    g_status.playingGameId = 0u;
    g_status.playingRecordIndex = 0u;
    return true;
}

Status GetStatus()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

}
