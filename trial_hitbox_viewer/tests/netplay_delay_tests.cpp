#include "netplay_delay.h"
#include "menu_nav.h"
#include "trial_protocol.h"

#include <cstdio>

namespace
{

bool Expect(const bool condition, const char* const message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        return false;
    }
    return true;
}

}

int main()
{
    using namespace thv::netplay;
    bool ok = true;
    ok = Expect(PingHalfPeriodCeilFrames(0u, 60u) == 0u, "zero ping ceil") && ok;
    ok = Expect(PingHalfPeriodCeilFrames(40u, 60u) == 2u, "40 ms 60fps frontend ceil") && ok;
    ok = Expect(PingHalfPeriodCeilFrames(40u, 64u) == 2u, "40 ms 64fps frontend ceil") && ok;
    ok = Expect(PingHalfPeriodCeilFrames(32u, 60u) == 1u, "32 ms is one frame at 60") && ok;
    ok = Expect(PingHalfPeriodCeilFrames(32u, 64u) == 2u, "32 ms crosses two-frame ceil at 64") && ok;
    ok = Expect(PingHalfPeriodFloorFrames(0u, 60u) == 0u, "zero ping floor at 60") && ok;
    ok = Expect(PingHalfPeriodFloorFrames(0u, 64u) == 0u, "zero ping floor at 64") && ok;
    ok = Expect(PingHalfPeriodFloorFrames(32u, 60u) == 0u, "32 ms floor at 60") && ok;
    ok = Expect(PingHalfPeriodFloorFrames(32u, 64u) == 1u, "32 ms floor at 64") && ok;
    ok = Expect(PingHalfPeriodFloorFrames(100u, 60u) == 3u, "100 ms floor at 60") && ok;
    ok = Expect(PingHalfPeriodFloorFrames(100u, 64u) == 3u, "100 ms floor at 64") && ok;
    ok = Expect(
        ShouldRepeatLocalInputForRemoteAhead(-2, 0u, 60u),
        "zero ping repeats only when remote leads by two at 60") && ok;
    ok = Expect(
        !ShouldRepeatLocalInputForRemoteAhead(-1, 0u, 60u),
        "zero ping boundary does not repeat at 60") && ok;
    ok = Expect(
        ShouldRepeatLocalInputForRemoteAhead(-2, 0u, 64u),
        "zero ping repeats only when remote leads by two at 64") && ok;
    ok = Expect(
        !ShouldRepeatLocalInputForRemoteAhead(-1, 0u, 64u),
        "zero ping boundary does not repeat at 64") && ok;
    ok = Expect(
        ShouldRepeatLocalInputForRemoteAhead(-2, 32u, 60u),
        "32 ms 60fps threshold is minus one") && ok;
    ok = Expect(
        !ShouldRepeatLocalInputForRemoteAhead(-1, 32u, 60u),
        "32 ms 60fps threshold boundary") && ok;
    ok = Expect(
        ShouldRepeatLocalInputForRemoteAhead(-1, 32u, 64u),
        "32 ms 64fps threshold is zero") && ok;
    ok = Expect(
        !ShouldRepeatLocalInputForRemoteAhead(0, 32u, 64u),
        "32 ms 64fps threshold boundary") && ok;
    ok = Expect(
        ShouldRepeatLocalInputForRemoteAhead(1, 100u, 60u),
        "100 ms 60fps threshold is two") && ok;
    ok = Expect(
        !ShouldRepeatLocalInputForRemoteAhead(2, 100u, 60u),
        "100 ms 60fps threshold boundary") && ok;
    ok = Expect(
        ShouldRepeatLocalInputForRemoteAhead(1, 100u, 64u),
        "100 ms 64fps threshold is two") && ok;
    ok = Expect(
        !ShouldRepeatLocalInputForRemoteAhead(2, 100u, 64u),
        "100 ms 64fps threshold boundary") && ok;
    ok = Expect(MinimumLiveInputDelay(40u, 0, 60u) == 2, "fractional ping uses half-RTT ceiling") && ok;
    ok = Expect(MinimumLiveInputDelay(100u, 2, 60u) == 1, "60fps ping floor minus rollback") && ok;
    ok = Expect(MinimumLiveInputDelay(100u, 2, 64u) == 2, "64fps ping floor minus rollback") && ok;
    ok = Expect(MinimumLiveInputDelay(100u, 6, 64u) == 0, "rollback covers ping floor") && ok;
    ok = Expect(AdjustLiveInputDelay(5, 1, 100u, 2, 60u) == 6, "increase one") && ok;
    ok = Expect(AdjustLiveInputDelay(20, 1, 100u, 2, 64u) == 20, "increase clamps 20") && ok;
    ok = Expect(AdjustLiveInputDelay(5, -1, 100u, 2, 64u) == 4, "decrease one") && ok;
    ok = Expect(AdjustLiveInputDelay(1, -1, 100u, 2, 64u) == 2, "decrease restores latency floor") && ok;
    ok = Expect(AdjustLiveInputDelay(0, -1, 0u, 20, 60u) == 0, "decrease clamps zero") && ok;
    thv::proto::RollbackReadyPacket offer{};
    offer.maxRollback = 6u;
    offer.reserved[0] = 64u;
    offer.sessionId = 0x1122334455667788ull;
    ok = Expect(
        thv::proto::IsValidRollbackReadyContract(offer, 64u, false, offer.sessionId),
        "64fps rollback offer contract accepted") && ok;
    ok = Expect(
        !thv::proto::IsValidRollbackReadyContract(offer, 60u, false, offer.sessionId),
        "rollback offer target mismatch rejected") && ok;
    offer.flags |= thv::proto::kRollbackFlagAck;
    ok = Expect(
        thv::proto::IsValidRollbackReadyContract(offer, 64u, true, offer.sessionId),
        "64fps rollback acknowledgement accepted") && ok;
    offer.reserved[1] = 1u;
    ok = Expect(
        !thv::proto::IsValidRollbackReadyContract(offer, 64u, true, offer.sessionId),
        "rollback reserved bytes fail closed") && ok;
    thv::proto::ExchangeRandomSeedPacket rendezvous{};
    rendezvous.clientNonce = 0x11223344u;
    ok = Expect(
        thv::proto::IsClientRendezvousHello(rendezvous),
        "nonce-bound runtime rendezvous accepted") && ok;
    thv::proto::ExchangeRandomSeedPacket seedOffer = rendezvous;
    seedOffer.kind = thv::proto::SeedHandshakeKind::HostOffer;
    seedOffer.hostNonce = 0x55667788u;
    seedOffer.seed = 0xAABBCCDDu;
    ok = Expect(
        thv::proto::IsHostSeedOffer(seedOffer, rendezvous.clientNonce),
        "host offer is bound to the current client nonce") && ok;
    ok = Expect(
        !thv::proto::IsHostSeedOffer(seedOffer, rendezvous.clientNonce + 1u),
        "stale host offer cannot bind a new client session") && ok;
    thv::proto::ExchangeRandomSeedPacket seedAck = seedOffer;
    seedAck.kind = thv::proto::SeedHandshakeKind::ClientAck;
    ok = Expect(
        thv::proto::IsClientSeedAck(
            seedAck,
            rendezvous.clientNonce,
            seedOffer.hostNonce,
            seedOffer.seed),
        "exact nonce-bound seed ACK proves the runtime endpoint") && ok;
    ok = Expect(
        !thv::proto::IsClientSeedAck(
            seedAck,
            rendezvous.clientNonce,
            seedOffer.hostNonce + 1u,
            seedOffer.seed)
            && !thv::proto::IsClientSeedAck(
                seedAck,
                rendezvous.clientNonce,
                seedOffer.hostNonce,
                seedOffer.seed + 1u),
        "stale host nonce or seed cannot finalize a new session") && ok;
    thv::proto::ExchangeRandomSeedPacket hostConfirm = seedOffer;
    hostConfirm.kind = thv::proto::SeedHandshakeKind::HostConfirm;
    ok = Expect(
        thv::proto::IsHostSeedConfirm(
            hostConfirm,
            rendezvous.clientNonce,
            seedOffer.hostNonce,
            seedOffer.seed)
            && !thv::proto::IsHostSeedConfirm(
                hostConfirm,
                rendezvous.clientNonce + 1u,
                seedOffer.hostNonce,
                seedOffer.seed),
        "host confirmation proves acceptance of this exact seed session") && ok;
    thv::proto::ExchangeRandomSeedPacket clientConfirmAck = hostConfirm;
    clientConfirmAck.kind = thv::proto::SeedHandshakeKind::ClientConfirmAck;
    ok = Expect(
        thv::proto::IsClientSeedConfirmAck(
            clientConfirmAck,
            rendezvous.clientNonce,
            seedOffer.hostNonce,
            seedOffer.seed)
            && !thv::proto::IsClientSeedConfirmAck(
                clientConfirmAck,
                rendezvous.clientNonce,
                seedOffer.hostNonce,
                seedOffer.seed + 1u),
        "client confirmation ACK closes only the exact nonce-bound exchange") && ok;
    rendezvous.controlWord = thv::proto::kActionRollbackReady;
    ok = Expect(
        !thv::proto::IsClientRendezvousHello(rendezvous),
        "foreign control cannot claim unknown client endpoint") && ok;
    thv::proto::LaunchGoPacket launch{};
    launch.inputDelay = 3u;
    launch.targetFps = 60u;
    launch.sessionId = thv::proto::MakeRuntimeSessionId(
        0x11223344u,
        0x55667788u,
        0xAABBCCDDu);
    ok = Expect(
        thv::proto::IsValidLaunchGoContract(launch, 60u, launch.sessionId),
        "60fps launch contract accepted") && ok;
    ok = Expect(
        !thv::proto::IsValidLaunchGoContract(launch, 64u, launch.sessionId),
        "launch target mismatch rejected") && ok;
    ok = Expect(
        !thv::proto::IsValidLaunchGoContract(launch, 60u, launch.sessionId + 1u)
            && thv::proto::MakeRuntimeSessionId(0u, 0x55667788u, 0xAABBCCDDu) == 0u,
        "stale runtime session and incomplete nonce tuple fail closed") && ok;
    launch.launchAtMs = 2000u;
    ok = Expect(
        !thv::proto::IsValidLaunchGoContract(launch, 60u, launch.sessionId),
        "launcher-domain scheduled launch cannot cross into runtime") && ok;
    launch.launchAtMs = 0u;
    launch.reserved[0] = 1u;
    ok = Expect(
        !thv::proto::IsValidLaunchGoContract(launch, 60u, launch.sessionId),
        "launch reserved bytes fail closed") && ok;
    ok = Expect(
        thv::menu::FilterOnlineSystemStateForPolicy(0xFFu, false)
            == thv::menu::kSystemStateEsc,
        "default online policy keeps only escape") && ok;
    ok = Expect(
        thv::menu::FilterOnlineSystemStateForPolicy(0xFFu, true)
            == thv::menu::kOnlineKnownSystemMask,
        "mutual opt-in adds only F4/F5") && ok;
    ok = Expect(
        (thv::menu::kOnlineKnownSystemMask
            & (thv::menu::kSystemStateIncFsf | thv::menu::kSystemStateDecFsf
                | thv::menu::kSystemStateF6 | thv::menu::kSystemStateF7
                | thv::menu::kSystemStateF8)) == 0u,
        "delay and mod keys never enter v12 wire") && ok;
    if (ok)
    {
        std::puts("netplay delay tests passed");
        return 0;
    }
    return 1;
}
