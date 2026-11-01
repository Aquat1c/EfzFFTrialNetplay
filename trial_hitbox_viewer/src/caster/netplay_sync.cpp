#include "autopunch_client.h"
#include "autopunch_protocol.h"
#include "netplay_sync.h"

#include "caster_input.h"
#include "debug_log.h"
#include "desync_check.h"
#include "lifecycle_log.h"
#include "determinism.h"
#include "game_pacing.h"
#include "menu_nav.h"
#include "netplay_lockstep.h"
#include "netplay_navigation.h"
#include "rng.h"
#include "rollback.h"
#include "rollback_config.h"
#include "spectator_protocol.h"
#include "spectator_relay.h"
#include "test_harness.h"
#include "trial_protocol.h"
#include "win32_compat.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace thv::netplay
{

void CloseSync();

bool IsDeterminismSynced();

namespace
{

constexpr std::uint32_t kSeedSalt = 0x45465A4Eu; // 'EFZN'

std::atomic<int> g_winsockState{0}; // 0=not attempted, 1=ready, negative=WSA error

std::atomic<bool> g_active{false};
std::atomic<bool> g_launchGo{false};
std::atomic<bool> g_retransmitLaunchGo{false};
DWORD g_lastLaunchGoSend = 0;

constexpr DWORD kLaunchGoRetransmitMs = 500;
constexpr DWORD kSeedHandshakeRetransmitMs = 200;
constexpr int kUdpOverlapSends = 9;
std::atomic<bool> g_seedSynced{false};
std::atomic<bool> g_seedHandshakeConfirmed{false};
std::atomic<std::uint32_t> g_syncedSeed{0};
std::atomic<std::uint64_t> g_runtimeSessionId{0u};
std::atomic<std::uint32_t> g_syncFrame{0};
std::atomic<std::uint16_t> g_syncTimeline{0};
std::atomic<std::uint32_t> g_syncGeneration{0};
std::atomic<DWORD> g_lastSyncArrivalMs{0};
std::atomic<DWORD> g_lastSyncRecvMs{0};
std::atomic<std::uint32_t> g_syncProgressRemoteFrontier{0};
std::atomic<std::uint32_t> g_syncProgressAckFrontier{0};
std::atomic<std::uint16_t> g_syncProgressTimeline{0};
std::atomic<bool> g_syncProgressWasNeeded{false};
std::atomic<DWORD> g_lastPeerPacketMs{0};
std::atomic<DWORD> g_transportStartMs{0};
std::atomic<bool> g_rollbackNegotiated{false};
std::atomic<bool> g_netplayAborted{false};
std::atomic<bool> g_peerGracefulQuitAck{false};
// Publish g_netplayAborted only after the handoff: bootstrap then closes the process.
std::atomic<bool> g_peerGracefulEndHandling{false};
std::atomic<bool> g_sourceEndDrainStarted{false};
std::atomic<unsigned> g_terminalHandoffDepth{0u};
std::atomic<bool> g_abortOwnerClaimed{false};
// Host's value from LaunchGo; -1 = not negotiated yet.
std::atomic<int> g_negotiatedInputDelay{-1};
std::atomic<int> g_negotiatedRollbackFrames{-1};
std::atomic<int> g_negotiatedTargetFps{-1};
std::uint8_t g_requestedRollbackFrames = 0;
std::atomic<std::uint8_t> g_requestedTargetFps{proto::kDefaultTargetFps};

// No handshake before learning the client's address, or the host echoes itself.
std::atomic<bool> g_peerAddrLearned{false};
DWORD g_lastClientHelloMs = 0;
DWORD g_lastSeedHandshakeSendMs = 0;

// epoch keeps a stale advert from satisfying or wedging a new barrier.
std::atomic<bool> g_peerEntryKnown{false};
std::atomic<bool> g_peerEntryReady{false};
std::atomic<std::uint32_t> g_peerEntryFrame{0};
std::atomic<std::uint32_t> g_peerEntryEpoch{0};
std::atomic<DWORD> g_lastBattleEntrySendMs{0};

// Not one-shot: advertise the committed frame until new-timeline traffic arrives.
std::atomic<bool> g_entryCommitKnown{false};
std::atomic<bool> g_entryCommitPending{false};
std::atomic<std::uint32_t> g_entryCommitFrame{0};
std::atomic<std::uint32_t> g_entryCommitEpoch{0};
std::atomic<std::uint16_t> g_entryCommitTimeline{0};
std::atomic<DWORD> g_lastEntryCommitSendMs{0};

std::atomic<std::uint32_t> g_pktSentTotal{0};
std::atomic<std::uint32_t> g_pktRecvTotal{0};
std::atomic<std::uint32_t> g_syncSentTotal{0};
std::atomic<std::uint32_t> g_syncRecvTotal{0};

bool g_requireRollback = false;
bool g_peerRollbackAck = false;
char g_abortReason[160] = {};

bool g_isHost = false;
bool g_seedExchangeActive = false;
bool g_clientAckedSeed = false;
std::uint32_t g_hostSeed = 0;
std::uint32_t g_hostSeedNonce = 0;
std::uint32_t g_localClientNonce = 0;
std::uint32_t g_activeClientNonce = 0;
std::uint32_t g_acceptedHostNonce = 0;
std::uint32_t g_acceptedHostSeed = 0;
std::uint16_t g_port = 7500;
std::uint16_t g_peerPort = 7500;
std::uint16_t g_localBoundPort = 0u;
DWORD g_lastSpectatorRegisterMs = 0u;
std::string g_peerNarrow;
SOCKET g_socket = INVALID_SOCKET;
sockaddr_in g_peer{};
trialnet::autopunch::Client g_autopunch{};
// Recursive: receive handlers may reply.
std::recursive_mutex g_socketMutex;
std::recursive_mutex g_transportOwnerMutex;

constexpr std::size_t kMaxSpectators = 16u;
constexpr DWORD kSpectatorBatchRetransmitMs = 120u;
constexpr DWORD kSpectatorStartRetransmitMs = 250u;
constexpr DWORD kSpectatorKeepAliveMs = 1000u;
constexpr DWORD kSpectatorTimeoutMs = 60000u;
constexpr std::size_t kSpectatorWindowBatches = 16u;
constexpr std::size_t kSpectatorPacketsPerSubscriberPoll = 2u;
constexpr std::size_t kSpectatorPacketsPerPoll = 16u;
constexpr int kReceivePacketsPerPoll = 256;
constexpr std::size_t kSpectatorRelayDirectLimit = 8u;
constexpr DWORD kSpectatorNodeAdvertIntervalMs = 500u;
constexpr DWORD kSpectatorNodeAdvertFreshMs = 2000u;
constexpr DWORD kSpectatorGrantTtlMs = 15000u;
constexpr DWORD kSpectatorGrantRetransmitMs = 250u;
constexpr DWORD kSpectatorNormalTailMaxMs = 10u * 60u * 1000u;

struct SpectatorSubscriber
{
    sockaddr_in endpoint{};
    std::uint32_t sessionTag = 0u;
    std::uint32_t clientNonce = 0u;
    std::uint64_t initFingerprint = 0u;
    std::uint32_t gameId = 0u;
    std::uint32_t ackedRecordIndex = 0u;
    std::uint32_t maxSentRecordIndex = 0u;
    std::uint32_t nextSequence = 1u;
    DWORD lastReceiveMs = 0u;
    DWORD lastSendMs = 0u;
    DWORD lastStatusLogMs = 0u;
    DWORD lastAckProgressMs = 0u;
    DWORD lastRetransmitMs = 0u;
    DWORD lastWaitingSendMs = 0u;
    std::uint64_t packetsSent = 0u;
    std::uint64_t bytesSent = 0u;
    std::uint64_t retransmits = 0u;
    bool initAcked = false;
    bool gameStartAcked = false;
    bool terminalSent = false;
    bool terminalAcked = false;
    spectator::protocol::TerminalReason terminalReason =
        spectator::protocol::TerminalReason::SessionEnded;
    std::uint32_t terminalGameId = 0u;
    std::uint32_t terminalRecordIndex = 0u;
    std::uint64_t terminalInputDigest = spectator::protocol::kInputDigestSeed;
};

std::vector<SpectatorSubscriber> g_spectators;
std::uint32_t g_nextSpectatorSessionTag = 1u;
std::size_t g_spectatorRoundRobin = 0u;
bool g_spectatorTerminalSent = false;
spectator::protocol::TerminalReason g_spectatorTerminalReason =
    spectator::protocol::TerminalReason::SessionEnded;

struct PeerSpectatorNode
{
    bool valid = false;
    DWORD receivedMs = 0u;
    std::uint32_t relayEpoch = 0u;
    std::uint32_t gameId = 0u;
    std::uint32_t nextRecordIndex = 0u;
    std::uint64_t inputDigest = spectator::protocol::kInputDigestSeed;
    std::uint64_t gameStartFingerprint = 0u;
    std::uint16_t internalPort = 0u;
    std::uint8_t freeSlots = 0u;
};

struct HostSpectatorGrant
{
    sockaddr_in viewerEndpoint{};
    std::uint32_t relayEpoch = 0u;
    std::uint32_t grantId = 0u;
    std::uint32_t clientNonce = 0u;
    std::uint32_t requestedGameId = 0u;
    std::uint32_t nextRecordIndex = 0u;
    std::uint64_t expectedDigest = spectator::protocol::kInputDigestSeed;
    std::uint64_t gameStartFingerprint = 0u;
    std::uint32_t targetIpv4 = 0u;
    std::uint16_t targetInternalPort = 0u;
    std::array<std::uint8_t, 16> token{};
    DWORD expiresAtMs = 0u;
    DWORD lastGrantSendMs = 0u;
    DWORD lastRedirectSendMs = 0u;
    std::uint32_t nextRedirectSequence = 1u;
    bool ready = false;
    bool claimed = false;
    bool failed = false;
    bool relayRejected = false;
};

struct ClientSpectatorGrant
{
    proto::SpectatorGrantPacket packet{};
    proto::SpectatorGrantStatus status = proto::SpectatorGrantStatus::InvalidGrant;
    DWORD expiresAtMs = 0u;
    DWORD lastReplySendMs = 0u;
    sockaddr_in consumedEndpoint{};
    bool consumed = false;
};

PeerSpectatorNode g_peerSpectatorNode{};
std::vector<HostSpectatorGrant> g_hostSpectatorGrants;
std::vector<ClientSpectatorGrant> g_clientSpectatorGrants;
std::uint32_t g_spectatorRelayEpoch = 0u;
DWORD g_lastSpectatorNodeAdvertMs = 0u;

bool SendToSpectatorEndpoint(
    const sockaddr_in& endpoint,
    const void* data,
    std::size_t size);
bool SameEndpoint(const sockaddr_in& left, const sockaddr_in& right);

void AutopunchLog(void*, const char* const message)
{
    if (message == nullptr)
    {
        return;
    }
    lifecycle::Netplay("autopunch %s", message);
    log::Info("netplay", "autopunch %s", message);
}

void SendSpectatorTerminal(const spectator::protocol::TerminalReason reason)
{
    if (g_socket == INVALID_SOCKET)
    {
        return;
    }
    const bool firstSend = !g_spectatorTerminalSent;
    if (firstSend)
    {
        g_spectatorTerminalSent = true;
        g_spectatorTerminalReason = reason;
    }
    const spectator::Status status = spectator::GetStatus();
    spectator::GameExtent extent{};
    const bool hasExtent = status.gameId != 0u
        && spectator::GetGameExtent(status.gameId, &extent);
    for (SpectatorSubscriber& subscriber : g_spectators)
    {
        if (subscriber.terminalAcked)
        {
            continue;
        }
        if (!subscriber.terminalSent)
        {
            const std::uint32_t finalRecord = hasExtent
                ? static_cast<std::uint32_t>(extent.frameCount) : 0u;
            const bool complete = status.gameId == 0u
                ? subscriber.gameId == 0u
                : subscriber.initAcked && subscriber.gameId == status.gameId
                    && subscriber.ackedRecordIndex >= finalRecord;
            subscriber.terminalReason = complete
                ? g_spectatorTerminalReason
                : spectator::protocol::TerminalReason::ArchiveUnavailable;
            subscriber.terminalGameId = complete ? status.gameId : subscriber.gameId;
            subscriber.terminalRecordIndex = complete
                ? finalRecord : subscriber.ackedRecordIndex;
            if (subscriber.terminalGameId == 0u)
            {
                subscriber.terminalInputDigest = spectator::protocol::kInputDigestSeed;
            }
            else if (!spectator::GetInputDigestAt(
                    subscriber.terminalGameId,
                    subscriber.terminalRecordIndex,
                    &subscriber.terminalInputDigest))
            {
                subscriber.terminalReason =
                    spectator::protocol::TerminalReason::ArchiveUnavailable;
                subscriber.terminalGameId = 0u;
                subscriber.terminalRecordIndex = 0u;
                subscriber.terminalInputDigest = spectator::protocol::kInputDigestSeed;
            }
            subscriber.terminalSent = true;
            if (!complete)
            {
                log::Warn(
                    "spectator",
                    "terminal truncates lagging viewer tag=%08X cursor=%u:%u sourceTail=%u:%u",
                    subscriber.sessionTag,
                    subscriber.terminalGameId,
                    subscriber.terminalRecordIndex,
                    status.gameId,
                    finalRecord);
            }
        }

        spectator::protocol::Terminal terminal{};
        terminal.header.sessionTag = subscriber.sessionTag;
        terminal.header.sequence = subscriber.nextSequence++;
        terminal.reason = subscriber.terminalReason;
        terminal.finalGameId = subscriber.terminalGameId;
        terminal.finalRecordIndex = subscriber.terminalRecordIndex;
        terminal.finalInputDigest = subscriber.terminalInputDigest;
        terminal.totalConfirmedFrames = status.totalConfirmedFrames;
        for (int repeat = 0; repeat < (firstSend ? 3 : 1); ++repeat)
        {
            (void)SendToSpectatorEndpoint(
                subscriber.endpoint,
                &terminal,
                sizeof(terminal));
        }
    }
    if (firstSend)
    {
        log::Info(
            "spectator",
            "SPECTATOR SOURCE END sent reason=%u viewers=%zu game=%u records=%u digest=%016llX total=%zu",
            static_cast<unsigned>(g_spectatorTerminalReason),
            g_spectators.size(),
            status.gameId,
            hasExtent ? static_cast<unsigned>(extent.frameCount) : 0u,
            static_cast<unsigned long long>(status.latestInputDigest),
            status.totalConfirmedFrames);
    }
}

struct DelayedSend
{
    DWORD releaseMs = 0;
    sockaddr_in peer{};
    int overlapSends = 1;
    std::vector<std::uint8_t> bytes;
};
std::mutex g_delayedSendMutex;
std::vector<DelayedSend> g_delayedSends;

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
    const int written =
        WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, out.data(), needed, nullptr, nullptr);
    if (written <= 1)
    {
        return {};
    }
    out.resize(static_cast<std::size_t>(written - 1));
    return out;
}

bool SendToPeer(const void* data, const std::size_t size, const char* label, const int overlapSends = kUdpOverlapSends)
{
    std::lock_guard<std::recursive_mutex> socketLock(g_socketMutex);
    if (g_socket == INVALID_SOCKET)
    {
        return false;
    }

    if (harness::NetShimEnabled())
    {
        const harness::NetShimDecision decision = harness::DecideOutgoing();
        if (decision.drop)
        {
            g_pktSentTotal.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (decision.delayMs > 0)
        {
            DelayedSend pending{};
            pending.releaseMs = GetTickCount() + decision.delayMs;
            pending.peer = g_peer;
            pending.overlapSends = overlapSends > 0 ? overlapSends : 1;
            pending.bytes.assign(
                static_cast<const std::uint8_t*>(data),
                static_cast<const std::uint8_t*>(data) + size);
            {
                std::lock_guard<std::mutex> lock(g_delayedSendMutex);
                g_delayedSends.push_back(std::move(pending));
            }
            g_pktSentTotal.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }

    const char* bytes = static_cast<const char*>(data);
    const int repeats = overlapSends > 0 ? overlapSends : 1;
    for (int repeat = 0; repeat < repeats; ++repeat)
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const int sent = sendto(
                g_socket,
                bytes,
                static_cast<int>(size),
                0,
                reinterpret_cast<const sockaddr*>(&g_peer),
                sizeof(g_peer));
            if (sent == static_cast<int>(size))
            {
                break;
            }
            if (attempt == 2)
            {
                lifecycle::Fault("netplay", "send %s failed after retries", label);
                log::Warn("netplay", "send %s failed after retries", label);
                return false;
            }
            Sleep(1);
        }
    }

    g_pktSentTotal.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool TickDeadlinePassed(const DWORD now, const DWORD deadline)
{
    return deadline != 0u && static_cast<std::int32_t>(now - deadline) >= 0;
}

bool TokenIsZero(const std::array<std::uint8_t, 16>& token)
{
    std::uint8_t any = 0u;
    for (const std::uint8_t byte : token)
    {
        any = static_cast<std::uint8_t>(any | byte);
    }
    return any == 0u;
}

bool TokensEqualConstantTime(
    const std::array<std::uint8_t, 16>& left,
    const std::array<std::uint8_t, 16>& right)
{
    std::uint8_t difference = 0u;
    for (std::size_t index = 0u; index < left.size(); ++index)
    {
        difference = static_cast<std::uint8_t>(difference | (left[index] ^ right[index]));
    }
    return difference == 0u;
}

std::uint64_t CanonicalRelayGameFingerprint(
    const spectator::protocol::GameStart& source)
{
    spectator::protocol::GameStart canonical = source;
    canonical.sourceGeneration = 0u;
    canonical.sourceFrame = 0u;
    canonical.clientNonce = 0u;
    canonical.localSide = 0u;
    return spectator::protocol::GameStartFingerprint(canonical);
}

bool ResolveSpectatorCursorProof(
    const std::uint32_t requestedGameId,
    const std::uint32_t nextRecordIndex,
    std::uint32_t* const outGameId,
    std::uint64_t* const outDigest,
    std::uint64_t* const outFingerprint)
{
    if (outGameId == nullptr || outDigest == nullptr || outFingerprint == nullptr)
    {
        return false;
    }

    spectator::protocol::GameStart start{};
    if (!spectator::BuildGameStartMessage(requestedGameId, 0u, 0u, &start))
    {
        if (requestedGameId == 0u && nextRecordIndex == 0u)
        {
            *outGameId = 0u;
            *outDigest = spectator::protocol::kInputDigestSeed;
            *outFingerprint = 0u;
            return true;
        }
        return false;
    }

    spectator::GameExtent extent{};
    if (!spectator::GetGameExtent(start.gameId, &extent)
        || nextRecordIndex > extent.frameCount
        || !spectator::GetInputDigestAt(start.gameId, nextRecordIndex, outDigest))
    {
        return false;
    }
    *outGameId = start.gameId;
    *outFingerprint = CanonicalRelayGameFingerprint(start);
    return true;
}

std::size_t ClientReservedRelaySlots(const DWORD now)
{
    return static_cast<std::size_t>(std::count_if(
        g_clientSpectatorGrants.begin(),
        g_clientSpectatorGrants.end(),
        [now](const ClientSpectatorGrant& grant) {
            return grant.status == proto::SpectatorGrantStatus::Ready
                && !grant.consumed && !TickDeadlinePassed(now, grant.expiresAtMs);
        }));
}

std::uint8_t LocalSpectatorFreeSlots(const DWORD now)
{
    const std::size_t used = g_spectators.size() + ClientReservedRelaySlots(now);
    return static_cast<std::uint8_t>(
        used < kMaxSpectators ? kMaxSpectators - used : 0u);
}

std::size_t SpectatorRelaySoftLimit()
{
    return harness::IsEnabled() ? 1u : kSpectatorRelayDirectLimit;
}

bool BuildLocalSpectatorNodeAdvert(proto::SpectatorNodeAdvertPacket* const outAdvert)
{
    if (outAdvert == nullptr || g_spectatorRelayEpoch == 0u)
    {
        return false;
    }
    proto::SpectatorNodeAdvertPacket advert{};
    advert.relayEpoch = g_spectatorRelayEpoch;
    advert.internalPort = g_localBoundPort;
    advert.freeSlots = LocalSpectatorFreeSlots(GetTickCount());

    const spectator::Status status = spectator::GetStatus();
    std::uint32_t resolvedGameId = 0u;
    if (!ResolveSpectatorCursorProof(
            status.firstGameId,
            0u,
            &resolvedGameId,
            &advert.inputDigest,
            &advert.gameStartFingerprint))
    {
        return false;
    }
    advert.gameId = resolvedGameId;
    advert.nextRecordIndex = 0u;
    if (status.hostArchiveActive && advert.freeSlots != 0u)
    {
        advert.flags |= proto::kSpectatorNodeCanServe;
    }
    *outAdvert = advert;
    return true;
}

bool ValidatePeerSpectatorNodeAdvert(
    const proto::SpectatorNodeAdvertPacket& advert,
    const DWORD now)
{
    if (advert.relayEpoch == 0u || advert.relayEpoch != g_spectatorRelayEpoch
        || advert.internalPort == 0u
        || (advert.flags & proto::kSpectatorNodeCanServe) == 0u
        || advert.freeSlots == 0u || advert.freeSlots > kMaxSpectators)
    {
        return false;
    }
    std::uint32_t localGameId = 0u;
    std::uint64_t localDigest = 0u;
    std::uint64_t localFingerprint = 0u;
    const bool proofAvailable = ResolveSpectatorCursorProof(
        advert.gameId,
        advert.nextRecordIndex,
        &localGameId,
        &localDigest,
        &localFingerprint);
    const bool valid = proofAvailable && localGameId == advert.gameId
        && localDigest == advert.inputDigest
        && localFingerprint == advert.gameStartFingerprint;
    if (!valid)
    {
        log::Warn(
            "spectator",
            "relay node proof rejected epoch=%08X game=%u cursor=%u digest=%016llX/%016llX fingerprint=%016llX/%016llX",
            advert.relayEpoch,
            advert.gameId,
            advert.nextRecordIndex,
            static_cast<unsigned long long>(advert.inputDigest),
            static_cast<unsigned long long>(localDigest),
            static_cast<unsigned long long>(advert.gameStartFingerprint),
            static_cast<unsigned long long>(localFingerprint));
        return false;
    }
    g_peerSpectatorNode.valid = true;
    g_peerSpectatorNode.receivedMs = now;
    g_peerSpectatorNode.relayEpoch = advert.relayEpoch;
    g_peerSpectatorNode.gameId = advert.gameId;
    g_peerSpectatorNode.nextRecordIndex = advert.nextRecordIndex;
    g_peerSpectatorNode.inputDigest = advert.inputDigest;
    g_peerSpectatorNode.gameStartFingerprint = advert.gameStartFingerprint;
    g_peerSpectatorNode.internalPort = advert.internalPort;
    g_peerSpectatorNode.freeSlots = advert.freeSlots;
    return true;
}

std::vector<HostSpectatorGrant>::iterator FindHostSpectatorGrant(
    const std::uint32_t grantId)
{
    return std::find_if(
        g_hostSpectatorGrants.begin(),
        g_hostSpectatorGrants.end(),
        [grantId](const HostSpectatorGrant& grant) { return grant.grantId == grantId; });
}

std::vector<ClientSpectatorGrant>::iterator FindClientSpectatorGrant(
    const std::uint32_t grantId)
{
    return std::find_if(
        g_clientSpectatorGrants.begin(),
        g_clientSpectatorGrants.end(),
        [grantId](const ClientSpectatorGrant& grant) {
            return grant.packet.grantId == grantId;
        });
}

std::uint32_t AllocateSpectatorGrantId()
{
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        std::uint32_t id = 0u;
        if (!win32::SecureRandomBytes(&id, sizeof(id)))
        {
            return 0u;
        }
        if (id != 0u && FindHostSpectatorGrant(id) == g_hostSpectatorGrants.end())
        {
            return id;
        }
    }
    return 0u;
}

bool SameGrantOffer(
    const proto::SpectatorGrantPacket& left,
    const proto::SpectatorGrantPacket& right)
{
    return left.wireVersion == right.wireVersion && left.hop == right.hop
        && left.relayEpoch == right.relayEpoch && left.grantId == right.grantId
        && left.clientNonce == right.clientNonce
        && left.requestedGameId == right.requestedGameId
        && left.nextRecordIndex == right.nextRecordIndex
        && left.viewerIpv4 == right.viewerIpv4
        && left.expectedDigest == right.expectedDigest
        && left.gameStartFingerprint == right.gameStartFingerprint
        && TokensEqualConstantTime(left.token, right.token);
}

bool MaybeQueueSpectatorRedirect(
    const spectator::protocol::Hello& hello,
    const sockaddr_in& from)
{
    // Redirect is pre-GameStart only: each recorder's rolling digest is source-local.
    if (!g_isHost || hello.requestedGameId != 0u || hello.nextRecordIndex != 0u
        || g_spectators.size() < SpectatorRelaySoftLimit())
    {
        return false;
    }
    const DWORD now = GetTickCount();
    const auto existing = std::find_if(
        g_hostSpectatorGrants.begin(),
        g_hostSpectatorGrants.end(),
        [&hello, &from](const HostSpectatorGrant& grant) {
            return grant.clientNonce == hello.clientNonce
                && grant.requestedGameId == hello.requestedGameId
                && grant.nextRecordIndex == hello.nextRecordIndex
                && grant.viewerEndpoint.sin_addr.S_un.S_addr
                    == from.sin_addr.S_un.S_addr;
        });
    if (existing != g_hostSpectatorGrants.end())
    {
        if (!existing->claimed)
        {
            existing->viewerEndpoint = from;
        }
        return existing->claimed || (!existing->failed && !existing->relayRejected
            && !TickDeadlinePassed(now, existing->expiresAtMs));
    }

    if (!g_peerSpectatorNode.valid || g_peerSpectatorNode.relayEpoch == 0u
        || now - g_peerSpectatorNode.receivedMs > kSpectatorNodeAdvertFreshMs
        || g_peerSpectatorNode.freeSlots == 0u)
    {
        return false;
    }
    const std::size_t reserved = static_cast<std::size_t>(std::count_if(
        g_hostSpectatorGrants.begin(),
        g_hostSpectatorGrants.end(),
        [now](const HostSpectatorGrant& grant) {
            // Count only unreserved offers: ready grants are already in the peer's free-slot ACK.
            return !grant.ready && !grant.claimed && !grant.failed && !grant.relayRejected
                && !TickDeadlinePassed(now, grant.expiresAtMs);
        }));
    if (reserved >= g_peerSpectatorNode.freeSlots)
    {
        return false;
    }

    std::uint32_t resolvedGameId = 0u;
    std::uint64_t expectedDigest = 0u;
    std::uint64_t gameStartFingerprint = 0u;
    if (!ResolveSpectatorCursorProof(
            hello.requestedGameId,
            hello.nextRecordIndex,
            &resolvedGameId,
            &expectedDigest,
            &gameStartFingerprint))
    {
        return false;
    }
    (void)resolvedGameId; // the grant deliberately preserves requestedGameId=0

    HostSpectatorGrant grant{};
    grant.viewerEndpoint = from;
    grant.relayEpoch = g_peerSpectatorNode.relayEpoch;
    grant.grantId = AllocateSpectatorGrantId();
    grant.clientNonce = hello.clientNonce;
    grant.requestedGameId = hello.requestedGameId;
    grant.nextRecordIndex = hello.nextRecordIndex;
    grant.expectedDigest = expectedDigest;
    grant.gameStartFingerprint = gameStartFingerprint;
    grant.targetIpv4 = g_peer.sin_addr.S_un.S_addr;
    grant.targetInternalPort = g_peerSpectatorNode.internalPort;
    grant.expiresAtMs = now + kSpectatorGrantTtlMs;
    if (grant.grantId == 0u
        || !win32::SecureRandomBytes(grant.token.data(), grant.token.size())
        || TokenIsZero(grant.token))
    {
        log::Warn(
            "spectator",
            "relay grant secure random unavailable; viewer remains direct nonce=%08X",
            hello.clientNonce);
        return false;
    }
    g_hostSpectatorGrants.push_back(grant);
    log::Info(
        "spectator",
        "SPECTATOR REDIRECT GRANT created id=%08X epoch=%08X viewer=%08X nonce=%08X cursor=%u:%u digest=%016llX fingerprint=%016llX target=%08X:%u ttl=%lu",
        grant.grantId,
        grant.relayEpoch,
        ntohl(from.sin_addr.S_un.S_addr),
        grant.clientNonce,
        grant.requestedGameId,
        grant.nextRecordIndex,
        static_cast<unsigned long long>(grant.expectedDigest),
        static_cast<unsigned long long>(grant.gameStartFingerprint),
        ntohl(grant.targetIpv4),
        static_cast<unsigned>(grant.targetInternalPort),
        static_cast<unsigned long>(kSpectatorGrantTtlMs));
    return true;
}

bool AuthorizeSpectatorFallbackHello(
    const spectator::protocol::Hello& hello,
    const sockaddr_in& from)
{
    if (!g_isHost || hello.relayEpoch == 0u || hello.grantId != 0u
        || hello.failedGrantId == 0u || TokenIsZero(hello.token))
    {
        return false;
    }
    const auto it = FindHostSpectatorGrant(hello.failedGrantId);
    if (it == g_hostSpectatorGrants.end()
        || it->relayEpoch != hello.relayEpoch || it->claimed
        || it->clientNonce != hello.clientNonce
        || it->requestedGameId != hello.requestedGameId
        || it->nextRecordIndex != hello.nextRecordIndex
        || it->viewerEndpoint.sin_addr.S_un.S_addr
            != from.sin_addr.S_un.S_addr
        || !TokensEqualConstantTime(it->token, hello.token))
    {
        return false;
    }
    if (!it->failed)
    {
        it->failed = true;
        log::Info(
            "spectator",
            "SPECTATOR REDIRECT fallback authenticated id=%08X epoch=%08X viewer=%08X nonce=%08X cursor=%u:%u",
            it->grantId,
            it->relayEpoch,
            ntohl(from.sin_addr.S_un.S_addr),
            hello.clientNonce,
            hello.requestedGameId,
            hello.nextRecordIndex);
    }
    return true;
}

bool AuthorizeTicketedSpectatorHello(
    const spectator::protocol::Hello& hello,
    const sockaddr_in& from)
{
    if (g_isHost || hello.relayEpoch == 0u || hello.grantId == 0u
        || hello.failedGrantId != 0u || TokenIsZero(hello.token))
    {
        return false;
    }
    const DWORD now = GetTickCount();
    const auto it = FindClientSpectatorGrant(hello.grantId);
    if (it == g_clientSpectatorGrants.end()
        || it->status != proto::SpectatorGrantStatus::Ready
        || TickDeadlinePassed(now, it->expiresAtMs)
        || it->packet.relayEpoch != hello.relayEpoch
        || it->packet.clientNonce != hello.clientNonce
        || it->packet.requestedGameId != hello.requestedGameId
        || it->packet.nextRecordIndex != hello.nextRecordIndex
        || it->packet.viewerIpv4 != from.sin_addr.S_un.S_addr
        || !TokensEqualConstantTime(it->packet.token, hello.token))
    {
        return false;
    }
    if (it->consumed)
    {
        const bool subscriberStillBound = std::any_of(
            g_spectators.begin(),
            g_spectators.end(),
            [&hello, &from](const SpectatorSubscriber& subscriber) {
                return subscriber.clientNonce == hello.clientNonce
                    && SameEndpoint(subscriber.endpoint, from);
            });
        return subscriberStillBound && SameEndpoint(it->consumedEndpoint, from);
    }

    std::uint32_t resolvedGameId = 0u;
    std::uint64_t localDigest = 0u;
    std::uint64_t localFingerprint = 0u;
    // A ready grant already owns a reserved slot; no extra free slot is required.
    const bool proofMatches = ResolveSpectatorCursorProof(
            hello.requestedGameId,
            hello.nextRecordIndex,
            &resolvedGameId,
            &localDigest,
            &localFingerprint)
        && localDigest == it->packet.expectedDigest
        && localFingerprint == it->packet.gameStartFingerprint;
    (void)resolvedGameId;
    return g_spectators.size() < kMaxSpectators && proofMatches;
}

void MarkTicketedSpectatorHelloConsumed(
    const spectator::protocol::Hello& hello,
    const sockaddr_in& from)
{
    const auto it = FindClientSpectatorGrant(hello.grantId);
    if (it == g_clientSpectatorGrants.end() || it->consumed)
    {
        return;
    }
    it->consumed = true;
    it->consumedEndpoint = from;
    it->lastReplySendMs = 0u;
    log::Info(
        "spectator",
        "SPECTATOR REDIRECT ticket consumed id=%08X epoch=%08X viewer=%08X:%u nonce=%08X cursor=%u:%u digest=%016llX",
        it->packet.grantId,
        it->packet.relayEpoch,
        ntohl(from.sin_addr.S_un.S_addr),
        static_cast<unsigned>(ntohs(from.sin_port)),
        hello.clientNonce,
        hello.requestedGameId,
        hello.nextRecordIndex,
        static_cast<unsigned long long>(it->packet.expectedDigest));
}

void HandleSpectatorNodeAdvertPacket(
    const proto::SpectatorNodeAdvertPacket& advert,
    const DWORD now)
{
    if (advert.controlWord != proto::kActionSpectatorNodeAdvert
        || advert.wireVersion != proto::kRollbackWireVersion
        || advert.relayEpoch == 0u
        || (advert.flags
            & ~(proto::kSpectatorNodeCanServe | proto::kSpectatorNodeCoordinator)) != 0u
        || advert.nextRecordIndex != 0u
        || advert.inputDigest != spectator::protocol::kInputDigestSeed
        || !std::all_of(
            std::begin(advert.reserved),
            std::end(advert.reserved),
            [](const std::uint8_t byte) { return byte == 0u; }))
    {
        return;
    }
    if (g_isHost)
    {
        if ((advert.flags & proto::kSpectatorNodeCoordinator) != 0u
            || advert.relayEpoch != g_spectatorRelayEpoch)
        {
            return;
        }
        const bool wasValid = g_peerSpectatorNode.valid;
        const std::uint8_t oldFreeSlots = g_peerSpectatorNode.freeSlots;
        g_peerSpectatorNode.valid = false;
        if (ValidatePeerSpectatorNodeAdvert(advert, now)
            && (!wasValid || oldFreeSlots != advert.freeSlots))
        {
            log::Info(
                "spectator",
                "SPECTATOR RELAY NODE ready epoch=%08X internalPort=%u free=%u proof=%u:%u/%016llX fingerprint=%016llX",
                advert.relayEpoch,
                static_cast<unsigned>(advert.internalPort),
                static_cast<unsigned>(advert.freeSlots),
                advert.gameId,
                advert.nextRecordIndex,
                static_cast<unsigned long long>(advert.inputDigest),
                static_cast<unsigned long long>(advert.gameStartFingerprint));
        }
        return;
    }

    if ((advert.flags & proto::kSpectatorNodeCoordinator) == 0u)
    {
        return;
    }
    if (g_spectatorRelayEpoch != advert.relayEpoch)
    {
        const std::uint32_t previous = g_spectatorRelayEpoch;
        g_spectatorRelayEpoch = advert.relayEpoch;
        g_clientSpectatorGrants.clear();
        g_lastSpectatorNodeAdvertMs = 0u;
        log::Info(
            "spectator",
            "SPECTATOR RELAY epoch accepted old=%08X new=%08X",
            previous,
            g_spectatorRelayEpoch);
    }
}

void HandleSpectatorGrantPacket(
    const proto::SpectatorGrantPacket& packet,
    const DWORD now)
{
    if (g_isHost || packet.controlWord != proto::kActionSpectatorGrant)
    {
        return;
    }
    auto existing = FindClientSpectatorGrant(packet.grantId);
    if (existing != g_clientSpectatorGrants.end())
    {
        if (!SameGrantOffer(existing->packet, packet))
        {
            log::Warn(
                "spectator",
                "relay grant replay/collision rejected id=%08X epoch=%08X",
                packet.grantId,
                packet.relayEpoch);
            return;
        }
        const DWORD offeredDeadline = now + std::min<DWORD>(packet.ttlMs, kSpectatorGrantTtlMs);
        if (existing->expiresAtMs == 0u
            || static_cast<std::int32_t>(offeredDeadline - existing->expiresAtMs) < 0)
        {
            existing->expiresAtMs = offeredDeadline;
        }
        return;
    }

    ClientSpectatorGrant grant{};
    grant.packet = packet;
    grant.expiresAtMs = now + std::min<DWORD>(packet.ttlMs, kSpectatorGrantTtlMs);
    std::uint32_t resolvedGameId = 0u;
    std::uint64_t localDigest = 0u;
    std::uint64_t localFingerprint = 0u;
    const bool layoutValid = packet.wireVersion == proto::kRollbackWireVersion
        && packet.hop == 1u && packet.reserved == 0u && packet.relayEpoch != 0u
        && packet.relayEpoch == g_spectatorRelayEpoch
        && packet.grantId != 0u && packet.clientNonce != 0u
        && packet.requestedGameId == 0u && packet.nextRecordIndex == 0u
        && packet.viewerIpv4 != 0u && packet.ttlMs != 0u
        && packet.ttlMs <= kSpectatorGrantTtlMs && !TokenIsZero(packet.token);
    const bool archiveValid = layoutValid && ResolveSpectatorCursorProof(
        packet.requestedGameId,
        packet.nextRecordIndex,
        &resolvedGameId,
        &localDigest,
        &localFingerprint);
    (void)resolvedGameId;
    if (!layoutValid)
    {
        grant.status = proto::SpectatorGrantStatus::InvalidGrant;
    }
    else if (!archiveValid || localDigest != packet.expectedDigest
        || localFingerprint != packet.gameStartFingerprint)
    {
        grant.status = proto::SpectatorGrantStatus::ArchiveMismatch;
    }
    else if (LocalSpectatorFreeSlots(now) == 0u)
    {
        grant.status = proto::SpectatorGrantStatus::CapacityUnavailable;
    }
    else
    {
        grant.status = proto::SpectatorGrantStatus::Ready;
    }
    g_clientSpectatorGrants.push_back(grant);
    log::Info(
        "spectator",
        "SPECTATOR REDIRECT GRANT validated id=%08X epoch=%08X status=%u viewer=%08X nonce=%08X cursor=%u:%u digest=%016llX/%016llX fingerprint=%016llX/%016llX free=%u",
        packet.grantId,
        packet.relayEpoch,
        static_cast<unsigned>(grant.status),
        ntohl(packet.viewerIpv4),
        packet.clientNonce,
        packet.requestedGameId,
        packet.nextRecordIndex,
        static_cast<unsigned long long>(packet.expectedDigest),
        static_cast<unsigned long long>(localDigest),
        static_cast<unsigned long long>(packet.gameStartFingerprint),
        static_cast<unsigned long long>(localFingerprint),
        static_cast<unsigned>(LocalSpectatorFreeSlots(now)));
}

void HandleSpectatorGrantAckPacket(
    const proto::SpectatorGrantAckPacket& ack,
    const DWORD now)
{
    if (!g_isHost || ack.controlWord != proto::kActionSpectatorGrantAck
        || ack.wireVersion != proto::kRollbackWireVersion
        || (ack.status != proto::SpectatorGrantStatus::Ready
            && ack.status != proto::SpectatorGrantStatus::ArchiveMismatch
            && ack.status != proto::SpectatorGrantStatus::CapacityUnavailable
            && ack.status != proto::SpectatorGrantStatus::InvalidGrant)
        || !std::all_of(
            std::begin(ack.reserved),
            std::end(ack.reserved),
            [](const std::uint8_t byte) { return byte == 0u; }))
    {
        return;
    }
    const auto it = FindHostSpectatorGrant(ack.grantId);
    if (it == g_hostSpectatorGrants.end() || it->relayEpoch != ack.relayEpoch
        || it->clientNonce != ack.clientNonce
        || it->requestedGameId != ack.requestedGameId
        || it->nextRecordIndex != ack.nextRecordIndex
        || it->expectedDigest != ack.expectedDigest
        || it->gameStartFingerprint != ack.gameStartFingerprint
        || ack.freeSlots > kMaxSpectators
        || TickDeadlinePassed(now, it->expiresAtMs) || it->failed || it->claimed)
    {
        log::Warn(
            "spectator",
            "relay grant ACK rejected id=%08X epoch=%08X status=%u cursor=%u:%u",
            ack.grantId,
            ack.relayEpoch,
            static_cast<unsigned>(ack.status),
            ack.requestedGameId,
            ack.nextRecordIndex);
        return;
    }
    if (ack.status != proto::SpectatorGrantStatus::Ready)
    {
        if (!it->relayRejected)
        {
            it->relayRejected = true;
            log::Warn(
                "spectator",
                "SPECTATOR REDIRECT GRANT refused id=%08X epoch=%08X status=%u; direct fallback enabled",
                ack.grantId,
                ack.relayEpoch,
                static_cast<unsigned>(ack.status));
        }
        return;
    }
    if (!it->ready)
    {
        it->ready = true;
        it->lastRedirectSendMs = 0u;
        g_peerSpectatorNode.freeSlots = ack.freeSlots;
        log::Info(
            "spectator",
            "SPECTATOR REDIRECT GRANT ready id=%08X epoch=%08X viewer=%08X nonce=%08X cursor=%u:%u digest=%016llX free=%u",
            it->grantId,
            it->relayEpoch,
            ntohl(it->viewerEndpoint.sin_addr.S_un.S_addr),
            it->clientNonce,
            it->requestedGameId,
            it->nextRecordIndex,
            static_cast<unsigned long long>(it->expectedDigest),
            static_cast<unsigned>(ack.freeSlots));
    }
}

void HandleSpectatorClaimPacket(
    const proto::SpectatorClaimPacket& claim,
    const DWORD now)
{
    if (!g_isHost || claim.controlWord != proto::kActionSpectatorClaim
        || claim.wireVersion != proto::kRollbackWireVersion || claim.hop != 1u
        || claim.reserved != 0u)
    {
        return;
    }
    const auto it = FindHostSpectatorGrant(claim.grantId);
    if (it == g_hostSpectatorGrants.end() || !it->ready || it->failed
        || it->relayRejected || TickDeadlinePassed(now, it->expiresAtMs)
        || it->relayEpoch != claim.relayEpoch
        || it->clientNonce != claim.clientNonce
        || it->requestedGameId != claim.requestedGameId
        || it->nextRecordIndex != claim.nextRecordIndex
        || it->expectedDigest != claim.expectedDigest
        || !TokensEqualConstantTime(it->token, claim.token))
    {
        log::Warn(
            "spectator",
            "relay claim rejected id=%08X epoch=%08X nonce=%08X cursor=%u:%u",
            claim.grantId,
            claim.relayEpoch,
            claim.clientNonce,
            claim.requestedGameId,
            claim.nextRecordIndex);
        return;
    }
    if (!it->claimed)
    {
        it->claimed = true;
        log::Info(
            "spectator",
            "SPECTATOR REDIRECT CLAIM accepted id=%08X epoch=%08X viewer=%08X nonce=%08X cursor=%u:%u digest=%016llX",
            it->grantId,
            it->relayEpoch,
            ntohl(it->viewerEndpoint.sin_addr.S_un.S_addr),
            it->clientNonce,
            it->requestedGameId,
            it->nextRecordIndex,
            static_cast<unsigned long long>(it->expectedDigest));
    }
}

bool HandleSpectatorPlayerControl(
    const std::uint8_t* const data,
    const std::size_t size)
{
    if (data == nullptr || size == 0u)
    {
        return false;
    }
    const DWORD now = GetTickCount();
    if (data[0] == proto::kActionSpectatorNodeAdvert
        && size == sizeof(proto::SpectatorNodeAdvertPacket))
    {
        proto::SpectatorNodeAdvertPacket packet{};
        std::memcpy(&packet, data, sizeof(packet));
        HandleSpectatorNodeAdvertPacket(packet, now);
        return true;
    }
    if (data[0] == proto::kActionSpectatorGrant
        && size == sizeof(proto::SpectatorGrantPacket))
    {
        proto::SpectatorGrantPacket packet{};
        std::memcpy(&packet, data, sizeof(packet));
        HandleSpectatorGrantPacket(packet, now);
        return true;
    }
    if (data[0] == proto::kActionSpectatorGrantAck
        && size == sizeof(proto::SpectatorGrantAckPacket))
    {
        proto::SpectatorGrantAckPacket packet{};
        std::memcpy(&packet, data, sizeof(packet));
        HandleSpectatorGrantAckPacket(packet, now);
        return true;
    }
    if (data[0] == proto::kActionSpectatorClaim
        && size == sizeof(proto::SpectatorClaimPacket))
    {
        proto::SpectatorClaimPacket packet{};
        std::memcpy(&packet, data, sizeof(packet));
        HandleSpectatorClaimPacket(packet, now);
        return true;
    }
    return data[0] >= proto::kActionSpectatorNodeAdvert
        && data[0] <= proto::kActionSpectatorClaim;
}

void ServiceSpectatorRelayControl()
{
    if (!IsDeterminismSynced() || g_socket == INVALID_SOCKET
        || !g_peerAddrLearned.load(std::memory_order_acquire))
    {
        return;
    }
    const DWORD now = GetTickCount();
    if (g_spectatorRelayEpoch != 0u
        && (g_lastSpectatorNodeAdvertMs == 0u
            || now - g_lastSpectatorNodeAdvertMs >= kSpectatorNodeAdvertIntervalMs))
    {
        proto::SpectatorNodeAdvertPacket advert{};
        if (BuildLocalSpectatorNodeAdvert(&advert))
        {
            if (g_isHost)
            {
                advert.flags |= proto::kSpectatorNodeCoordinator;
            }
            if (SendToPeer(&advert, sizeof(advert), "spectator-node-advert", 1))
            {
                g_lastSpectatorNodeAdvertMs = now;
            }
        }
    }

    if (g_isHost)
    {
        for (HostSpectatorGrant& grant : g_hostSpectatorGrants)
        {
            if (grant.failed || grant.relayRejected || grant.claimed
                || TickDeadlinePassed(now, grant.expiresAtMs))
            {
                continue;
            }
            if (!grant.ready)
            {
                if (grant.lastGrantSendMs != 0u
                    && now - grant.lastGrantSendMs < kSpectatorGrantRetransmitMs)
                {
                    continue;
                }
                proto::SpectatorGrantPacket packet{};
                packet.relayEpoch = grant.relayEpoch;
                packet.grantId = grant.grantId;
                packet.clientNonce = grant.clientNonce;
                packet.requestedGameId = grant.requestedGameId;
                packet.nextRecordIndex = grant.nextRecordIndex;
                packet.viewerIpv4 = grant.viewerEndpoint.sin_addr.S_un.S_addr;
                packet.ttlMs = static_cast<DWORD>(grant.expiresAtMs - now);
                packet.expectedDigest = grant.expectedDigest;
                packet.gameStartFingerprint = grant.gameStartFingerprint;
                packet.token = grant.token;
                if (SendToPeer(&packet, sizeof(packet), "spectator-grant", 1))
                {
                    grant.lastGrantSendMs = now;
                }
                continue;
            }
            if (grant.lastRedirectSendMs != 0u
                && now - grant.lastRedirectSendMs < kSpectatorGrantRetransmitMs)
            {
                continue;
            }
            spectator::protocol::Redirect redirect{};
            redirect.header.sequence = grant.nextRedirectSequence++;
            redirect.clientNonce = grant.clientNonce;
            redirect.relayEpoch = grant.relayEpoch;
            redirect.grantId = grant.grantId;
            redirect.requestedGameId = grant.requestedGameId;
            redirect.nextRecordIndex = grant.nextRecordIndex;
            redirect.expectedDigest = grant.expectedDigest;
            redirect.targetIpv4 = grant.targetIpv4;
            redirect.targetInternalPort = grant.targetInternalPort;
            redirect.hop = 1u;
            redirect.token = grant.token;
            if (SendToSpectatorEndpoint(
                    grant.viewerEndpoint,
                    &redirect,
                    sizeof(redirect)))
            {
                grant.lastRedirectSendMs = now;
                log::Info(
                    "spectator",
                    "SPECTATOR REDIRECT sent id=%08X epoch=%08X viewer=%08X:%u target=%08X:%u cursor=%u:%u digest=%016llX",
                    grant.grantId,
                    grant.relayEpoch,
                    ntohl(grant.viewerEndpoint.sin_addr.S_un.S_addr),
                    static_cast<unsigned>(ntohs(grant.viewerEndpoint.sin_port)),
                    ntohl(grant.targetIpv4),
                    static_cast<unsigned>(grant.targetInternalPort),
                    grant.requestedGameId,
                    grant.nextRecordIndex,
                    static_cast<unsigned long long>(grant.expectedDigest));
            }
        }
        g_hostSpectatorGrants.erase(
            std::remove_if(
                g_hostSpectatorGrants.begin(),
                g_hostSpectatorGrants.end(),
                [now](const HostSpectatorGrant& grant) {
                    // Keep expired tickets one more TTL so a fallback can name the failed grant.
                    return TickDeadlinePassed(
                        now,
                        grant.expiresAtMs + kSpectatorGrantTtlMs);
                }),
            g_hostSpectatorGrants.end());
        return;
    }

    for (ClientSpectatorGrant& grant : g_clientSpectatorGrants)
    {
        if (TickDeadlinePassed(now, grant.expiresAtMs)
            || (grant.lastReplySendMs != 0u
                && now - grant.lastReplySendMs < kSpectatorGrantRetransmitMs))
        {
            continue;
        }
        if (!grant.consumed)
        {
            proto::SpectatorGrantAckPacket ack{};
            ack.status = grant.status;
            ack.freeSlots = LocalSpectatorFreeSlots(now);
            ack.relayEpoch = grant.packet.relayEpoch;
            ack.grantId = grant.packet.grantId;
            ack.clientNonce = grant.packet.clientNonce;
            ack.requestedGameId = grant.packet.requestedGameId;
            ack.nextRecordIndex = grant.packet.nextRecordIndex;
            ack.expectedDigest = grant.packet.expectedDigest;
            ack.gameStartFingerprint = grant.packet.gameStartFingerprint;
            if (SendToPeer(&ack, sizeof(ack), "spectator-grant-ack", 1))
            {
                grant.lastReplySendMs = now;
            }
            continue;
        }
        proto::SpectatorClaimPacket claim{};
        claim.relayEpoch = grant.packet.relayEpoch;
        claim.grantId = grant.packet.grantId;
        claim.clientNonce = grant.packet.clientNonce;
        claim.requestedGameId = grant.packet.requestedGameId;
        claim.nextRecordIndex = grant.packet.nextRecordIndex;
        claim.expectedDigest = grant.packet.expectedDigest;
        claim.token = grant.packet.token;
        if (SendToPeer(&claim, sizeof(claim), "spectator-claim", 1))
        {
            grant.lastReplySendMs = now;
        }
    }
    g_clientSpectatorGrants.erase(
        std::remove_if(
            g_clientSpectatorGrants.begin(),
            g_clientSpectatorGrants.end(),
            [now](const ClientSpectatorGrant& grant) {
                return TickDeadlinePassed(now, grant.expiresAtMs);
            }),
        g_clientSpectatorGrants.end());
}

struct SpectatorPunchCandidate
{
    sockaddr_in endpoint{};
    DWORD observedMs = 0u;
};
std::vector<SpectatorPunchCandidate> g_spectatorPunchCandidates;

bool HandleSpectatorAutopunchControl(
    const std::uint8_t* const data,
    const std::size_t size,
    const sockaddr_in& from)
{
    if (data == nullptr)
    {
        return false;
    }
    sockaddr_in coordinator{};
    coordinator.sin_family = AF_INET;
    coordinator.sin_port = htons(trialnet::autopunch::kCoordinatorPort);
    (void)InetPtonA(
        AF_INET,
        trialnet::autopunch::kCoordinatorIpv4,
        &coordinator.sin_addr);
    const bool fromCoordinator = from.sin_addr.S_un.S_addr
            == coordinator.sin_addr.S_un.S_addr
        && from.sin_port == coordinator.sin_port;
    if (fromCoordinator)
    {
        trialnet::autopunch::Mapping mapping{};
        if (!trialnet::autopunch::ParseMapping(data, size, &mapping))
        {
            return true;
        }
        // Never rewrites g_peer.
        const bool primaryGameplayMapping =
            mapping.hostNetworkOrder == g_peer.sin_addr.S_un.S_addr
            && (!IsDeterminismSynced()
                || mapping.natPort == ntohs(g_peer.sin_port)
                || mapping.internalPort == g_peerPort);
        if (primaryGameplayMapping)
        {
            return false;
        }

        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_addr.S_un.S_addr = mapping.hostNetworkOrder;
        endpoint.sin_port = htons(mapping.natPort);
        static constexpr std::uint8_t punch = 0u;
        for (int repeat = 0; repeat < 4; ++repeat)
        {
            (void)SendToSpectatorEndpoint(endpoint, &punch, sizeof(punch));
        }
        const auto existing = std::find_if(
            g_spectatorPunchCandidates.begin(),
            g_spectatorPunchCandidates.end(),
            [&endpoint](const SpectatorPunchCandidate& candidate) {
                return SameEndpoint(candidate.endpoint, endpoint);
            });
        if (existing != g_spectatorPunchCandidates.end())
        {
            existing->observedMs = GetTickCount();
        }
        else
        {
            if (g_spectatorPunchCandidates.size() >= 32u)
            {
                g_spectatorPunchCandidates.erase(g_spectatorPunchCandidates.begin());
            }
            g_spectatorPunchCandidates.push_back({endpoint, GetTickCount()});
        }
        log::Info(
            "spectator",
            "autopunch candidate internal=%u public=%08X:%u candidates=%zu",
            mapping.internalPort,
            ntohl(endpoint.sin_addr.S_un.S_addr),
            static_cast<unsigned>(ntohs(endpoint.sin_port)),
            g_spectatorPunchCandidates.size());
        return true;
    }

    if (!trialnet::autopunch::IsDirectPunch(data, size))
    {
        return false;
    }
    const DWORD now = GetTickCount();
    g_spectatorPunchCandidates.erase(
        std::remove_if(
            g_spectatorPunchCandidates.begin(),
            g_spectatorPunchCandidates.end(),
            [now](const SpectatorPunchCandidate& candidate) {
                return now - candidate.observedMs > 30000u;
            }),
        g_spectatorPunchCandidates.end());
    const auto candidate = std::find_if(
        g_spectatorPunchCandidates.begin(),
        g_spectatorPunchCandidates.end(),
        [&from](const SpectatorPunchCandidate& item) {
            return SameEndpoint(item.endpoint, from);
        });
    if (candidate == g_spectatorPunchCandidates.end())
    {
        return false;
    }
    candidate->observedMs = now;
    static constexpr std::uint8_t punch = 0u;
    (void)SendToSpectatorEndpoint(from, &punch, sizeof(punch));
    return true;
}

bool SameEndpoint(const sockaddr_in& left, const sockaddr_in& right)
{
    return left.sin_family == right.sin_family
        && left.sin_addr.S_un.S_addr == right.sin_addr.S_un.S_addr
        && left.sin_port == right.sin_port;
}

bool SendToSpectatorEndpoint(
    const sockaddr_in& endpoint,
    const void* const data,
    const std::size_t size)
{
    std::lock_guard<std::recursive_mutex> socketLock(g_socketMutex);
    if (g_socket == INVALID_SOCKET || data == nullptr || size == 0u || size > 1200u)
    {
        return false;
    }
    const int sent = sendto(
        g_socket,
        static_cast<const char*>(data),
        static_cast<int>(size),
        0,
        reinterpret_cast<const sockaddr*>(&endpoint),
        sizeof(endpoint));
    if (sent != static_cast<int>(size))
    {
        return false;
    }
    g_pktSentTotal.fetch_add(1u, std::memory_order_relaxed);
    return true;
}

void SendSpectatorReject(
    const sockaddr_in& endpoint,
    const spectator::protocol::RejectReason reason,
    const std::uint32_t requestedGameId,
    const std::uint32_t sequence)
{
    spectator::protocol::Reject reject{};
    reject.header.sequence = sequence;
    reject.reason = reason;
    reject.requestedGameId = requestedGameId;
    (void)SendToSpectatorEndpoint(endpoint, &reject, sizeof(reject));
}

std::vector<SpectatorSubscriber>::iterator FindSpectator(const sockaddr_in& endpoint)
{
    return std::find_if(
        g_spectators.begin(),
        g_spectators.end(),
        [&endpoint](const SpectatorSubscriber& subscriber) {
            return SameEndpoint(subscriber.endpoint, endpoint);
        });
}

std::vector<SpectatorSubscriber>::iterator FindSpectatorByNonce(
    const sockaddr_in& endpoint,
    const std::uint32_t nonce)
{
    return std::find_if(
        g_spectators.begin(),
        g_spectators.end(),
        [&endpoint, nonce](const SpectatorSubscriber& subscriber) {
            return nonce != 0u && subscriber.clientNonce == nonce
                && subscriber.endpoint.sin_addr.S_un.S_addr
                    == endpoint.sin_addr.S_un.S_addr;
        });
}

std::uint32_t AllocateSpectatorSessionTag(const sockaddr_in& endpoint, const std::uint32_t nonce)
{
    std::uint32_t tag = ++g_nextSpectatorSessionTag
        ^ static_cast<std::uint32_t>(GetTickCount())
        ^ endpoint.sin_addr.S_un.S_addr
        ^ (static_cast<std::uint32_t>(endpoint.sin_port) << 16u)
        ^ nonce;
    if (tag == 0u)
    {
        tag = ++g_nextSpectatorSessionTag;
    }
    return tag;
}

bool HandleSpectatorDatagram(
    const std::uint8_t* const data,
    const std::size_t size,
    const sockaddr_in& from)
{
    if (data == nullptr || size < sizeof(spectator::protocol::Header))
    {
        return false;
    }

    spectator::protocol::Header header{};
    std::memcpy(&header, data, sizeof(header));
    if (header.magic != spectator::protocol::kMagic)
    {
        return false;
    }

    // Consume spectator traffic before endpoint discovery.
    if (header.version != spectator::protocol::kVersion)
    {
        SendSpectatorReject(
            from,
            spectator::protocol::RejectReason::VersionMismatch,
            0u,
            header.sequence);
        return true;
    }

    const DWORD now = GetTickCount();
    if (header.kind == spectator::protocol::MessageKind::Hello)
    {
        if (size != sizeof(spectator::protocol::Hello))
        {
            SendSpectatorReject(
                from,
                spectator::protocol::RejectReason::InvalidRequest,
                0u,
                header.sequence);
            return true;
        }
        spectator::protocol::Hello hello{};
        std::memcpy(&hello, data, sizeof(hello));
        if (hello.clientNonce == 0u
            || hello.simulationContractId != spectator::protocol::kSimulationContractId)
        {
            SendSpectatorReject(
                from,
                spectator::protocol::RejectReason::SimulationMismatch,
                hello.requestedGameId,
                header.sequence);
            return true;
        }

        const bool tokenIsZero = TokenIsZero(hello.token);
        const bool normalHello = hello.relayEpoch == 0u && hello.grantId == 0u
            && hello.failedGrantId == 0u && tokenIsZero;
        const bool ticketedHello = hello.relayEpoch != 0u && hello.grantId != 0u
            && hello.failedGrantId == 0u && !tokenIsZero;
        const bool fallbackHello = hello.relayEpoch != 0u && hello.grantId == 0u
            && hello.failedGrantId != 0u && !tokenIsZero;
        if (!normalHello && !ticketedHello && !fallbackHello)
        {
            SendSpectatorReject(
                from,
                spectator::protocol::RejectReason::InvalidRequest,
                hello.requestedGameId,
                header.sequence);
            log::Warn(
                "spectator",
                "malformed spectator relay hello from=%08X:%u nonce=%08X epoch=%08X grant=%08X failed=%08X",
                ntohl(from.sin_addr.S_un.S_addr),
                static_cast<unsigned>(ntohs(from.sin_port)),
                hello.clientNonce,
                hello.relayEpoch,
                hello.grantId,
                hello.failedGrantId);
            return true;
        }
        if (ticketedHello && !AuthorizeTicketedSpectatorHello(hello, from))
        {
            SendSpectatorReject(
                from,
                spectator::protocol::RejectReason::InvalidRequest,
                hello.requestedGameId,
                header.sequence);
            log::Warn(
                "spectator",
                "ticketed spectator hello rejected from=%08X:%u nonce=%08X epoch=%08X grant=%08X cursor=%u:%u",
                ntohl(from.sin_addr.S_un.S_addr),
                static_cast<unsigned>(ntohs(from.sin_port)),
                hello.clientNonce,
                hello.relayEpoch,
                hello.grantId,
                hello.requestedGameId,
                hello.nextRecordIndex);
            return true;
        }
        if (fallbackHello && !AuthorizeSpectatorFallbackHello(hello, from))
        {
            SendSpectatorReject(
                from,
                spectator::protocol::RejectReason::InvalidRequest,
                hello.requestedGameId,
                header.sequence);
            log::Warn(
                "spectator",
                "spectator redirect fallback rejected from=%08X:%u nonce=%08X epoch=%08X failed=%08X",
                ntohl(from.sin_addr.S_un.S_addr),
                static_cast<unsigned>(ntohs(from.sin_port)),
                hello.clientNonce,
                hello.relayEpoch,
                hello.failedGrantId);
            return true;
        }

        spectator::GameExtent extent{};
        if (hello.requestedGameId != 0u
            && (!spectator::GetGameExtent(hello.requestedGameId, &extent)
                || hello.nextRecordIndex > extent.frameCount))
        {
            SendSpectatorReject(
                from,
                spectator::protocol::RejectReason::GameNotRetained,
                hello.requestedGameId,
                header.sequence);
            return true;
        }

        auto it = FindSpectator(from);
        if (it == g_spectators.end())
        {
            it = FindSpectatorByNonce(from, hello.clientNonce);
        }
        if (it == g_spectators.end())
        {
            if (normalHello && !g_isHost)
            {
                SendSpectatorReject(
                    from,
                    spectator::protocol::RejectReason::InvalidRequest,
                    hello.requestedGameId,
                    header.sequence);
                return true;
            }
            if (normalHello && MaybeQueueSpectatorRedirect(hello, from))
            {
                return true;
            }
            if (g_spectators.size() >= kMaxSpectators)
            {
                SendSpectatorReject(
                    from,
                    spectator::protocol::RejectReason::ServerFull,
                    hello.requestedGameId,
                    header.sequence);
                return true;
            }
            g_spectators.emplace_back();
            it = std::prev(g_spectators.end());
            *it = {};
            it->endpoint = from;
            it->clientNonce = hello.clientNonce;
            it->sessionTag = AllocateSpectatorSessionTag(from, hello.clientNonce);
            it->gameId = hello.requestedGameId;
            it->ackedRecordIndex = hello.nextRecordIndex;
            it->maxSentRecordIndex = hello.nextRecordIndex;
            it->lastReceiveMs = now;
            it->lastAckProgressMs = now;
            log::Info(
                "spectator",
                "subscriber attach addr=%08X:%u tag=%08X nonce=%08X requested=%u:%u active=%zu relayGrant=%08X",
                ntohl(from.sin_addr.S_un.S_addr),
                static_cast<unsigned>(ntohs(from.sin_port)),
                it->sessionTag,
                it->clientNonce,
                hello.requestedGameId,
                hello.nextRecordIndex,
                g_spectators.size(),
                ticketedHello ? hello.grantId : 0u);
            if (ticketedHello)
            {
                MarkTicketedSpectatorHelloConsumed(hello, from);
            }
            return true;
        }

        if (it->clientNonce != hello.clientNonce)
        {
            *it = {};
            it->endpoint = from;
            it->clientNonce = hello.clientNonce;
            it->sessionTag = AllocateSpectatorSessionTag(from, hello.clientNonce);
            it->gameId = hello.requestedGameId;
            it->ackedRecordIndex = hello.nextRecordIndex;
            it->maxSentRecordIndex = hello.nextRecordIndex;
            it->lastAckProgressMs = now;
        }
        else
        {
            it->endpoint = from;
        }
        it->lastReceiveMs = now;
        log::Info(
            "spectator",
            "subscriber hello addr=%08X:%u tag=%08X nonce=%08X cursor=%u:%u init=%d active=%zu",
            ntohl(from.sin_addr.S_un.S_addr),
            static_cast<unsigned>(ntohs(from.sin_port)),
            it->sessionTag,
            it->clientNonce,
            it->gameId,
            it->ackedRecordIndex,
            it->initAcked ? 1 : 0,
            g_spectators.size());
        return true;
    }

    auto it = FindSpectator(from);
    if (it == g_spectators.end() || header.sessionTag == 0u
        || it->sessionTag != header.sessionTag)
    {
        return true;
    }
    if (header.kind == spectator::protocol::MessageKind::Detach)
    {
        if (size == sizeof(spectator::protocol::Detach))
        {
            it->lastReceiveMs = now;
            log::Info(
                "spectator",
                "subscriber detached tag=%08X game=%u cursor=%u",
                it->sessionTag,
                it->gameId,
                it->ackedRecordIndex);
            g_spectators.erase(it);
        }
        return true;
    }

    if (header.kind == spectator::protocol::MessageKind::InitAck)
    {
        if (size != sizeof(spectator::protocol::InitAck))
        {
            return true;
        }
        spectator::protocol::InitAck ack{};
        std::memcpy(&ack, data, sizeof(ack));
        if (ack.clientNonce != it->clientNonce || ack.gameId != it->gameId
            || ack.nextRecordIndex != it->ackedRecordIndex
            || ack.initFingerprint == 0u || ack.initFingerprint != it->initFingerprint)
        {
            log::Warn(
                "spectator",
                "rejected init ack tag=%08X game=%u nonce=%08X fingerprint=%016llX",
                it->sessionTag,
                ack.gameId,
                ack.clientNonce,
                static_cast<unsigned long long>(ack.initFingerprint));
            return true;
        }
        it->lastReceiveMs = now;
        it->initAcked = true;
        it->gameStartAcked = true;
        it->lastAckProgressMs = now;
        log::Info(
            "spectator",
            "deterministic init acknowledged tag=%08X game=%u cursor=%u fingerprint=%016llX",
            it->sessionTag,
            it->gameId,
            it->ackedRecordIndex,
            static_cast<unsigned long long>(it->initFingerprint));
        return true;
    }

    if (header.kind == spectator::protocol::MessageKind::TerminalAck)
    {
        if (size != sizeof(spectator::protocol::TerminalAck))
        {
            return true;
        }
        spectator::protocol::TerminalAck ack{};
        std::memcpy(&ack, data, sizeof(ack));
        if (g_spectatorTerminalSent && it->terminalSent
            && ack.finalGameId == it->terminalGameId
            && ack.finalRecordIndex == it->terminalRecordIndex
            && ack.finalInputDigest == it->terminalInputDigest)
        {
            it->terminalAcked = true;
            it->lastReceiveMs = now;
        }
        return true;
    }

    if (header.kind == spectator::protocol::MessageKind::Ack
        || header.kind == spectator::protocol::MessageKind::KeepAlive)
    {
        std::uint32_t gameId = 0u;
        std::uint32_t nextRecordIndex = 0u;
        if (header.kind == spectator::protocol::MessageKind::Ack
            && size == sizeof(spectator::protocol::Ack))
        {
            spectator::protocol::Ack ack{};
            std::memcpy(&ack, data, sizeof(ack));
            gameId = ack.gameId;
            nextRecordIndex = ack.nextRecordIndex;
        }
        else if (header.kind == spectator::protocol::MessageKind::KeepAlive
            && size == sizeof(spectator::protocol::KeepAlive))
        {
            spectator::protocol::KeepAlive keepAlive{};
            std::memcpy(&keepAlive, data, sizeof(keepAlive));
            gameId = keepAlive.gameId;
            nextRecordIndex = keepAlive.nextRecordIndex;
        }
        else
        {
            return true;
        }

        if (gameId != it->gameId || nextRecordIndex < it->ackedRecordIndex
            || nextRecordIndex > it->maxSentRecordIndex)
        {
            return true;
        }
        it->lastReceiveMs = now;
        if (!it->gameStartAcked && nextRecordIndex != it->ackedRecordIndex)
        {
            return true;
        }
        if (!it->initAcked)
        {
            return true;
        }
        it->gameStartAcked = true;
        if (nextRecordIndex > it->ackedRecordIndex)
        {
            it->lastAckProgressMs = now;
        }
        it->ackedRecordIndex = nextRecordIndex;
        return true;
    }

    return true;
}

bool HandleLauncherPresenceQuery(
    const std::uint8_t* const data,
    const std::size_t size,
    const sockaddr_in& from)
{
    constexpr std::size_t kConnectionRequestBytes = 201u;
    constexpr std::uint8_t kActionConnect = 0u;
    constexpr std::uint8_t kActionSpectatorQuery = 8u;
    constexpr std::uint8_t kReplyInPlay = 1u;
    constexpr std::uint8_t kReplyVersionMismatch = 3u;
    constexpr std::uint32_t kTagEfz = 0x45465A4Eu;
    if (data == nullptr || size != kConnectionRequestBytes
        || (data[0] != kActionConnect && data[0] != kActionSpectatorQuery))
    {
        return false;
    }
    std::uint32_t identifier = 0u;
    std::uint16_t major = 0u;
    std::uint16_t minor = 0u;
    std::memcpy(&identifier, data + 1u, sizeof(identifier));
    std::memcpy(&major, data + 5u, sizeof(major));
    std::memcpy(&minor, data + 7u, sizeof(minor));
    if (identifier != kTagEfz)
    {
        return false;
    }
    // Presence stays host-only; the other peer must never appear joinable.
    if (!g_isHost)
    {
        return true;
    }
    const std::uint8_t reply =
        major == trialnet::compat::kLauncherProtocolMajor
            && minor == trialnet::compat::kCompatibilityVersion
        ? kReplyInPlay : kReplyVersionMismatch;
    (void)SendToSpectatorEndpoint(from, &reply, sizeof(reply));
    log::Info(
        "spectator",
        "launcher presence query kind=%u from=%08X:%u reply=%u",
        static_cast<unsigned>(data[0]),
        ntohl(from.sin_addr.S_un.S_addr),
        static_cast<unsigned>(ntohs(from.sin_port)),
        static_cast<unsigned>(reply));
    return true;
}

void PollSpectatorSubscribers()
{
    if (g_spectators.empty())
    {
        return;
    }

    const DWORD now = GetTickCount();
    for (std::size_t index = 0u; index < g_spectators.size();)
    {
        SpectatorSubscriber& subscriber = g_spectators[index];
        spectator::GameExtent cursorExtent{};
        const bool invalidCursor = subscriber.gameId != 0u
            && (!spectator::GetGameExtent(subscriber.gameId, &cursorExtent)
                || subscriber.ackedRecordIndex > cursorExtent.frameCount);
        if (now - subscriber.lastReceiveMs > kSpectatorTimeoutMs || invalidCursor)
        {
            if (invalidCursor)
            {
                SendSpectatorReject(
                    subscriber.endpoint,
                    spectator::protocol::RejectReason::GameNotRetained,
                    subscriber.gameId,
                    subscriber.nextSequence++);
            }
            log::Info(
                "spectator",
                "subscriber removed tag=%08X game=%u cursor=%u timeout=%d invalid=%d",
                subscriber.sessionTag,
                subscriber.gameId,
                subscriber.ackedRecordIndex,
                now - subscriber.lastReceiveMs > kSpectatorTimeoutMs ? 1 : 0,
                invalidCursor ? 1 : 0);
            g_spectators.erase(g_spectators.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        ++index;
    }
    if (g_spectators.empty())
    {
        g_spectatorRoundRobin = 0u;
        return;
    }

    std::size_t globalBudget = kSpectatorPacketsPerPoll;
    const std::size_t subscriberCount = g_spectators.size();
    const std::size_t startIndex = g_spectatorRoundRobin % subscriberCount;
    std::size_t visited = 0u;
    for (; visited < subscriberCount && globalBudget != 0u; ++visited)
    {
        SpectatorSubscriber& subscriber =
            g_spectators[(startIndex + visited) % subscriberCount];
        std::size_t subscriberBudget = kSpectatorPacketsPerSubscriberPoll;

        if (now - subscriber.lastStatusLogMs >= 1000u)
        {
            subscriber.lastStatusLogMs = now;
            spectator::GameExtent diagnosticExtent{};
            const bool hasExtent = subscriber.gameId != 0u
                && spectator::GetGameExtent(subscriber.gameId, &diagnosticExtent);
            const spectator::Status recorder = spectator::GetStatus();
            log::Info(
                "spectator",
                "subscriber status tag=%08X game=%u init=%d startAck=%d ack=%u maxSent=%u extent=%zu next=%d packets=%llu bytes=%llu retransmits=%llu recorder[game=%u current=%zu total=%zu last=%u:%u digest=%016llX]",
                subscriber.sessionTag,
                subscriber.gameId,
                subscriber.initAcked ? 1 : 0,
                subscriber.gameStartAcked ? 1 : 0,
                subscriber.ackedRecordIndex,
                subscriber.maxSentRecordIndex,
                hasExtent ? diagnosticExtent.frameCount : 0u,
                hasExtent && diagnosticExtent.hasNextGame ? 1 : 0,
                static_cast<unsigned long long>(subscriber.packetsSent),
                static_cast<unsigned long long>(subscriber.bytesSent),
                static_cast<unsigned long long>(subscriber.retransmits),
                recorder.gameId,
                recorder.confirmedFrames,
                recorder.totalConfirmedFrames,
                recorder.lastSourceGeneration,
                recorder.lastSourceFrame,
                static_cast<unsigned long long>(recorder.latestInputDigest));
        }

        if (subscriber.gameId == 0u)
        {
            spectator::protocol::GameStart first{};
            if (!spectator::BuildGameStartMessage(
                    0u,
                    subscriber.sessionTag,
                    subscriber.nextSequence,
                    &first))
            {
                if (now - subscriber.lastWaitingSendMs >= kSpectatorKeepAliveMs)
                {
                    spectator::protocol::Waiting waiting{};
                    waiting.header.sessionTag = subscriber.sessionTag;
                    waiting.header.sequence = subscriber.nextSequence++;
                    waiting.clientNonce = subscriber.clientNonce;
                    if (SendToSpectatorEndpoint(
                            subscriber.endpoint,
                            &waiting,
                            sizeof(waiting)))
                    {
                        subscriber.lastWaitingSendMs = now;
                        ++subscriber.packetsSent;
                        subscriber.bytesSent += sizeof(waiting);
                        --subscriberBudget;
                        --globalBudget;
                    }
                }
                continue;
            }
            subscriber.gameId = first.gameId;
            subscriber.ackedRecordIndex = 0u;
            subscriber.maxSentRecordIndex = 0u;
            subscriber.gameStartAcked = false;
            subscriber.lastSendMs = 0u;
        }

        if (!subscriber.gameStartAcked)
        {
            if (subscriber.lastSendMs == 0u
                || now - subscriber.lastSendMs >= kSpectatorStartRetransmitMs)
            {
                spectator::protocol::GameStart start{};
                if (spectator::BuildGameStartMessage(
                        subscriber.gameId,
                        subscriber.sessionTag,
                        subscriber.nextSequence++,
                        &start))
                {
                    start.clientNonce = subscriber.clientNonce;
                    subscriber.initFingerprint =
                        spectator::protocol::GameStartFingerprint(start);
                    if (SendToSpectatorEndpoint(
                            subscriber.endpoint,
                            &start,
                            sizeof(start)))
                    {
                        subscriber.lastSendMs = now;
                        ++subscriber.packetsSent;
                        subscriber.bytesSent += sizeof(start);
                        --subscriberBudget;
                        --globalBudget;
                    }
                }
            }
            continue;
        }

        spectator::GameExtent extent{};
        if (!spectator::GetGameExtent(subscriber.gameId, &extent)
            || subscriber.ackedRecordIndex > extent.frameCount)
        {
            // Keep the player session independent of a corrupt spectator cursor.
            SendSpectatorReject(
                subscriber.endpoint,
                spectator::protocol::RejectReason::GameNotRetained,
                subscriber.gameId,
                subscriber.nextSequence++);
            const std::size_t badIndex = (startIndex + visited) % subscriberCount;
            g_spectators.erase(
                g_spectators.begin() + static_cast<std::ptrdiff_t>(badIndex));
            g_spectatorRoundRobin = g_spectators.empty()
                ? 0u : badIndex % g_spectators.size();
            return;
        }

        const std::uint32_t windowFrames = static_cast<std::uint32_t>(
            kSpectatorWindowBatches * spectator::protocol::kBatchFrames);
        const std::uint32_t inFlight =
            subscriber.maxSentRecordIndex - subscriber.ackedRecordIndex;
        if (subscriber.ackedRecordIndex < subscriber.maxSentRecordIndex
            && now - subscriber.lastAckProgressMs >= kSpectatorBatchRetransmitMs
            && now - subscriber.lastRetransmitMs >= kSpectatorBatchRetransmitMs
            && subscriberBudget != 0u && globalBudget != 0u)
        {
            spectator::protocol::ConfirmedInputBatch batch{};
            if (spectator::BuildConfirmedInputBatch(
                    subscriber.gameId,
                    subscriber.ackedRecordIndex,
                    subscriber.sessionTag,
                    subscriber.nextSequence++,
                    &batch))
            {
                const std::size_t bytes =
                    spectator::protocol::ConfirmedInputBatchWireSize(batch.count);
                if (SendToSpectatorEndpoint(
                        subscriber.endpoint,
                        &batch,
                        bytes))
                {
                    subscriber.lastSendMs = now;
                    subscriber.lastRetransmitMs = now;
                    ++subscriber.packetsSent;
                    subscriber.bytesSent += bytes;
                    ++subscriber.retransmits;
                    --subscriberBudget;
                    --globalBudget;
                }
            }
        }

        while (subscriber.maxSentRecordIndex < extent.frameCount
            && (inFlight == 0u
                || subscriber.maxSentRecordIndex - subscriber.ackedRecordIndex
                    < windowFrames)
            && subscriberBudget != 0u && globalBudget != 0u)
        {
            spectator::protocol::ConfirmedInputBatch batch{};
            if (!spectator::BuildConfirmedInputBatch(
                    subscriber.gameId,
                    subscriber.maxSentRecordIndex,
                    subscriber.sessionTag,
                    subscriber.nextSequence++,
                    &batch))
            {
                break;
            }
            const std::size_t bytes =
                spectator::protocol::ConfirmedInputBatchWireSize(batch.count);
            if (!SendToSpectatorEndpoint(subscriber.endpoint, &batch, bytes))
            {
                break;
            }
            subscriber.maxSentRecordIndex =
                batch.startRecordIndex + static_cast<std::uint32_t>(batch.count);
            subscriber.lastSendMs = now;
            ++subscriber.packetsSent;
            subscriber.bytesSent += bytes;
            --subscriberBudget;
            --globalBudget;
        }

        if (subscriber.ackedRecordIndex < extent.frameCount)
        {
            continue;
        }

        if (extent.hasNextGame)
        {
            spectator::protocol::GameStart next{};
            if (spectator::BuildGameStartMessage(
                    subscriber.gameId + 1u,
                    subscriber.sessionTag,
                    subscriber.nextSequence,
                    &next))
            {
                subscriber.gameId = next.gameId;
                subscriber.ackedRecordIndex = 0u;
                subscriber.maxSentRecordIndex = 0u;
                subscriber.gameStartAcked = false;
                subscriber.initAcked = false;
                subscriber.initFingerprint = 0u;
                subscriber.lastSendMs = 0u;
                subscriber.lastAckProgressMs = now;
                subscriber.lastRetransmitMs = 0u;
                continue;
            }
        }

        if (subscriberBudget != 0u && globalBudget != 0u
            && (subscriber.lastSendMs == 0u
                || now - subscriber.lastSendMs >= kSpectatorKeepAliveMs))
        {
            spectator::protocol::KeepAlive keepAlive{};
            keepAlive.header.sessionTag = subscriber.sessionTag;
            keepAlive.header.sequence = subscriber.nextSequence++;
            keepAlive.gameId = subscriber.gameId;
            keepAlive.nextRecordIndex = subscriber.ackedRecordIndex;
            if (SendToSpectatorEndpoint(subscriber.endpoint, &keepAlive, sizeof(keepAlive)))
            {
                subscriber.lastSendMs = now;
                ++subscriber.packetsSent;
                subscriber.bytesSent += sizeof(keepAlive);
                --globalBudget;
            }
        }
    }
    g_spectatorRoundRobin = (startIndex + visited) % subscriberCount;
}

void FlushDelayedSends()
{
    std::lock_guard<std::recursive_mutex> socketLock(g_socketMutex);
    if (g_socket == INVALID_SOCKET)
    {
        return;
    }

    std::vector<DelayedSend> due;
    {
        std::lock_guard<std::mutex> lock(g_delayedSendMutex);
        if (g_delayedSends.empty())
        {
            return;
        }
        const DWORD now = GetTickCount();
        for (std::size_t i = 0; i < g_delayedSends.size();)
        {
            if (static_cast<std::int32_t>(now - g_delayedSends[i].releaseMs) >= 0)
            {
                due.push_back(std::move(g_delayedSends[i]));
                g_delayedSends[i] = std::move(g_delayedSends.back());
                g_delayedSends.pop_back();
            }
            else
            {
                ++i;
            }
        }
    }

    for (const DelayedSend& pending : due)
    {
        for (int repeat = 0; repeat < pending.overlapSends; ++repeat)
        {
            (void)sendto(
                g_socket,
                reinterpret_cast<const char*>(pending.bytes.data()),
                static_cast<int>(pending.bytes.size()),
                0,
                reinterpret_cast<const sockaddr*>(&pending.peer),
                sizeof(pending.peer));
        }
    }
}

void CloseSocket()
{
    std::lock_guard<std::recursive_mutex> socketLock(g_socketMutex);
    g_autopunch.Detach();
    if (g_socket != INVALID_SOCKET)
    {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }
    g_localBoundPort = 0u;
    g_lastSpectatorRegisterMs = 0u;
    std::lock_guard<std::mutex> lock(g_delayedSendMutex);
    g_delayedSends.clear();
}

bool BindLocalPort()
{
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(g_isHost ? g_port : 0u);
    local.sin_addr.S_un.S_addr = htonl(INADDR_ANY);

    constexpr unsigned kHostTakeoverAttempts = 41u;
    constexpr DWORD kHostTakeoverRetryMs = 25u;
    const bool managed = GetEnvironmentVariableW(
        L"TRIAL_CASTER_SESSION_INI", nullptr, 0u) > 1u;
    unsigned bindAttempt = 0u;
    int bindError = 0;
    for (;;)
    {
        if (bind(g_socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != SOCKET_ERROR)
        {
            if (bindAttempt > 0u)
            {
                log::Info(
                    "netplay",
                    "host UDP takeover bind succeeded port=%u attempts=%u wait_ms=%u",
                    static_cast<unsigned>(g_port),
                    bindAttempt + 1u,
                    bindAttempt * static_cast<unsigned>(kHostTakeoverRetryMs));
            }
            break;
        }

        bindError = WSAGetLastError();
        if (!g_isHost || !managed || bindError != WSAEADDRINUSE
            || bindAttempt + 1u >= kHostTakeoverAttempts)
        {
            log::Error(
                "netplay",
                "UDP bind failed role=%s port=%u error=%d attempts=%u",
                g_isHost ? "host" : "client",
                static_cast<unsigned>(g_isHost ? g_port : 0u),
                bindError,
                bindAttempt + 1u);
            return false;
        }
        if (bindAttempt == 0u)
        {
            log::Info(
                "netplay",
                "host UDP port=%u still owned by launcher; bounded takeover retry started",
                static_cast<unsigned>(g_port));
        }
        ++bindAttempt;
        Sleep(kHostTakeoverRetryMs);
    }

    if (!g_isHost)
    {
        sockaddr_in bound{};
        int boundLen = sizeof(bound);
        if (getsockname(g_socket, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0)
        {
            log::Info("netplay", "client bound ephemeral local port=%u (dials host port=%u)",
                static_cast<unsigned>(ntohs(bound.sin_port)), static_cast<unsigned>(g_peerPort));
        }
    }
    return true;
}

bool OpenSocket()
{
    CloseSocket();
    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET)
    {
        lifecycle::Fault("netplay", "socket() failed port=%u", static_cast<unsigned>(g_port));
        return false;
    }

    u_long nonBlocking = 1;
    if (ioctlsocket(g_socket, FIONBIO, &nonBlocking) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        lifecycle::Fault("netplay", "FIONBIO failed error=%d", error);
        log::Error("netplay", "FIONBIO failed error=%d", error);
        CloseSocket();
        return false;
    }

    if (!BindLocalPort())
    {
        lifecycle::Fault("netplay", "bind() failed port=%u", static_cast<unsigned>(g_port));
        CloseSocket();
        return false;
    }

    if (!g_peerNarrow.empty())
    {
        g_peer = {};
        g_peer.sin_family = AF_INET;
        g_peer.sin_port = htons(g_peerPort);
        if (InetPtonA(AF_INET, g_peerNarrow.c_str(), &g_peer.sin_addr) != 1)
        {
            lifecycle::Fault("netplay", "invalid peer IPv4 address '%s'", g_peerNarrow.c_str());
            log::Error("netplay", "invalid peer IPv4 address '%s'", g_peerNarrow.c_str());
            CloseSocket();
            return false;
        }
    }
    else
    {
        lifecycle::Fault("netplay", "peer IPv4 address is empty");
        CloseSocket();
        return false;
    }

    sockaddr_in bound{};
    int boundLength = sizeof(bound);
    if (getsockname(g_socket, reinterpret_cast<sockaddr*>(&bound), &boundLength) != 0
        || bound.sin_port == 0u)
    {
        lifecycle::Fault("netplay", "getsockname() failed after bind");
        CloseSocket();
        return false;
    }
    g_localBoundPort = ntohs(bound.sin_port);
    if (!g_autopunch.Attach(
            g_socket,
            ntohs(bound.sin_port),
            &g_peer,
            !g_isHost,
            &AutopunchLog,
            nullptr))
    {
        log::Warn("netplay", "autopunch helper attach failed; direct UDP remains available");
    }

    return true;
}

void ServiceSpectatorCoordinatorRegistration(const DWORD now)
{
    if (g_socket == INVALID_SOCKET || g_localBoundPort == 0u
        || (g_lastSpectatorRegisterMs != 0u
            && now - g_lastSpectatorRegisterMs
                < trialnet::autopunch::kRegisterIntervalMs))
    {
        return;
    }
    // Registration only: client AutoPunch would punch the player host forever.
    g_lastSpectatorRegisterMs = now;
    sockaddr_in coordinator{};
    coordinator.sin_family = AF_INET;
    coordinator.sin_port = htons(trialnet::autopunch::kCoordinatorPort);
    if (InetPtonA(
            AF_INET,
            trialnet::autopunch::kCoordinatorIpv4,
            &coordinator.sin_addr) != 1)
    {
        return;
    }
    const trialnet::autopunch::RegisterPacket registration =
        trialnet::autopunch::MakeRegister(g_localBoundPort);
    (void)SendToSpectatorEndpoint(
        coordinator,
        registration.data(),
        registration.size());
}

void ServiceAutopunch()
{
    const DWORD now = GetTickCount();
    if (IsDeterminismSynced())
    {
        g_autopunch.Stop("in-game peer handshake complete; spectator registration only");
        ServiceSpectatorCoordinatorRegistration(now);
        return;
    }

    const DWORD transportStarted = g_transportStartMs.load(std::memory_order_acquire);
    if (!g_autopunch.IsActive()
        && transportStarted != 0u
        && now - transportStarted >= trialnet::autopunch::kDirectAttemptMs)
    {
        g_autopunch.Start(now);
    }
    g_autopunch.Service(now);
}

std::uint32_t GenerateHostSeed()
{
    const std::uint32_t tick = static_cast<std::uint32_t>(GetTickCount());
    const std::uint32_t shadow = det::rng::Snapshot();
    const std::uint32_t seed =
        tick ^ shadow ^ kSeedSalt ^ (static_cast<std::uint32_t>(g_port) << 16);
    return seed != 0u ? seed : kSeedSalt;
}

bool GenerateHandshakeNonce(std::uint32_t* const outNonce)
{
    if (outNonce == nullptr)
    {
        return false;
    }
    *outNonce = 0u;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        std::uint32_t candidate = 0u;
        if (!win32::SecureRandomBytes(&candidate, sizeof(candidate)))
        {
            return false;
        }
        if (candidate != 0u)
        {
            *outNonce = candidate;
            return true;
        }
    }
    return false;
}

void ApplyLocalDeterminism(const std::uint32_t seed)
{
    det::RepinFpu();
    det::rng::SetSeed(seed);
    rollback::SetRngSeed(seed);
    g_syncedSeed.store(seed, std::memory_order_release);
}

void MarkDeterminismSynced(const std::uint32_t seed)
{
    g_syncedSeed.store(seed, std::memory_order_release);
    g_seedSynced.store(true, std::memory_order_release);

    lifecycle::Netplay(
        "determinism seed applied seed=0x%08X shadow=0x%08X canary=%d fpuCw=0x%04X",
        seed,
        det::rng::Snapshot(),
        det::rng::PredictCanary(seed),
        static_cast<unsigned>(det::GetCapturedFpuCw()));
    log::Info(
        "netplay",
        "determinism seed applied seed=0x%08X shadow=0x%08X canary=%d fpuCw=0x%04X",
        seed,
        det::rng::Snapshot(),
        det::rng::PredictCanary(seed),
        static_cast<unsigned>(det::GetCapturedFpuCw()));
}

void MarkDeterminismHandshakeConfirmed(const char* const proof)
{
    if (g_seedHandshakeConfirmed.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    lifecycle::Netplay(
        "determinism handshake confirmed role=%s clientNonce=%08X hostNonce=%08X seed=%08X proof=%s",
        g_isHost ? "host" : "client",
        g_isHost ? g_activeClientNonce : g_localClientNonce,
        g_isHost ? g_hostSeedNonce : g_acceptedHostNonce,
        g_isHost ? g_hostSeed : g_acceptedHostSeed,
        proof != nullptr ? proof : "unknown");
    log::Info(
        "netplay",
        "determinism handshake confirmed role=%s clientNonce=%08X hostNonce=%08X seed=%08X proof=%s",
        g_isHost ? "host" : "client",
        g_isHost ? g_activeClientNonce : g_localClientNonce,
        g_isHost ? g_hostSeedNonce : g_acceptedHostNonce,
        g_isHost ? g_hostSeed : g_acceptedHostSeed,
        proof != nullptr ? proof : "unknown");
}

bool BeginSeedExchange()
{
    g_seedSynced.store(false, std::memory_order_release);
    g_seedHandshakeConfirmed.store(false, std::memory_order_release);
    g_syncedSeed.store(0, std::memory_order_release);
    g_runtimeSessionId.store(0u, std::memory_order_release);
    g_clientAckedSeed = false;
    g_seedExchangeActive = true;
    g_hostSeed = 0u;
    g_hostSeedNonce = 0u;
    g_localClientNonce = 0u;
    g_activeClientNonce = 0u;
    g_acceptedHostNonce = 0u;
    g_acceptedHostSeed = 0u;
    g_lastClientHelloMs = 0u;
    g_lastSeedHandshakeSendMs = 0u;

    nav::SetPacketStage(nav::PacketStage::SeedExchange);
    det::RepinFpu();

    if (g_isHost)
    {
        if (!GenerateHandshakeNonce(&g_hostSeedNonce))
        {
            g_seedExchangeActive = false;
            lifecycle::Fault("netplay", "secure host seed nonce generation failed");
            log::Error("netplay", "secure host seed nonce generation failed");
            return false;
        }
        g_hostSeed = GenerateHostSeed();
        ApplyLocalDeterminism(g_hostSeed);
        lifecycle::Netplay(
            "host seed exchange started seed=0x%08X hostNonce=%08X",
            g_hostSeed,
            g_hostSeedNonce);
        log::Info(
            "netplay",
            "host seed exchange started seed=0x%08X hostNonce=%08X",
            g_hostSeed,
            g_hostSeedNonce);
    }
    else
    {
        if (!GenerateHandshakeNonce(&g_localClientNonce))
        {
            g_seedExchangeActive = false;
            lifecycle::Fault("netplay", "secure client seed nonce generation failed");
            log::Error("netplay", "secure client seed nonce generation failed");
            return false;
        }
        lifecycle::Netplay(
            "client waiting for host RNG seed clientNonce=%08X",
            g_localClientNonce);
        log::Info(
            "netplay",
            "client waiting for host RNG seed clientNonce=%08X",
            g_localClientNonce);
    }
    return true;
}

void HandleSeedPacket(const proto::ExchangeRandomSeedPacket& packet, const sockaddr_in& from)
{
    if (g_isHost)
    {
        if (proto::IsClientRendezvousHello(packet))
        {
            return;
        }

        if (g_clientAckedSeed
            && proto::IsClientSeedConfirmAck(
                packet,
                g_activeClientNonce,
                g_hostSeedNonce,
                g_hostSeed))
        {
            nav::NoteSeedRecv();
            MarkDeterminismHandshakeConfirmed("client-confirm-ack");
            g_seedExchangeActive = false;
            return;
        }

        if (proto::IsClientSeedAck(
                packet,
                g_activeClientNonce,
                g_hostSeedNonce,
                g_hostSeed))
        {
            nav::NoteSeedRecv();
            g_peer.sin_addr = from.sin_addr;
            g_peer.sin_port = from.sin_port;
            if (!g_clientAckedSeed)
            {
                const std::uint64_t sessionId = proto::MakeRuntimeSessionId(
                    g_activeClientNonce,
                    g_hostSeedNonce,
                    g_hostSeed);
                if (sessionId == 0u)
                {
                    lifecycle::Fault("netplay", "refused incomplete host runtime session identity");
                    return;
                }
                g_runtimeSessionId.store(sessionId, std::memory_order_release);
                g_clientAckedSeed = true;
                MarkDeterminismSynced(g_hostSeed);
            }
            // Until the final ACK returns, neither negotiation nor LaunchGo may leave this phase.
            g_lastSeedHandshakeSendMs = 0u;
            return;
        }

        lifecycle::Netplay(
            "ignore seed proof mismatch kind=%u client=%08X/%08X host=%08X/%08X seed=%08X/%08X",
            static_cast<unsigned>(packet.kind),
            packet.clientNonce,
            g_activeClientNonce,
            packet.hostNonce,
            g_hostSeedNonce,
            packet.seed,
            g_hostSeed);
        return;
    }

    if (g_seedSynced.load(std::memory_order_acquire)
        && proto::IsHostSeedConfirm(
            packet,
            g_localClientNonce,
            g_acceptedHostNonce,
            g_acceptedHostSeed))
    {
        nav::NoteSeedRecv();
        MarkDeterminismHandshakeConfirmed("host-confirm");
        proto::ExchangeRandomSeedPacket ack = packet;
        ack.kind = proto::SeedHandshakeKind::ClientConfirmAck;
        if (SendToPeer(&ack, sizeof(ack), "seed-confirm-ack"))
        {
            nav::NoteSeedSent();
        }
        g_seedExchangeActive = false;
        return;
    }

    if (!proto::IsHostSeedOffer(packet, g_localClientNonce))
    {
        log::Debug(
            "netplay",
            "ignore seed offer kind=%u client=%08X expected=%08X host=%08X seed=%08X",
            static_cast<unsigned>(packet.kind),
            packet.clientNonce,
            g_localClientNonce,
            packet.hostNonce,
            packet.seed);
        return;
    }

    nav::NoteSeedRecv();
    if (g_seedSynced.load(std::memory_order_acquire))
    {
        if (packet.hostNonce != g_acceptedHostNonce || packet.seed != g_acceptedHostSeed)
        {
            lifecycle::Netplay(
                "ignore changed seed offer after commit host=%08X/%08X seed=%08X/%08X",
                packet.hostNonce,
                g_acceptedHostNonce,
                packet.seed,
                g_acceptedHostSeed);
            return;
        }
        proto::ExchangeRandomSeedPacket ack = packet;
        ack.kind = proto::SeedHandshakeKind::ClientAck;
        if (SendToPeer(&ack, sizeof(ack), "seed-ack"))
        {
            nav::NoteSeedSent();
        }
        return;
    }

    g_acceptedHostNonce = packet.hostNonce;
    g_acceptedHostSeed = packet.seed;
    const std::uint64_t sessionId = proto::MakeRuntimeSessionId(
        g_localClientNonce,
        g_acceptedHostNonce,
        g_acceptedHostSeed);
    if (sessionId == 0u)
    {
        lifecycle::Fault("netplay", "refused incomplete client runtime session identity");
        return;
    }
    g_runtimeSessionId.store(sessionId, std::memory_order_release);
    ApplyLocalDeterminism(packet.seed);
    MarkDeterminismSynced(packet.seed);

    proto::ExchangeRandomSeedPacket ack = packet;
    ack.kind = proto::SeedHandshakeKind::ClientAck;
    if (SendToPeer(&ack, sizeof(ack), "seed-ack"))
    {
        nav::NoteSeedSent();
    }
}

void AbortNetplay(const char* reason)
{
    // A graceful close already committed a handoff; liveness must not replace it.
    if (g_peerGracefulEndHandling.load(std::memory_order_acquire))
    {
        return;
    }
    // Claim ownership before publishing the abort: bootstrap may ExitProcess on it.
    if (g_abortOwnerClaimed.exchange(true, std::memory_order_acq_rel)
        || g_netplayAborted.load(std::memory_order_acquire))
    {
        return;
    }
    // Publish the takeover before waiting; Poll rechecks this depth between phases.
    g_terminalHandoffDepth.fetch_add(1u, std::memory_order_acq_rel);
    std::lock_guard<std::recursive_mutex> transportLock(g_transportOwnerMutex);
    g_netplayAborted.store(true, std::memory_order_release);
    if (reason != nullptr && reason[0] != '\0')
    {
        std::strncpy(g_abortReason, reason, sizeof(g_abortReason) - 1u);
        g_abortReason[sizeof(g_abortReason) - 1u] = '\0';
        lifecycle::Exit("netplay", reason, nullptr);
    }
    proto::QuitPacket quit{};
    quit.reason = proto::QuitReason::ProtocolAbort;
    // Zero before the nonce proof, so the peer drops it and waits for its own deadline.
    quit.sessionId = g_runtimeSessionId.load(std::memory_order_acquire);
    // Tell the peer before the local drain, or it sits in gameplay until its timeout.
    SendToPeer(&quit, sizeof(quit), "quit");
    // Keep the relay alive briefly: an immediate close can lose the lifecycle proof.
    NotifySpectatorsSourceEnded(true);
    CloseSync();
    g_terminalHandoffDepth.fetch_sub(1u, std::memory_order_acq_rel);
}

bool IsValidRollbackPacket(const proto::RollbackReadyPacket& packet)
{
    return proto::IsValidRollbackReadyContract(
        packet,
        g_requestedTargetFps,
        g_isHost,
        g_runtimeSessionId.load(std::memory_order_acquire));
}

void HandleRollbackReadyPacket(const proto::RollbackReadyPacket& packet)
{
    if (!g_requireRollback)
    {
        return;
    }

    if (!IsDeterminismSynced())
    {
        log::Debug("netplay", "ignored rollback-ready before confirmed seed handshake");
        return;
    }

    if (!IsValidRollbackPacket(packet))
    {
        AbortNetplay("peer sent invalid rollback-ready packet");
        return;
    }

    if (g_isHost)
    {
        if ((packet.flags & proto::kRollbackFlagAck) == 0u
            || packet.maxRollback != g_requestedRollbackFrames
            || packet.reserved[0] != g_requestedTargetFps)
        {
            AbortNetplay("peer rollback/frame-rate configuration acknowledgement mismatch");
            return;
        }
        if (!spectator::CommitNegotiatedRollbackFrames(packet.maxRollback))
        {
            AbortNetplay("spectator recorder rejected negotiated rollback contract");
            return;
        }
        if (g_rollbackNegotiated.load(std::memory_order_acquire))
        {
            return;
        }
        g_peerRollbackAck = true;
        g_negotiatedRollbackFrames.store(packet.maxRollback, std::memory_order_release);
        g_negotiatedTargetFps.store(packet.reserved[0], std::memory_order_release);
        g_rollbackNegotiated.store(true, std::memory_order_release);
        lifecycle::Netplay(
            "rollback negotiated host ack rollback=%u target_fps=%u",
            static_cast<unsigned>(packet.maxRollback),
            static_cast<unsigned>(packet.reserved[0]));
        log::Info(
            "netplay",
            "rollback negotiated (host got client ack) rollback=%u target_fps=%u",
            static_cast<unsigned>(packet.maxRollback),
            static_cast<unsigned>(packet.reserved[0]));
        return;
    }

    if (packet.reserved[0] != g_requestedTargetFps)
    {
        AbortNetplay("host frame-rate contract differs from launcher session");
        return;
    }

    const bool firstAck = !g_rollbackNegotiated.load(std::memory_order_acquire)
        || g_negotiatedRollbackFrames.load(std::memory_order_acquire) != packet.maxRollback
        || g_negotiatedTargetFps.load(std::memory_order_acquire) != packet.reserved[0];
    if (!spectator::CommitNegotiatedRollbackFrames(packet.maxRollback))
    {
        AbortNetplay("spectator recorder rejected host rollback contract");
        return;
    }
    g_negotiatedRollbackFrames.store(packet.maxRollback, std::memory_order_release);
    g_negotiatedTargetFps.store(packet.reserved[0], std::memory_order_release);
    proto::RollbackReadyPacket ack = packet;
    ack.flags |= proto::kRollbackFlagAck;
    if (SendToPeer(&ack, sizeof(ack), "rollback-ack"))
    {
        g_rollbackNegotiated.store(true, std::memory_order_release);
        // ACK every duplicate offer: a whole logical send can be lost.
        if (firstAck)
        {
            lifecycle::Netplay(
                "rollback negotiated client ack sent rollback=%u target_fps=%u",
                static_cast<unsigned>(packet.maxRollback),
                static_cast<unsigned>(packet.reserved[0]));
            log::Info(
                "netplay",
                "rollback negotiated (client ack sent) rollback=%u target_fps=%u",
                static_cast<unsigned>(packet.maxRollback),
                static_cast<unsigned>(packet.reserved[0]));
        }
    }
}

void PollRollbackNegotiation()
{
    if (!g_requireRollback || g_rollbackNegotiated.load(std::memory_order_acquire)
        || g_netplayAborted.load(std::memory_order_acquire))
    {
        return;
    }

    if (!IsDeterminismSynced())
    {
        return;
    }

    if (!rollback::kCompiledIn || (!rollback::IsEnabled() && !rollback::IsArmedForNetplay()))
    {
        AbortNetplay("local rollback module not active");
        return;
    }

    if (g_isHost)
    {
        proto::RollbackReadyPacket packet{};
        packet.maxRollback = g_requestedRollbackFrames;
        packet.reserved[0] = g_requestedTargetFps;
        packet.sessionId = g_runtimeSessionId.load(std::memory_order_acquire);
        SendToPeer(&packet, sizeof(packet), "rollback-ready");
        if (g_peerRollbackAck)
        {
            g_rollbackNegotiated.store(true, std::memory_order_release);
        }
        return;
    }

}

void HandleLaunchGoPacket(const proto::LaunchGoPacket& packet)
{
    if (packet.controlWord != proto::kActionLaunchGo)
    {
        return;
    }

    if (packet.launchAtMs != 0u)
    {
        // Late 9-byte launcher LaunchGo copies must neither commit nor abort the session.
        log::Debug(
            "netplay",
            "ignored launcher-domain launch-go leadMs=%u",
            static_cast<unsigned>(packet.launchAtMs));
        return;
    }

    if (!IsDeterminismSynced())
    {
        log::Debug("netplay", "ignored launch-go before confirmed seed handshake");
        return;
    }

    if (g_requireRollback && !g_rollbackNegotiated.load(std::memory_order_acquire))
    {
        log::Debug("netplay", "ignored launch-go before current rollback contract");
        return;
    }

    nav::NoteLaunchGoRecv();

    if (!proto::IsValidLaunchGoContract(
            packet,
            g_requestedTargetFps,
            g_runtimeSessionId.load(std::memory_order_acquire))
        || pacing::GetSessionTargetFps() != packet.targetFps)
    {
        AbortNetplay("host launch-go frame-rate contract mismatch");
        return;
    }

    // The host retransmits LaunchGo, so only the first copy may apply the delay.
    const std::uint8_t inputDelay = std::min(packet.inputDelay, proto::kMaxInputDelay);
    const bool first = !g_launchGo.exchange(true, std::memory_order_acq_rel);
    if (!first
        && g_negotiatedInputDelay.load(std::memory_order_acquire)
               == static_cast<int>(inputDelay)
        && g_negotiatedTargetFps.load(std::memory_order_acquire)
               == static_cast<int>(packet.targetFps))
    {
        return;
    }

    nav::SetPacketStage(nav::PacketStage::LaunchGo);
    rollback::SetInputDelay(inputDelay);
    g_negotiatedInputDelay.store(static_cast<int>(inputDelay), std::memory_order_release);
    g_negotiatedTargetFps.store(static_cast<int>(packet.targetFps), std::memory_order_release);
    lifecycle::Netplay(
        "launch-go received delay=%u leadMs=%u target_fps=%u",
        static_cast<unsigned>(inputDelay),
        static_cast<unsigned>(packet.launchAtMs),
        static_cast<unsigned>(packet.targetFps));
    log::Info(
        "netplay",
        "launch-go received (delay=%u leadMs=%u target_fps=%u)",
        static_cast<unsigned>(inputDelay),
        static_cast<unsigned>(packet.launchAtMs),
        static_cast<unsigned>(packet.targetFps));
}

void ApplyRemoteSyncPacket(const proto::SyncDataPacket& packet)
{
    nav::NoteSyncRecv();
    const std::uint32_t recvFrame = packet.currentFrame;
    g_syncRecvTotal.fetch_add(1, std::memory_order_relaxed);
    const std::size_t tailIndex =
        recvFrame > 0u
            ? (std::min(static_cast<std::size_t>(recvFrame), static_cast<std::size_t>(proto::kMaxQueueSlot))
               - 1u)
            : 0u;
    // Per-packet logging here costs real frame rate.
    if (packet.actionData[tailIndex].playerAction != 0u)
    {
        log::Debug(
            "netplay",
            "recv peer input frame=%u action=0x%02X sys=0x%02X",
            recvFrame,
            static_cast<unsigned>(packet.actionData[tailIndex].playerAction),
            static_cast<unsigned>(packet.actionData[tailIndex].systemState));
    }

    const bool applied = rollback::IsEnabled() ? rollback::ApplySyncDataPacket(packet)
                                             : lockstep::ApplyIncomingPacket(packet);
    if (!applied)
    {
        lifecycle::Sync(
            "netplay",
            "reject SyncData frame=%u rollback=%d local=%u remote=%u",
            packet.currentFrame,
            rollback::IsEnabled() ? 1 : 0,
            netplay::lockstep::GetLocalHistorySize(),
            netplay::lockstep::GetRemoteHistorySize());
        AbortNetplay("unrecoverable SyncData history gap or invalid frontier");
        return;
    }

    if (applied && !rollback::IsEnabled())
    {
        caster::RefreshLockstepWireInject();
        if (caster::HasRemoteInput())
        {
            nav::NoteSyncApplied();
        }
    }
    else if (applied)
    {
        nav::NoteSyncApplied();
    }

    if (packet.confirmedHashFrame != UINT32_MAX)
    {
        rollback::desync::RecordRemoteChain(
            packet.confirmedHashFrame,
            packet.confirmedHashChain);
    }

    g_syncFrame.store(packet.currentFrame, std::memory_order_release);
}

void HandleSyncPacket(const proto::SyncDataPacket& packet)
{
    if (packet.controlWord != proto::kActionSyncData)
    {
        return;
    }

    const std::uint16_t localTimeline = g_syncTimeline.load(std::memory_order_acquire);
    if (packet.timeline != localTimeline)
    {
        // Other generations must not touch this generation's immutable input history.
        log::Debug(
            "netplay",
            "ignore SyncData timeline=%u localTimeline=%u frame=%u",
            static_cast<unsigned>(packet.timeline),
            static_cast<unsigned>(localTimeline),
            packet.currentFrame);
        return;
    }

    if (g_entryCommitPending.load(std::memory_order_acquire)
        && packet.timeline == g_entryCommitTimeline.load(std::memory_order_acquire)
        && g_entryCommitPending.exchange(false, std::memory_order_acq_rel))
    {
        const std::uint32_t frame = g_entryCommitFrame.load(std::memory_order_acquire);
        const std::uint32_t epoch = g_entryCommitEpoch.load(std::memory_order_acquire);
        lifecycle::Netplay(
            "battle entry peer commit proven frame=%u epoch=%u timeline=%u",
            frame,
            epoch,
            static_cast<unsigned>(packet.timeline));
        log::Step(
            "netplay",
            "battle entry peer commit proven frame=%u epoch=%u timeline=%u",
            frame,
            epoch,
            static_cast<unsigned>(packet.timeline));
    }

    const DWORD now = GetTickCount();
    g_lastSyncArrivalMs.store(now, std::memory_order_release);
    ApplyRemoteSyncPacket(packet);
    if (g_netplayAborted.load(std::memory_order_acquire))
    {
        return;
    }

    std::uint32_t remoteFrontier = 0u;
    std::uint32_t ackFrontier = 0u;
    if (rollback::IsEnabled())
    {
        const rollback::Status status = rollback::GetStatus();
        remoteFrontier = static_cast<std::uint32_t>(std::min<std::size_t>(
            status.remoteInputCount,
            std::numeric_limits<std::uint32_t>::max()));
        ackFrontier = status.peerAckFrame;
    }
    else
    {
        remoteFrontier = lockstep::GetRemoteHistorySize();
        ackFrontier = lockstep::GetPeerAckFrame();
    }

    const std::uint16_t progressTimeline =
        g_syncProgressTimeline.load(std::memory_order_acquire);
    const std::uint32_t previousRemote =
        g_syncProgressRemoteFrontier.load(std::memory_order_acquire);
    const std::uint32_t previousAck =
        g_syncProgressAckFrontier.load(std::memory_order_acquire);
    const bool firstProgressPacket =
        g_lastSyncRecvMs.load(std::memory_order_acquire) == 0u;
    const bool generationChanged = progressTimeline != packet.timeline;
    const bool frontierAdvanced = remoteFrontier > previousRemote
        || ackFrontier > previousAck;
    if (firstProgressPacket || generationChanged || frontierAdvanced)
    {
        g_syncProgressTimeline.store(packet.timeline, std::memory_order_release);
        g_syncProgressRemoteFrontier.store(remoteFrontier, std::memory_order_release);
        g_syncProgressAckFrontier.store(ackFrontier, std::memory_order_release);
        g_lastSyncRecvMs.store(now, std::memory_order_release);
    }
}

void CheckPeerLiveness()
{
    if (g_netplayAborted.load(std::memory_order_acquire))
    {
        return;
    }
    DWORD lastProgress = g_lastSyncRecvMs.load(std::memory_order_acquire);
    const DWORD lastArrival = g_lastSyncArrivalMs.load(std::memory_order_acquire);
    const DWORD now = GetTickCount();
    if (lastArrival == 0u)
    {
        const DWORD started = g_transportStartMs.load(std::memory_order_acquire);
        if (started == 0u)
        {
            return;
        }
        const std::uint64_t scaledHandshake =
            15000ull + static_cast<std::uint64_t>(lockstep::GetPingMs()) * 8ull;
        const DWORD handshakeTimeoutMs = static_cast<DWORD>(
            std::clamp<std::uint64_t>(scaledHandshake, 30000ull, 120000ull));
        // Absolute deadline: seed or unknown packets must not slide the timeout forward.
        if (now - started > handshakeTimeoutMs)
        {
            AbortNetplay("peer timeout during netplay handshake");
        }
        return;
    }
    std::uint32_t localFrontier = 0u;
    std::uint32_t remoteFrontier = 0u;
    std::uint32_t ackFrontier = 0u;
    if (rollback::IsEnabled())
    {
        const rollback::Status status = rollback::GetStatus();
        localFrontier = static_cast<std::uint32_t>(std::min<std::size_t>(
            status.localInputCount,
            std::numeric_limits<std::uint32_t>::max()));
        remoteFrontier = static_cast<std::uint32_t>(std::min<std::size_t>(
            status.remoteInputCount,
            std::numeric_limits<std::uint32_t>::max()));
        ackFrontier = status.peerAckFrame;
    }
    else
    {
        localFrontier = lockstep::GetLocalHistorySize();
        remoteFrontier = lockstep::GetRemoteHistorySize();
        ackFrontier = lockstep::GetPeerAckFrame();
    }
    const bool progressNeeded = remoteFrontier < localFrontier
        || ackFrontier < localFrontier;
    const bool progressWasNeeded =
        g_syncProgressWasNeeded.exchange(progressNeeded, std::memory_order_acq_rel);
    if (progressNeeded && !progressWasNeeded)
    {
        lastProgress = now;
        g_lastSyncRecvMs.store(now, std::memory_order_release);
    }
    // Duplicates move this deadline only while both required frontiers are caught up.
    const DWORD last = progressNeeded ? lastProgress : lastArrival;
    const std::uint64_t scaled =
        4000ull + static_cast<std::uint64_t>(lockstep::GetPingMs()) * 8ull;
    const DWORD timeoutMs = static_cast<DWORD>(std::clamp<std::uint64_t>(scaled, 8000ull, 60000ull));
    if (now - last > timeoutMs)
    {
        char reason[96] = {};
        std::snprintf(
            reason,
            sizeof(reason),
            progressNeeded
                ? "peer timeout — no SyncData input/ACK progress for %lums"
                : "peer timeout — no authenticated SyncData for %lums",
            static_cast<unsigned long>(timeoutMs));
        AbortNetplay(reason);
    }
}

bool IsCurrentRuntimeDatagram(const std::uint8_t* const data, const std::size_t size)
{
    if (data == nullptr || size == 0u)
    {
        return false;
    }
    std::uint64_t sessionId = 0u;
    switch (data[0])
    {
    case proto::kActionSyncData:
        if (size == sizeof(proto::SyncDataPacket))
        {
            proto::SyncDataPacket packet{};
            std::memcpy(&packet, data, sizeof(packet));
            sessionId = packet.sessionId;
        }
        break;
    case proto::kActionQuit:
        if (size == sizeof(proto::QuitPacket))
        {
            proto::QuitPacket packet{};
            std::memcpy(&packet, data, sizeof(packet));
            sessionId = packet.sessionId;
        }
        break;
    case proto::kActionLaunchGo:
        if (size == sizeof(proto::LaunchGoPacket))
        {
            proto::LaunchGoPacket packet{};
            std::memcpy(&packet, data, sizeof(packet));
            sessionId = packet.sessionId;
        }
        break;
    case proto::kActionRollbackReady:
        if (size == sizeof(proto::RollbackReadyPacket))
        {
            proto::RollbackReadyPacket packet{};
            std::memcpy(&packet, data, sizeof(packet));
            sessionId = packet.sessionId;
        }
        break;
    case proto::kActionFrameHash:
        if (size == sizeof(proto::FrameHashPacket))
        {
            proto::FrameHashPacket packet{};
            std::memcpy(&packet, data, sizeof(packet));
            sessionId = packet.sessionId;
        }
        break;
    case proto::kActionBattleEntry:
        if (size == sizeof(proto::BattleEntryPacket))
        {
            proto::BattleEntryPacket packet{};
            std::memcpy(&packet, data, sizeof(packet));
            sessionId = packet.sessionId;
        }
        break;
    default:
        return false;
    }

    const std::uint64_t expected = g_runtimeSessionId.load(std::memory_order_acquire);
    if (sessionId == 0u || sessionId != expected)
    {
        log::Debug(
            "netplay",
            "ignored foreign/stale runtime datagram action=%u size=%zu session=%016llX expected=%016llX",
            static_cast<unsigned>(data[0]),
            size,
            static_cast<unsigned long long>(sessionId),
            static_cast<unsigned long long>(expected));
        return false;
    }
    return true;
}

void DrainReceiveQueue()
{
    std::lock_guard<std::recursive_mutex> socketLock(g_socketMutex);
    if (!g_active.load() || g_socket == INVALID_SOCKET)
    {
        return;
    }

    int packetsThisPoll = 0;
    for (; packetsThisPoll < kReceivePacketsPerPoll;)
    {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        std::uint8_t buffer[1200]{};
        const int received = recvfrom(
            g_socket,
            reinterpret_cast<char*>(buffer),
            sizeof(buffer),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &fromLen);
        if (received <= 0)
        {
            break;
        }

        ++packetsThisPoll;

        // Consume before peer-source validation: coordinator and punch data aren't EFZ packets.
        if (HandleSpectatorAutopunchControl(
                buffer,
                static_cast<std::size_t>(received),
                from))
        {
            continue;
        }
        sockaddr_in mappedPeer{};
        const auto punchResult = g_autopunch.HandleInbound(
            buffer,
            static_cast<std::size_t>(received),
            from,
            &mappedPeer);
        if (punchResult == trialnet::autopunch::InboundResult::PeerMapped
            || punchResult == trialnet::autopunch::InboundResult::PeerObserved)
        {
            const bool endpointMayChange = g_isHost
                ? !g_clientAckedSeed
                : !g_seedSynced.load(std::memory_order_acquire);
            if (endpointMayChange
                && mappedPeer.sin_addr.S_un.S_addr == g_peer.sin_addr.S_un.S_addr
                && mappedPeer.sin_port != 0u)
            {
                const unsigned oldPort = ntohs(g_peer.sin_port);
                g_peer = mappedPeer;
                g_peerAddrLearned.store(true, std::memory_order_release);
                lifecycle::Netplay(
                    "autopunch selected peer endpoint port=%u->%u",
                    oldPort,
                    static_cast<unsigned>(ntohs(g_peer.sin_port)));
                log::Info(
                    "netplay",
                    "autopunch selected peer endpoint port=%u->%u",
                    oldPort,
                    static_cast<unsigned>(ntohs(g_peer.sin_port)));
            }
            continue;
        }
        if (punchResult == trialnet::autopunch::InboundResult::Consumed)
        {
            continue;
        }

        if (HandleLauncherPresenceQuery(
                buffer,
                static_cast<std::size_t>(received),
                from))
        {
            continue;
        }

        if (HandleSpectatorDatagram(
                buffer,
                static_cast<std::size_t>(received),
                from))
        {
            continue;
        }

        if (!g_peerAddrLearned.load(std::memory_order_acquire)
            && buffer[0] >= proto::kActionSpectatorNodeAdvert
            && buffer[0] <= proto::kActionSpectatorClaim)
        {
            // Relay control is valid only after the handshake; it never teaches a source port.
            continue;
        }

        // Learn the client port only from the nonce-bearing hello; launcher ACKs reach here.
        const bool hostAwaitingSeed = g_isHost && !g_clientAckedSeed;
        const bool endpointUnknown = hostAwaitingSeed
            && !g_peerAddrLearned.load(std::memory_order_acquire);
        proto::ExchangeRandomSeedPacket rendezvous{};
        bool isRuntimeRendezvous = false;
        bool isCurrentSeedProof = false;
        if (received == static_cast<int>(sizeof(rendezvous)))
        {
            std::memcpy(&rendezvous, buffer, sizeof(rendezvous));
            isRuntimeRendezvous = proto::IsClientRendezvousHello(rendezvous);
            isCurrentSeedProof = proto::IsClientSeedAck(
                rendezvous,
                g_activeClientNonce,
                g_hostSeedNonce,
                g_hostSeed);
        }
        if (hostAwaitingSeed
            && from.sin_addr.S_un.S_addr == g_peer.sin_addr.S_un.S_addr
            && from.sin_port != 0u
            && (isRuntimeRendezvous || isCurrentSeedProof))
        {
            const bool changed = !g_peerAddrLearned.load(std::memory_order_acquire)
                || g_peer.sin_port != from.sin_port
                || (isRuntimeRendezvous
                    && g_activeClientNonce != rendezvous.clientNonce);
            if (changed)
            {
                const unsigned oldPort = static_cast<unsigned>(ntohs(g_peer.sin_port));
                g_peer.sin_port = from.sin_port;
                if (isRuntimeRendezvous)
                {
                    g_activeClientNonce = rendezvous.clientNonce;
                }
                g_peerAddrLearned.store(true, std::memory_order_release);
                lifecycle::Netplay(
                    "host selected client runtime port=%u->%u clientNonce=%08X proof=%s",
                    oldPort,
                    static_cast<unsigned>(ntohs(from.sin_port)),
                    g_activeClientNonce,
                    isCurrentSeedProof ? "seed-echo" : "hello");
                log::Info(
                    "netplay",
                    "host selected client runtime port=%u->%u clientNonce=%08X proof=%s",
                    oldPort,
                    static_cast<unsigned>(ntohs(from.sin_port)),
                    g_activeClientNonce,
                    isCurrentSeedProof ? "seed-echo" : "hello");
            }
        }
        else if (endpointUnknown)
        {
            continue;
        }

        // Reject other sources: any same-size datagram could mutate histories.
        const bool peerLearned = g_peerAddrLearned.load(std::memory_order_acquire);
        const bool sourceMatches =
            from.sin_addr.S_un.S_addr == g_peer.sin_addr.S_un.S_addr
            && (!peerLearned || from.sin_port == g_peer.sin_port);
        if (!sourceMatches)
        {
            continue;
        }

        // Before the nonce proof, accept only the seed protocol.
        if (received == static_cast<int>(sizeof(proto::ExchangeRandomSeedPacket))
            && buffer[0] == proto::kActionExchangeRandomSeed)
        {
            proto::ExchangeRandomSeedPacket packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            HandleSeedPacket(packet, from);
            continue;
        }
        bool currentTaggedQuitBeforeConfirm = false;
        if (!IsDeterminismSynced()
            && received == static_cast<int>(sizeof(proto::QuitPacket))
            && buffer[0] == proto::kActionQuit)
        {
            proto::QuitPacket quit{};
            std::memcpy(&quit, buffer, sizeof(quit));
            const std::uint64_t currentSessionId =
                g_runtimeSessionId.load(std::memory_order_acquire);
            currentTaggedQuitBeforeConfirm = currentSessionId != 0u
                && quit.sessionId == currentSessionId;
        }
        if (!IsDeterminismSynced() && !currentTaggedQuitBeforeConfirm)
        {
            continue;
        }

        // Consume before liveness clocks: adverts must not keep a wedged session alive.
        if (HandleSpectatorPlayerControl(
                buffer,
                static_cast<std::size_t>(received)))
        {
            continue;
        }

        // Validate first so a previous process's packet is ignored, not treated as malformed.
        if (!IsCurrentRuntimeDatagram(buffer, static_cast<std::size_t>(received)))
        {
            continue;
        }
        g_lastPeerPacketMs.store(GetTickCount(), std::memory_order_release);

        if (received == static_cast<int>(sizeof(proto::QuitPacket))
            && buffer[0] == proto::kActionQuit)
        {
            proto::QuitPacket packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            const bool validReason = packet.reason == proto::QuitReason::GracefulSessionEnd
                || packet.reason == proto::QuitReason::PlayerDisconnected
                || packet.reason == proto::QuitReason::ProtocolAbort;
            const bool reservedClear = std::all_of(
                std::begin(packet.reserved),
                std::end(packet.reserved),
                [](const std::uint8_t byte) { return byte == 0u; });
            if (packet.wireVersion != proto::kRollbackWireVersion || !validReason
                || (packet.flags & ~proto::kQuitFlagAck) != 0u || !reservedClear)
            {
                AbortNetplay("peer sent malformed or incompatible quit packet");
                break;
            }
            if ((packet.flags & proto::kQuitFlagAck) != 0u)
            {
                if (packet.reason == proto::QuitReason::GracefulSessionEnd
                    && !g_peerGracefulQuitAck.exchange(true, std::memory_order_acq_rel))
                {
                    log::Info("netplay", "graceful player shutdown acknowledged by peer");
                }
                continue;
            }

            proto::QuitPacket ack = packet;
            ack.flags |= proto::kQuitFlagAck;
            (void)SendToPeer(&ack, sizeof(ack), "quit-ack", 3);
            if (packet.reason == proto::QuitReason::GracefulSessionEnd)
            {
                if (!g_peerGracefulEndHandling.exchange(true, std::memory_order_acq_rel))
                {
                    std::strncpy(
                        g_abortReason,
                        "peer ended the netplay session normally",
                        sizeof(g_abortReason) - 1u);
                    g_abortReason[sizeof(g_abortReason) - 1u] = '\0';
                    log::Info(
                        "netplay",
                        "peer graceful shutdown received; draining local spectator tail normally");
                    // Must precede g_netplayAborted/lifecycle::Exit, which end this receive-thread drain.
                    NotifySpectatorsSourceEnded(false);
                    g_netplayAborted.store(true, std::memory_order_release);
                    lifecycle::Exit("netplay", g_abortReason, nullptr);
                    CloseSync();
                }
            }
            else if (packet.reason == proto::QuitReason::PlayerDisconnected)
            {
                AbortNetplay("peer reported player disconnect");
            }
            else
            {
                AbortNetplay("peer requested netplay shutdown after protocol failure");
            }
            break;
        }

        if (received == static_cast<int>(sizeof(proto::LaunchGoPacket))
            && buffer[0] == proto::kActionLaunchGo)
        {
            proto::LaunchGoPacket packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            HandleLaunchGoPacket(packet);
            continue;
        }

        if (received == static_cast<int>(sizeof(proto::SyncDataPacket))
            && buffer[0] == proto::kActionSyncData)
        {
            proto::SyncDataPacket packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            HandleSyncPacket(packet);
            continue;
        }

        if (received == static_cast<int>(sizeof(proto::RollbackReadyPacket))
            && buffer[0] == proto::kActionRollbackReady)
        {
            proto::RollbackReadyPacket packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            HandleRollbackReadyPacket(packet);
            continue;
        }

        if (received == static_cast<int>(sizeof(proto::BattleEntryPacket))
            && buffer[0] == proto::kActionBattleEntry)
        {
            proto::BattleEntryPacket packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            if (packet.controlWord == proto::kActionBattleEntry)
            {
                const bool wasKnown = g_peerEntryKnown.load(std::memory_order_acquire);
                const std::uint32_t prevFrame = g_peerEntryFrame.load(std::memory_order_acquire);
                const std::uint32_t prevEpoch = g_peerEntryEpoch.load(std::memory_order_acquire);
                g_peerEntryFrame.store(packet.frame, std::memory_order_release);
                g_peerEntryReady.store(packet.ready != 0u, std::memory_order_release);
                g_peerEntryEpoch.store(packet.epoch, std::memory_order_release);
                g_peerEntryKnown.store(true, std::memory_order_release);
                if (!wasKnown || prevFrame != packet.frame || prevEpoch != packet.epoch)
                {
                    lifecycle::Netplay(
                        "peer battle entry frame=%u ready=%u epoch=%u",
                        packet.frame,
                        static_cast<unsigned>(packet.ready),
                        static_cast<unsigned>(packet.epoch));
                    log::Info(
                        "netplay",
                        "peer battle entry frame=%u ready=%u epoch=%u",
                        packet.frame,
                        static_cast<unsigned>(packet.ready),
                        static_cast<unsigned>(packet.epoch));
                }
            }
            continue;
        }

        if (received == static_cast<int>(sizeof(proto::FrameHashPacket))
            && buffer[0] == proto::kActionFrameHash)
        {
            proto::FrameHashPacket packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            if (packet.controlWord == proto::kActionFrameHash)
            {
                if (packet.regionCount != proto::kFrameHashRegions)
                {
                    AbortNetplay("peer sent invalid confirmed-state hash layout");
                    break;
                }
                if (packet.timeline != g_syncTimeline.load(std::memory_order_acquire))
                {
                    continue;
                }
                rollback::desync::RecordRemote(packet.frame, packet.hash, proto::kFrameHashRegions);
                if (rollback::desync::Mismatches() != 0u)
                {
                    AbortNetplay("confirmed state hash mismatch");
                    break;
                }
            }
            continue;
        }

        lifecycle::Netplay("unknown UDP packet size=%d", received);
    }
    if (packetsThisPoll == kReceivePacketsPerPoll)
    {
        log::Warn("netplay", "receive budget exhausted packets=%d", packetsThisPoll);
    }

    if (packetsThisPoll > 0)
    {
        g_pktRecvTotal.fetch_add(static_cast<std::uint32_t>(packetsThisPoll), std::memory_order_relaxed);
    }
}

bool CanExchangeSyncData()
{
    if (!g_active.load() || g_socket == INVALID_SOCKET || !IsDeterminismSynced()
        || g_netplayAborted.load(std::memory_order_acquire))
    {
        return false;
    }

    if (g_requireRollback && !g_rollbackNegotiated.load(std::memory_order_acquire))
    {
        return false;
    }

    return menu::IsSyncEligibleNow() || rollback::IsEnabled();
}

void PollSyncDataExchange()
{
    if (!CanExchangeSyncData())
    {
        return;
    }

    SendLocalSyncFrame();
}

void PollLaunchGoRetransmit()
{
    if (!g_retransmitLaunchGo.load(std::memory_order_acquire) || !g_isHost
        || !g_active.load() || g_netplayAborted.load(std::memory_order_acquire))
    {
        return;
    }

    if (!g_peerAddrLearned.load(std::memory_order_acquire) || !IsDeterminismSynced())
    {
        return;
    }

    if (g_requireRollback && !g_rollbackNegotiated.load(std::memory_order_acquire))
    {
        return;
    }

    const DWORD now = GetTickCount();
    if (g_lastLaunchGoSend != 0 && now - g_lastLaunchGoSend < kLaunchGoRetransmitMs)
    {
        return;
    }

    SendLaunchGo();
    g_lastLaunchGoSend = now;
}

void PollSeedExchange()
{
    if (IsDeterminismSynced())
    {
        g_seedExchangeActive = false;
        return;
    }

    if (!g_seedExchangeActive)
    {
        return;
    }

    if (g_isHost)
    {
        if (!g_peerAddrLearned.load(std::memory_order_acquire)
            || g_activeClientNonce == 0u)
        {
            return;
        }
        const DWORD now = GetTickCount();
        if (g_lastSeedHandshakeSendMs != 0u
            && now - g_lastSeedHandshakeSendMs < kSeedHandshakeRetransmitMs)
        {
            return;
        }
        g_lastSeedHandshakeSendMs = now;
        proto::ExchangeRandomSeedPacket packet{};
        packet.kind = g_clientAckedSeed
            ? proto::SeedHandshakeKind::HostConfirm
            : proto::SeedHandshakeKind::HostOffer;
        packet.clientNonce = g_activeClientNonce;
        packet.hostNonce = g_hostSeedNonce;
        packet.seed = g_hostSeed;
        if (SendToPeer(
                &packet,
                sizeof(packet),
                g_clientAckedSeed ? "seed-confirm" : "seed-offer"))
        {
            nav::NoteSeedSent();
        }
        return;
    }

    // Beacon the same nonce until HostConfirm: an old Hello can replace the candidate.
    const DWORD now = GetTickCount();
    if (g_lastClientHelloMs == 0 || now - g_lastClientHelloMs >= 200u)
    {
        g_lastClientHelloMs = now;
        proto::ExchangeRandomSeedPacket hello{};
        hello.clientNonce = g_localClientNonce;
        SendToPeer(&hello, sizeof(hello), "client-hello", 1);
    }
}

void ResetEntryCommitState()
{
    g_entryCommitKnown.store(false, std::memory_order_release);
    g_entryCommitPending.store(false, std::memory_order_release);
    g_entryCommitFrame.store(0u, std::memory_order_release);
    g_entryCommitEpoch.store(0u, std::memory_order_release);
    g_entryCommitTimeline.store(0u, std::memory_order_release);
    g_lastEntryCommitSendMs.store(0u, std::memory_order_release);
}

void PollEntryCommitRetransmit()
{
    if (!g_entryCommitPending.load(std::memory_order_acquire))
    {
        return;
    }

    const DWORD now = GetTickCount();
    const DWORD last = g_lastEntryCommitSendMs.load(std::memory_order_acquire);
    if (last != 0u && now - last < 100u)
    {
        return;
    }

    g_lastEntryCommitSendMs.store(now, std::memory_order_release);
    SendBattleEntry(
        g_entryCommitFrame.load(std::memory_order_acquire),
        true,
        g_entryCommitEpoch.load(std::memory_order_acquire),
        true);
}

}

bool InitializeSocketRuntime()
{
    const int existing = g_winsockState.load(std::memory_order_acquire);
    if (existing != 0)
    {
        return existing == 1;
    }

    // No WSACleanup path: the mod owns Winsock until process termination.
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    g_winsockState.store(result == 0 ? 1 : -result, std::memory_order_release);
    return result == 0;
}

int SocketRuntimeError()
{
    const int state = g_winsockState.load(std::memory_order_acquire);
    return state < 0 ? -state : 0;
}

bool InitializeFromSession(const session::LauncherSession& session)
{
    g_sourceEndDrainStarted.store(false, std::memory_order_release);
    g_terminalHandoffDepth.store(0u, std::memory_order_release);
    g_abortOwnerClaimed.store(false, std::memory_order_release);
    g_spectatorTerminalSent = false;
    g_spectatorTerminalReason = spectator::protocol::TerminalReason::SessionEnded;
    lifecycle::Netplay(
        "InitializeFromSession host=%d port=%u peerPort=%u delay=%u rollback=%d target_fps=%u launcherAutopunch=%d peer=%ls",
        session.isHost ? 1 : 0,
        static_cast<unsigned>(session.port),
        static_cast<unsigned>(session.peerPort),
        static_cast<unsigned>(session.inputDelay),
        session.requireRollback ? 1 : 0,
        static_cast<unsigned>(session.targetFps),
        session.usedAutopunch ? 1 : 0,
        session.peerIp.c_str());
    log::Step("netplay", "sync InitializeFromSession");
    CloseSync();

    if (session.mode != session::LauncherMode::Host && session.mode != session::LauncherMode::Client)
    {
        return false;
    }

    if (!proto::IsSupportedTargetFps(session.targetFps)
        || pacing::GetSessionTargetFps() != session.targetFps)
    {
        lifecycle::Fault(
            "netplay",
            "invalid/unapplied frame-rate contract descriptor=%u runtime=%u",
            static_cast<unsigned>(session.targetFps),
            static_cast<unsigned>(pacing::GetSessionTargetFps()));
        log::Error(
            "netplay",
            "invalid/unapplied frame-rate contract descriptor=%u runtime=%u",
            static_cast<unsigned>(session.targetFps),
            static_cast<unsigned>(pacing::GetSessionTargetFps()));
        return false;
    }

    g_isHost = session.isHost;
    g_port = session.port;
    g_peerPort = session.peerPort != 0 ? session.peerPort : session.port;
    g_peerNarrow = Narrow(session.peerIp);
    g_requireRollback = session.requireRollback;
    g_launchGo.store(false);
    g_syncFrame.store(0);
    g_syncTimeline.store(0, std::memory_order_release);
    g_syncGeneration.store(0, std::memory_order_release);
    g_lastSyncArrivalMs.store(0, std::memory_order_release);
    g_lastSyncRecvMs.store(0, std::memory_order_release);
    g_syncProgressRemoteFrontier.store(0u, std::memory_order_release);
    g_syncProgressAckFrontier.store(0u, std::memory_order_release);
    g_syncProgressTimeline.store(0u, std::memory_order_release);
    g_syncProgressWasNeeded.store(false, std::memory_order_release);
    g_lastPeerPacketMs.store(0, std::memory_order_release);
    g_transportStartMs.store(GetTickCount(), std::memory_order_release);
    rollback::desync::Reset();
    g_peerRollbackAck = false;
    g_abortReason[0] = '\0';
    g_netplayAborted.store(false, std::memory_order_release);
    g_peerGracefulQuitAck.store(false, std::memory_order_release);
    g_peerGracefulEndHandling.store(false, std::memory_order_release);
    g_negotiatedInputDelay.store(-1, std::memory_order_release);
    g_negotiatedTargetFps.store(-1, std::memory_order_release);
    g_requestedRollbackFrames = std::min(session.rollbackFrames, proto::kMaxRollbackFrames);
    g_requestedTargetFps = session.targetFps;
    g_negotiatedRollbackFrames.store(
        session.isHost && session.requireRollback ? static_cast<int>(g_requestedRollbackFrames) : -1,
        std::memory_order_release);
    g_peerAddrLearned.store(!session.isHost, std::memory_order_release);
    g_lastClientHelloMs = 0;
    g_peerEntryKnown.store(false, std::memory_order_release);
    g_peerEntryReady.store(false, std::memory_order_release);
    g_peerEntryFrame.store(0, std::memory_order_release);
    g_peerEntryEpoch.store(0, std::memory_order_release);
    g_lastBattleEntrySendMs.store(0u, std::memory_order_release);
    ResetEntryCommitState();
    g_rollbackNegotiated.store(!g_requireRollback, std::memory_order_release);
    g_spectatorRelayEpoch = 0u;
    if (g_isHost
        && (!win32::SecureRandomBytes(&g_spectatorRelayEpoch, sizeof(g_spectatorRelayEpoch))
            || g_spectatorRelayEpoch == 0u))
    {
        g_spectatorRelayEpoch = 0u;
        log::Warn(
            "spectator",
            "secure relay epoch generation failed; direct spectator service remains available");
    }
    else if (g_isHost)
    {
        log::Info(
            "spectator",
            "SPECTATOR RELAY coordinator epoch=%08X directSoftLimit=%zu",
            g_spectatorRelayEpoch,
            SpectatorRelaySoftLimit());
    }

    if (g_requireRollback)
    {
        if (!rollback::kCompiledIn)
        {
            AbortNetplay("rollback module not compiled in");
            return false;
        }
        if (!rollback::IsArmedForNetplay() && !rollback::IsEnabled())
        {
            AbortNetplay("rollback not armed for netplay session");
            return false;
        }
        log::Step("netplay", "rollback required — awaiting peer rollback-ready (activates at battle)");
    }
    else
    {
        log::Step("netplay", "lockstep netplay — SyncData only (rollback disabled in session)");
    }

    g_active.store(OpenSocket());

    lockstep::Reset();
    rollback::SetInputDelay(session.inputDelay);
    lockstep::SetPingMs(session.pingMs);
    rollback::SetPingMs(session.pingMs);

    if (g_active.load())
    {
        if (!BeginSeedExchange())
        {
            lifecycle::Fault("netplay", "secure determinism handshake initialization failed");
            CloseSync();
            return false;
        }
        lifecycle::Netplay(
            "UDP open %s port=%u peer=%s",
            g_isHost ? "host" : "client",
            static_cast<unsigned>(g_port),
            g_peerNarrow.c_str());
        log::Info(
            "netplay",
            "sync UDP %s port=%u peer=%s",
            g_isHost ? "host" : "client",
            static_cast<unsigned>(g_port),
            g_peerNarrow.c_str());
    }
    else
    {
        lifecycle::Fault("netplay", "UDP open failed host=%d port=%u", g_isHost ? 1 : 0, static_cast<unsigned>(g_port));
    }
    return g_active.load();
}

void CloseSync()
{
    std::lock_guard<std::recursive_mutex> transportLock(g_transportOwnerMutex);
    lifecycle::Netplay("CloseSync");
    log::Step("netplay", "sync CloseSync");
    lockstep::Reset();
    g_active.store(false);
    g_launchGo.store(false);
    g_retransmitLaunchGo.store(false, std::memory_order_release);
    g_lastLaunchGoSend = 0;
    g_seedSynced.store(false);
    g_seedHandshakeConfirmed.store(false, std::memory_order_release);
    g_syncedSeed.store(0);
    g_runtimeSessionId.store(0u, std::memory_order_release);
    g_syncFrame.store(0);
    g_syncTimeline.store(0, std::memory_order_release);
    g_syncGeneration.store(0, std::memory_order_release);
    g_lastSyncArrivalMs.store(0, std::memory_order_release);
    g_lastSyncRecvMs.store(0, std::memory_order_release);
    g_syncProgressRemoteFrontier.store(0u, std::memory_order_release);
    g_syncProgressAckFrontier.store(0u, std::memory_order_release);
    g_syncProgressTimeline.store(0u, std::memory_order_release);
    g_syncProgressWasNeeded.store(false, std::memory_order_release);
    g_lastPeerPacketMs.store(0, std::memory_order_release);
    g_transportStartMs.store(0, std::memory_order_release);
    g_seedExchangeActive = false;
    g_clientAckedSeed = false;
    g_hostSeed = 0;
    g_hostSeedNonce = 0u;
    g_localClientNonce = 0u;
    g_activeClientNonce = 0u;
    g_acceptedHostNonce = 0u;
    g_acceptedHostSeed = 0u;
    g_requireRollback = false;
    g_peerRollbackAck = false;
    g_peerGracefulQuitAck.store(false, std::memory_order_release);
    g_peerGracefulEndHandling.store(false, std::memory_order_release);
    g_negotiatedInputDelay.store(-1, std::memory_order_release);
    g_negotiatedRollbackFrames.store(-1, std::memory_order_release);
    g_negotiatedTargetFps.store(-1, std::memory_order_release);
    g_requestedRollbackFrames = 0;
    g_requestedTargetFps = proto::kDefaultTargetFps;
    g_peerAddrLearned.store(false, std::memory_order_release);
    g_lastClientHelloMs = 0;
    g_lastSeedHandshakeSendMs = 0u;
    g_peerEntryKnown.store(false, std::memory_order_release);
    g_peerEntryReady.store(false, std::memory_order_release);
    g_peerEntryFrame.store(0, std::memory_order_release);
    g_peerEntryEpoch.store(0, std::memory_order_release);
    g_lastBattleEntrySendMs.store(0u, std::memory_order_release);
    ResetEntryCommitState();
    g_rollbackNegotiated.store(false, std::memory_order_release);
    g_spectators.clear();
    g_spectatorRoundRobin = 0u;
    g_spectatorPunchCandidates.clear();
    g_peerSpectatorNode = {};
    g_hostSpectatorGrants.clear();
    g_clientSpectatorGrants.clear();
    g_spectatorRelayEpoch = 0u;
    g_lastSpectatorNodeAdvertMs = 0u;
    CloseSocket();
}

void Poll()
{
    std::lock_guard<std::recursive_mutex> transportLock(g_transportOwnerMutex);
    const auto terminalOrClosed = []() {
        return g_terminalHandoffDepth.load(std::memory_order_acquire) != 0u
            || g_netplayAborted.load(std::memory_order_acquire)
            || !g_active.load(std::memory_order_acquire)
            || g_socket == INVALID_SOCKET;
    };
    if (terminalOrClosed())
    {
        return;
    }

    ServiceAutopunch();
    FlushDelayedSends();
    DrainReceiveQueue();
    if (terminalOrClosed())
    {
        return;
    }
    CheckPeerLiveness();
    if (terminalOrClosed())
    {
        return;
    }
    PollEntryCommitRetransmit();
    PollSeedExchange();
    PollRollbackNegotiation();
    if (terminalOrClosed())
    {
        return;
    }
    PollLaunchGoRetransmit();
    PollSyncDataExchange();
    if (terminalOrClosed())
    {
        return;
    }
    ServiceSpectatorRelayControl();
    // Fan-out is last: a full spectator budget must not delay the player's sends.
    PollSpectatorSubscribers();
}

void LogDiagnosticHeartbeat()
{
    if (!g_active.load(std::memory_order_acquire))
    {
        return;
    }

    static std::uint32_t s_sent = 0u;
    static std::uint32_t s_recv = 0u;
    static std::uint32_t s_syncS = 0u;
    static std::uint32_t s_syncR = 0u;

    const std::uint32_t sent = g_pktSentTotal.load(std::memory_order_relaxed);
    const std::uint32_t recv = g_pktRecvTotal.load(std::memory_order_relaxed);
    const std::uint32_t syncS = g_syncSentTotal.load(std::memory_order_relaxed);
    const std::uint32_t syncR = g_syncRecvTotal.load(std::memory_order_relaxed);

    const det::rng::Status rng = det::rng::GetStatus();
    const int memorial = rollback::MapMemorialScreenIndexFromGame();
    const unsigned fpuCw = det::GetCapturedFpuCw();
    const DWORD lastSyncMs = g_lastSyncRecvMs.load(std::memory_order_acquire);
    const DWORD lastArrivalMs = g_lastSyncArrivalMs.load(std::memory_order_acquire);
    const DWORD now = GetTickCount();
    const DWORD progressAgeMs = lastSyncMs != 0u ? now - lastSyncMs : 0u;
    const DWORD arrivalAgeMs = lastArrivalMs != 0u ? now - lastArrivalMs : 0u;
    const bool rngHooked = rng.iatRandHooked || rng.staticRandHooked;

    const lockstep::HandoffStatus ho = lockstep::EvaluateHandoff();
    const rollback::Status rb = rollback::GetStatus();

    log::Info(
        "netplay",
        "HEARTBEAT screen=%d rollback=%d | pkt sent=%u(+%u) recv=%u(+%u) sync[s=%u(+%u) r=%u(+%u)] arrivalAge=%ums progressAge=%ums needProgress=%d "
        "| fpuCW=%04X rng[hook=%d shadow=%08X rand=%u srand=%u] "
        "| rb[cur=%u L=%zu R=%zu ack=%u iter=%d] | handoff[ready=%d %s sim=%u L=%zu R=%zu]",
        memorial,
        rollback::IsEnabled() ? 1 : 0,
        sent, sent - s_sent,
        recv, recv - s_recv,
        syncS, syncS - s_syncS,
        syncR, syncR - s_syncR,
        static_cast<unsigned>(arrivalAgeMs),
        static_cast<unsigned>(progressAgeMs),
        g_syncProgressWasNeeded.load(std::memory_order_acquire) ? 1 : 0,
        fpuCw,
        rngHooked ? 1 : 0,
        rng.shadowState,
        rng.randCalls,
        rng.srandCalls,
        rb.currentFrame,
        rb.localInputCount,
        rb.remoteInputCount,
        rb.peerAckFrame,
        rollback::GetLastMainTickIterations(),
        ho.ready ? 1 : 0,
        lockstep::HandoffBlockReasonName(ho.blockReason),
        ho.simFrame,
        ho.localSize,
        ho.remoteSize);

    if (sent - s_sent == 0u || recv - s_recv == 0u)
    {
        lifecycle::Sync(
            "heartbeat",
            "PACKET FLOW STALLED screen=%d sent+%u recv+%u arrivalAge=%ums progressAge=%ums — transfer may be one-sided",
            memorial,
            sent - s_sent,
            recv - s_recv,
            static_cast<unsigned>(arrivalAgeMs),
            static_cast<unsigned>(progressAgeMs));
    }

    s_sent = sent;
    s_recv = recv;
    s_syncS = syncS;
    s_syncR = syncR;
}

void SendFrameHash(std::uint32_t frame, const std::uint32_t* regionHashes, int count)
{
    if (!g_active.load() || g_netplayAborted.load(std::memory_order_acquire) || regionHashes == nullptr)
    {
        return;
    }
    proto::FrameHashPacket packet{};
    packet.sessionId = g_runtimeSessionId.load(std::memory_order_acquire);
    packet.timeline = g_syncTimeline.load(std::memory_order_acquire);
    packet.frame = frame;
    packet.regionCount = static_cast<std::uint8_t>(proto::kFrameHashRegions);
    const int n = count < proto::kFrameHashRegions ? count : proto::kFrameHashRegions;
    for (int r = 0; r < n; ++r)
    {
        packet.hash[r] = regionHashes[r];
    }
    SendToPeer(&packet, sizeof(packet), "frame-hash", 1);
}

bool LaunchGoReceived()
{
    return g_launchGo.load(std::memory_order_acquire);
}

void SetLaunchGoRetransmit(const bool enabled)
{
    g_retransmitLaunchGo.store(enabled, std::memory_order_release);
    if (enabled)
    {
        g_lastLaunchGoSend = 0;
    }
}

void SendLaunchGo()
{
    if (!g_active.load())
    {
        return;
    }

    if (!IsDeterminismSynced())
    {
        lifecycle::Fault("netplay", "launch-go blocked — RNG/FPU not synced yet");
        log::Error("netplay", "launch-go blocked — RNG/FPU not synced yet");
        return;
    }

    if (g_requireRollback && !g_rollbackNegotiated.load(std::memory_order_acquire))
    {
        lifecycle::Fault("netplay", "launch-go blocked — rollback not negotiated");
        log::Error("netplay", "launch-go blocked — rollback not negotiated with peer");
        return;
    }

    proto::LaunchGoPacket packet{};
    packet.sessionId = g_runtimeSessionId.load(std::memory_order_acquire);
    const session::LauncherSession* session = session::GetCachedSession();
    if (session != nullptr)
    {
        packet.inputDelay = std::min(session->inputDelay, proto::kMaxInputDelay);
        packet.targetFps = session->targetFps;
    }
    if (!proto::IsSupportedTargetFps(packet.targetFps)
        || packet.targetFps != g_requestedTargetFps
        || pacing::GetSessionTargetFps() != packet.targetFps)
    {
        AbortNetplaySession("local launch-go frame-rate contract is invalid");
        return;
    }
    packet.launchAtMs = 0u;

    if (SendToPeer(&packet, sizeof(packet), "launch-go"))
    {
        const bool first = !g_launchGo.exchange(true, std::memory_order_acq_rel);
        rollback::SetInputDelay(packet.inputDelay);
        g_negotiatedInputDelay.store(static_cast<int>(packet.inputDelay), std::memory_order_release);
        g_negotiatedTargetFps.store(static_cast<int>(packet.targetFps), std::memory_order_release);
        if (first)
        {
            nav::NoteLaunchGoSent();
            nav::SetPacketStage(nav::PacketStage::LaunchGo);
            lifecycle::Netplay(
                "launch-go sent delay=%u target_fps=%u",
                static_cast<unsigned>(packet.inputDelay),
                static_cast<unsigned>(packet.targetFps));
            log::Info(
                "netplay",
                "launch-go sent (delay=%u target_fps=%u)",
                static_cast<unsigned>(packet.inputDelay),
                static_cast<unsigned>(packet.targetFps));
        }
    }
}

bool IsDeterminismSynced()
{
    return g_seedSynced.load(std::memory_order_acquire)
        && g_seedHandshakeConfirmed.load(std::memory_order_acquire)
        && g_runtimeSessionId.load(std::memory_order_acquire) != 0u;
}

int GetNegotiatedInputDelay()
{
    return g_negotiatedInputDelay.load(std::memory_order_acquire);
}

int GetNegotiatedRollbackFrames()
{
    return g_negotiatedRollbackFrames.load(std::memory_order_acquire);
}

int GetNegotiatedTargetFps()
{
    return g_negotiatedTargetFps.load(std::memory_order_acquire);
}

void SendBattleEntry(const std::uint32_t frame, const bool ready, const std::uint32_t epoch, const bool force)
{
    if (!g_active.load() || g_netplayAborted.load(std::memory_order_acquire))
    {
        return;
    }

    // A committed entry bypasses this throttle until matching-timeline traffic arrives.
    const DWORD now = GetTickCount();
    const DWORD last = g_lastBattleEntrySendMs.load(std::memory_order_acquire);
    if (!force && last != 0u && now - last < 100u)
    {
        return;
    }
    g_lastBattleEntrySendMs.store(now, std::memory_order_release);

    proto::BattleEntryPacket packet{};
    packet.ready = ready ? 1u : 0u;
    packet.epoch = epoch;
    packet.frame = frame;
    packet.sessionId = g_runtimeSessionId.load(std::memory_order_acquire);
    SendToPeer(&packet, sizeof(packet), "battle-entry");
}

bool GetPeerBattleEntry(std::uint32_t* outFrame, bool* outReady, const std::uint32_t epoch)
{
    if (!g_peerEntryKnown.load(std::memory_order_acquire))
    {
        return false;
    }
    if (g_peerEntryEpoch.load(std::memory_order_acquire) != epoch)
    {
        return false;
    }
    if (outFrame != nullptr)
    {
        *outFrame = g_peerEntryFrame.load(std::memory_order_acquire);
    }
    if (outReady != nullptr)
    {
        *outReady = g_peerEntryReady.load(std::memory_order_acquire);
    }
    return true;
}

void CommitBattleEntry(const std::uint32_t frame, const std::uint32_t epoch)
{
    if (!g_active.load(std::memory_order_acquire)
        || g_netplayAborted.load(std::memory_order_acquire))
    {
        return;
    }

    if (g_entryCommitKnown.load(std::memory_order_acquire)
        && g_entryCommitEpoch.load(std::memory_order_acquire) == epoch)
    {
        const std::uint32_t committedFrame =
            g_entryCommitFrame.load(std::memory_order_acquire);
        if (committedFrame != frame)
        {
            lifecycle::Fault(
                "netplay",
                "battle entry recommit mismatch epoch=%u oldFrame=%u newFrame=%u",
                epoch,
                committedFrame,
                frame);
            AbortNetplay("battle-entry commit changed after timeline advance");
        }
        return;
    }

    const std::uint32_t generation = g_syncGeneration.load(std::memory_order_acquire);
    if (epoch != proto::kBattleEntryEpochHandoff && epoch != generation)
    {
        lifecycle::Fault(
            "netplay",
            "battle entry commit generation mismatch epoch=%u generation=%u frame=%u",
            epoch,
            generation,
            frame);
        AbortNetplay("battle-entry generation mismatch");
        return;
    }

    const std::uint16_t nextTimeline = static_cast<std::uint16_t>(
        g_syncTimeline.load(std::memory_order_acquire) + 1u);
    g_entryCommitFrame.store(frame, std::memory_order_release);
    g_entryCommitEpoch.store(epoch, std::memory_order_release);
    g_entryCommitTimeline.store(nextTimeline, std::memory_order_release);
    g_lastEntryCommitSendMs.store(0u, std::memory_order_release);
    g_entryCommitKnown.store(true, std::memory_order_release);
    g_entryCommitPending.store(true, std::memory_order_release);

    // Advertise before crossing so a peer still on the old timeline can follow.
    SendBattleEntry(frame, true, epoch, true);
    AdvanceSyncTimeline();
    lifecycle::Netplay(
        "battle entry committed frame=%u epoch=%u timeline=%u (awaiting peer proof)",
        frame,
        epoch,
        static_cast<unsigned>(nextTimeline));
    log::Step(
        "netplay",
        "battle entry committed frame=%u epoch=%u timeline=%u (awaiting peer proof)",
        frame,
        epoch,
        static_cast<unsigned>(nextTimeline));
}

void ResetBattleEntryExchange()
{
    // Staleness uses the epoch tag: a new round's hold must not wipe a delivered advert.
    g_peerEntryKnown.store(false, std::memory_order_release);
    g_peerEntryReady.store(false, std::memory_order_release);
    g_peerEntryFrame.store(0, std::memory_order_release);
    g_peerEntryEpoch.store(0, std::memory_order_release);
    g_lastBattleEntrySendMs.store(0u, std::memory_order_release);
    ResetEntryCommitState();
}

bool IsRollbackNegotiated()
{
    return g_rollbackNegotiated.load(std::memory_order_acquire);
}

bool IsRollbackRequired()
{
    return g_requireRollback;
}

bool IsNetplayAborted()
{
    return g_netplayAborted.load(std::memory_order_acquire);
}

bool IsTerminalHandoffInProgress()
{
    return g_terminalHandoffDepth.load(std::memory_order_acquire) != 0u;
}

void AbortNetplaySession(const char* reason)
{
    AbortNetplay(reason);
}

void CompleteHarnessSession()
{
    if (!harness::IsEnabled())
    {
        AbortNetplay("graceful harness completion requested outside the harness");
        return;
    }
    if (g_netplayAborted.load(std::memory_order_acquire))
    {
        return;
    }
    std::strncpy(g_abortReason, "harness complete", sizeof(g_abortReason) - 1u);
    g_abortReason[sizeof(g_abortReason) - 1u] = '\0';
    lifecycle::Exit("netplay", g_abortReason, "graceful test-session handoff");

    SendQuit();
    NotifySpectatorsSourceEnded(false);
    CloseSync();
    // Publish completion only after the terminal owner: bootstrap closes on this flag.
    g_netplayAborted.store(true, std::memory_order_release);
}

void SendQuit()
{
    if (!g_active.load(std::memory_order_acquire))
    {
        return;
    }
    proto::QuitPacket packet{};
    packet.reason = proto::QuitReason::GracefulSessionEnd;
    packet.sessionId = g_runtimeSessionId.load(std::memory_order_acquire);
    g_peerGracefulQuitAck.store(false, std::memory_order_release);
    if (!harness::NetShimEnabled())
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            (void)SendToPeer(&packet, sizeof(packet), "graceful-quit");
        }
        return;
    }

    const DWORD deadline = GetTickCount() + 500u;
    DWORD nextSendMs = 0u;
    int attempts = 0;
    while (static_cast<std::int32_t>(deadline - GetTickCount()) > 0)
    {
        const DWORD now = GetTickCount();
        if (attempts < 8 && (nextSendMs == 0u
            || static_cast<std::int32_t>(now - nextSendMs) >= 0))
        {
            (void)SendToPeer(&packet, sizeof(packet), "graceful-quit", 3);
            ++attempts;
            nextSendMs = now + 50u;
        }
        FlushDelayedSends();
        Sleep(5u);
    }
    FlushDelayedSends();
}

void NotifySpectatorsSourceEnded(const bool playerDisconnected)
{
    std::lock_guard<std::recursive_mutex> transportLock(g_transportOwnerMutex);
    if (g_sourceEndDrainStarted.exchange(true, std::memory_order_acq_rel))
    {
        log::Info(
            "spectator",
            "duplicate source-end drain suppressed disconnected=%d",
            playerDisconnected ? 1 : 0);
        return;
    }

    struct TerminalHandoffScope
    {
        TerminalHandoffScope()
        {
            g_terminalHandoffDepth.fetch_add(1u, std::memory_order_acq_rel);
        }
        ~TerminalHandoffScope()
        {
            g_terminalHandoffDepth.fetch_sub(1u, std::memory_order_acq_rel);
        }
    } terminalHandoffScope;

    const DWORD entryNow = GetTickCount();
    const bool unsettledRelay = !playerDisconnected
        && (std::any_of(
                g_hostSpectatorGrants.begin(),
                g_hostSpectatorGrants.end(),
                [entryNow](const HostSpectatorGrant& grant) {
                    return !grant.claimed && !grant.failed && !grant.relayRejected
                        && !TickDeadlinePassed(entryNow, grant.expiresAtMs);
                })
            || std::any_of(
                g_clientSpectatorGrants.begin(),
                g_clientSpectatorGrants.end(),
                [entryNow](const ClientSpectatorGrant& grant) {
                    return grant.status == proto::SpectatorGrantStatus::Ready
                        && !grant.consumed
                        && !TickDeadlinePassed(entryNow, grant.expiresAtMs);
                }));
    if ((g_spectators.empty() && !unsettledRelay) || g_socket == INVALID_SOCKET)
    {
        return;
    }

    const auto drainSpectatorReplies = []() {
        std::lock_guard<std::recursive_mutex> socketLock(g_socketMutex);
        for (int packet = 0; packet < 256; ++packet)
        {
            sockaddr_in from{};
            int fromLength = sizeof(from);
            std::uint8_t bytes[1200]{};
            const int received = recvfrom(
                g_socket,
                reinterpret_cast<char*>(bytes),
                sizeof(bytes),
                0,
                reinterpret_cast<sockaddr*>(&from),
                &fromLength);
            if (received <= 0)
            {
                break;
            }
            if (SameEndpoint(from, g_peer)
                && received == static_cast<int>(sizeof(proto::QuitPacket))
                && bytes[0] == proto::kActionQuit)
            {
                proto::QuitPacket quit{};
                std::memcpy(&quit, bytes, sizeof(quit));
                const bool validReason =
                    quit.reason == proto::QuitReason::GracefulSessionEnd
                    || quit.reason == proto::QuitReason::PlayerDisconnected
                    || quit.reason == proto::QuitReason::ProtocolAbort;
                const bool reservedClear = std::all_of(
                    std::begin(quit.reserved),
                    std::end(quit.reserved),
                    [](const std::uint8_t byte) { return byte == 0u; });
                if (quit.wireVersion == proto::kRollbackWireVersion && validReason
                    && (quit.flags & ~proto::kQuitFlagAck) == 0u && reservedClear
                    && quit.sessionId
                        == g_runtimeSessionId.load(std::memory_order_acquire))
                {
                    if ((quit.flags & proto::kQuitFlagAck) != 0u)
                    {
                        g_peerGracefulQuitAck.store(true, std::memory_order_release);
                    }
                    else
                    {
                        proto::QuitPacket ack = quit;
                        ack.flags |= proto::kQuitFlagAck;
                        (void)SendToPeer(&ack, sizeof(ack), "quit-ack-tail", 3);
                    }
                }
                else
                {
                    log::Warn("netplay", "malformed quit ignored during terminal drain");
                }
                continue;
            }
            if (HandleSpectatorAutopunchControl(
                    bytes,
                    static_cast<std::size_t>(received),
                    from))
            {
                continue;
            }
            if (SameEndpoint(from, g_peer)
                && HandleSpectatorPlayerControl(
                    bytes,
                    static_cast<std::size_t>(received)))
            {
                continue;
            }
            (void)HandleSpectatorDatagram(
                bytes,
                static_cast<std::size_t>(received),
                from);
        }
    };
    const auto allAtConfirmedTail = []() {
        const DWORD now = GetTickCount();
        const bool relayStillSettling = std::any_of(
            g_hostSpectatorGrants.begin(),
            g_hostSpectatorGrants.end(),
            [now](const HostSpectatorGrant& grant) {
                return !grant.claimed && !grant.failed && !grant.relayRejected
                    && !TickDeadlinePassed(now, grant.expiresAtMs);
            }) || std::any_of(
            g_clientSpectatorGrants.begin(),
            g_clientSpectatorGrants.end(),
            [now](const ClientSpectatorGrant& grant) {
                return grant.status == proto::SpectatorGrantStatus::Ready
                    && !grant.consumed && !TickDeadlinePassed(now, grant.expiresAtMs);
            });
        if (relayStillSettling)
        {
            return false;
        }
        const spectator::Status recorder = spectator::GetStatus();
        if (recorder.gameId == 0u)
        {
            return true;
        }
        for (const SpectatorSubscriber& subscriber : g_spectators)
        {
            spectator::GameExtent extent{};
            if (!subscriber.initAcked || subscriber.gameId == 0u
                || !spectator::GetGameExtent(subscriber.gameId, &extent)
                || extent.hasNextGame
                || subscriber.ackedRecordIndex < extent.frameCount)
            {
                return false;
            }
        }
        return true;
    };

    const auto confirmedFramesBehind = [](const SpectatorSubscriber& subscriber) {
        const spectator::Status recorder = spectator::GetStatus();
        if (recorder.gameId == 0u)
        {
            return std::uint64_t{0u};
        }
        if (subscriber.gameId == 0u || subscriber.gameId > recorder.gameId)
        {
            return static_cast<std::uint64_t>(recorder.retainedFrames);
        }

        std::uint64_t remaining = 0u;
        for (std::uint32_t gameId = subscriber.gameId; gameId <= recorder.gameId; ++gameId)
        {
            spectator::GameExtent extent{};
            if (!spectator::GetGameExtent(gameId, &extent))
            {
                return static_cast<std::uint64_t>(recorder.retainedFrames);
            }
            const std::size_t cursor = gameId == subscriber.gameId
                ? std::min<std::size_t>(subscriber.ackedRecordIndex, extent.frameCount)
                : 0u;
            remaining += static_cast<std::uint64_t>(extent.frameCount - cursor);
            if (gameId == std::numeric_limits<std::uint32_t>::max())
            {
                break;
            }
        }
        return remaining;
    };

    std::uint64_t largestBacklog = 0u;
    for (const SpectatorSubscriber& subscriber : g_spectators)
    {
        largestBacklog = std::max(largestBacklog, confirmedFramesBehind(subscriber));
    }
    const std::uint64_t estimatedTransferMs = (largestBacklog * 1000ull + 999ull) / 1000ull;
    const std::uint64_t networkAllowanceMs = std::max<std::uint64_t>(
        5000ull,
        2000ull + static_cast<std::uint64_t>(lockstep::GetPingMs()) * 8ull);
    const DWORD tailDrainMs = playerDisconnected
        ? 2000u
        : static_cast<DWORD>(std::min<std::uint64_t>(
            kSpectatorNormalTailMaxMs,
            std::max<std::uint64_t>(
                unsettledRelay ? kSpectatorGrantTtlMs + 2000ull : 5000ull,
                estimatedTransferMs + networkAllowanceMs)));
    log::Info(
        "spectator",
        "source-end relay drain viewers=%zu relayGrants=%zu/%zu unsettled=%d largestBacklog=%llu budgetMs=%lu disconnected=%d",
        g_spectators.size(),
        g_hostSpectatorGrants.size(),
        g_clientSpectatorGrants.size(),
        unsettledRelay ? 1 : 0,
        static_cast<unsigned long long>(largestBacklog),
        static_cast<unsigned long>(tailDrainMs),
        playerDisconnected ? 1 : 0);
    const DWORD tailDeadline = GetTickCount() + tailDrainMs;
    while (!allAtConfirmedTail()
        && static_cast<std::int32_t>(tailDeadline - GetTickCount()) > 0)
    {
        FlushDelayedSends();
        ServiceSpectatorRelayControl();
        PollSpectatorSubscribers();
        drainSpectatorReplies();
        Sleep(10u);
    }

    const auto reason = playerDisconnected
        ? spectator::protocol::TerminalReason::PlayerDisconnected
        : spectator::protocol::TerminalReason::SessionEnded;
    SendSpectatorTerminal(reason);
    DWORD lastTerminalSend = GetTickCount();
    const DWORD terminalDeadline = lastTerminalSend + 2000u;
    for (;;)
    {
        FlushDelayedSends();
        drainSpectatorReplies();
        const bool allAcked = std::all_of(
            g_spectators.begin(),
            g_spectators.end(),
            [](const SpectatorSubscriber& subscriber) { return subscriber.terminalAcked; });
        if (allAcked
            || static_cast<std::int32_t>(terminalDeadline - GetTickCount()) <= 0)
        {
            break;
        }
        const DWORD now = GetTickCount();
        if (now - lastTerminalSend >= 100u)
        {
            SendSpectatorTerminal(reason);
            lastTerminalSend = now;
        }
        Sleep(10u);
    }
}

const char* GetNetplayAbortReason()
{
    return g_abortReason[0] != '\0' ? g_abortReason : "netplay session aborted";
}

std::uint32_t GetSyncedRngSeed()
{
    return g_syncedSeed.load(std::memory_order_acquire);
}

bool IsSyncTransportActive()
{
    return g_active.load(std::memory_order_acquire);
}

std::uint32_t GetRemoteSyncFrontier()
{
    return g_syncFrame.load(std::memory_order_acquire);
}

std::uint32_t GetSyncGeneration()
{
    return g_syncGeneration.load(std::memory_order_acquire);
}

void AdvanceSyncTimeline()
{
    const std::uint16_t next = static_cast<std::uint16_t>(
        g_syncTimeline.fetch_add(1u, std::memory_order_acq_rel) + 1u);
    const std::uint32_t generation =
        g_syncGeneration.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    g_syncFrame.store(0u, std::memory_order_release);
    std::uint32_t remoteFrontier = 0u;
    std::uint32_t ackFrontier = 0u;
    if (rollback::IsEnabled())
    {
        const rollback::Status status = rollback::GetStatus();
        remoteFrontier = static_cast<std::uint32_t>(std::min<std::size_t>(
            status.remoteInputCount,
            std::numeric_limits<std::uint32_t>::max()));
        ackFrontier = status.peerAckFrame;
    }
    else
    {
        remoteFrontier = lockstep::GetRemoteHistorySize();
        ackFrontier = lockstep::GetPeerAckFrame();
    }
    g_syncProgressTimeline.store(next, std::memory_order_release);
    g_syncProgressRemoteFrontier.store(remoteFrontier, std::memory_order_release);
    g_syncProgressAckFrontier.store(ackFrontier, std::memory_order_release);
    g_syncProgressWasNeeded.store(false, std::memory_order_release);
    g_lastSyncRecvMs.store(GetTickCount(), std::memory_order_release);
    lifecycle::Netplay(
        "SyncData timeline advanced to %u generation=%u",
        static_cast<unsigned>(next),
        generation);
    log::Step(
        "netplay",
        "SyncData timeline advanced to %u generation=%u",
        static_cast<unsigned>(next),
        generation);
}

void SendLocalSyncFrame()
{
    if (!CanExchangeSyncData())
    {
        return;
    }

    proto::SyncDataPacket packet{};
    if (rollback::IsEnabled())
    {
        rollback::FillSyncDataPacket(&packet);
    }
    else
    {
        lockstep::FillOutgoingPacket(&packet);
    }
    packet.timeline = g_syncTimeline.load(std::memory_order_acquire);
    packet.sessionId = g_runtimeSessionId.load(std::memory_order_acquire);

    if (SendToPeer(&packet, sizeof(packet), "SyncData", 2))
    {
        g_syncSentTotal.fetch_add(1, std::memory_order_relaxed);
        nav::NoteSyncSent();
        const int localSide = caster::GetLocalSide();
        const std::size_t tailIndex =
            packet.currentFrame > 0u
                ? (std::min<std::size_t>(
                      static_cast<std::size_t>(packet.currentFrame),
                      proto::kMaxQueueSlot)
                  - 1u)
                : 0u;
        log::Debug(
            "netplay",
            "sent SyncData frame=%u ack=%u local=P%d mask=0x%02X sys=0x%02X",
            packet.currentFrame,
            packet.ackFrame,
            localSide + 1,
            static_cast<unsigned>(packet.actionData[tailIndex].playerAction),
            static_cast<unsigned>(packet.actionData[tailIndex].systemState));
    }
}

nav::FlowStats GetPacketFlowStats()
{
    return nav::GetStats();
}

lockstep::Stats GetLockstepStats()
{
    return lockstep::GetStats();
}

}
