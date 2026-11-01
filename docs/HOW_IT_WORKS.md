# How EFZ FF Netplay works

EFZ FF Netplay adds rollback netplay, spectating and practice tools to the Fast
Food Trial edition of *Eternal Fighter Zero*, a game with no online play of its
own. This is a tour of how the launcher, the injected DLL and the DirectDraw
wrapper do that, for curious players and modders. Game memory addresses are
collected in `trial_hitbox_viewer/src/core/trial_addresses.h`. For install and
everyday use, see the [README](../README.md).

## The short version

You run a launcher instead of the game. The launcher finds your opponent over
UDP, checks that both installations are from the same release, and agrees the
match settings. Both launchers then start the game at about the same moment,
load a DLL into it before it runs, and hand over the settings in a small file.

Inside the game, the DLL repeats a stricter handshake on its own socket, drives
both copies from the title screen to character select, and keeps menus in
lockstep: neither side advances until it has the other side's input. From the
first movable frame of a match, rollback takes over. The game runs ahead on a
guess of the remote input. When the guess turns out wrong, it restores a
snapshot, replays the frames unseen with the real input, and shows the
corrected result.

Both players keep a record of every confirmed input, so a spectator can replay
the match from character select on a separate protocol and check its state
against the players' hashes. Offline, the same DLL provides hitboxes, frame
stepping and savestates.

## Repository map

| Area | What lives there |
| --- | --- |
| `trial_caster/` | Builds `EFZ FF Netplay.exe`, the launcher: UI, connection handshake, game start and injection, tests |
| `trial_hitbox_viewer/` | Builds `netplay.dll`, which holds the whole netplay runtime as well as the hitbox overlay and practice tools. The folder name is narrower than its contents |
| `ddraw_wrapper/` | Builds `ddraw.dll`, the game's display layer, based on Aquat1c/EfzDdrawWrapper |
| `shared/` | Headers and code compiled into both the launcher and the DLL |
| `scripts/` | Packaging, the loopback test harness, Wine helpers |
| `tools/` | `tools/mdump_stack.py`, a small crash-dump stack reader |
| `lib/` | Bundled SDL3, Dear ImGui and MinHook |

Inside `trial_hitbox_viewer/src/`:

| Folder | Contents |
| --- | --- |
| `core/` | DLL entry point, game addresses, the player wire format |
| `caster/` | Session loading, peer transport, frontend lockstep, input, spectating |
| `rollback/` | Rollback engine, snapshots, desync detector |
| `hooks/` | Game hooks: screen ticks, presentation gate, pacing, DirectInput, hitbox drawing |
| `practice/` | Savestates, digests and the snapshot profiler |
| `util/` | Determinism, RNG, logging, crash handling, Wine helpers, test harness |

## Launcher session flow

The launcher's state lives in `CasterApp` (`trial_caster/src/caster_app.cpp`).
Each network job runs on a worker thread, and the UI only reads snapshots of the
result.

### Host, join, spectate

- **Host:** `CasterApp::StartHost()` starts `NetSession::HostHandshake()`
  (`trial_caster/src/net_session.cpp`), which binds your configured UDP port and
  waits. The host is always P1.
- **Join:** `CasterApp::StartConnect()` parses the address with
  `ParseConnectDestination()` (`trial_caster/src/util.h`), then
  `NetSession::ConnectHandshake()` opens a socket on an ephemeral port and sends
  a `ConnectionRequest` every 250 ms.
- **Spectate:** `CasterApp::LaunchSpectator()` runs
  `CasterApp::NetThreadSpectatorProbe()`, which asks the host about its state
  with `NetSession::ProbeSpectatorHost()`. The query reserves nothing. The
  answer is `kReplyHostWaiting`, `kReplyHostStarting` or `kReplyInPlay`
  (`trial_caster/src/protocol.h`). On "in play" the viewer launches at once.
  While the pair is still starting, or if you chose to wait, the launcher keeps
  polling. A host with no match yet offers a choice between waiting and joining
  as a player.

If you join a host that is already busy, you are not left to time out.
`NetSession::ServiceOccupiedHost()` answers a third player while the first pair
sits on the ready screen, and the injected game answers once the match runs
(`HandleLauncherPresenceQuery()` in
`trial_hitbox_viewer/src/caster/netplay_sync.cpp`). The launcher turns those
answers into a prompt to watch instead (`ConnectionPrompt` in
`trial_caster/src/caster_app.h`).

### Direct first, then AutoPunch

Both handshakes begin with plain UDP. If nothing has connected after
`kDirectAttemptMs` (1.5 seconds, `shared/autopunch_protocol.h`), the
`trialnet::autopunch::Client` attached to the same socket starts helping
(`shared/autopunch_client.cpp`):

1. Every 500 ms each side registers with the rendezvous coordinator at
   `delthas.fr`, which sees the public address and port of that socket.
2. The joiner also looks up the host by IP and advertised port, and sends the
   host's address a one-byte punch every 125 ms.
3. When the coordinator replies with a mapping, the receiver fires a short burst
   of punches at that endpoint, opening its own NAT in that direction.

The coordinator's address is pinned, and only endpoint information passes through
it. Game traffic always goes directly between the players. A host listens
passively: punch bytes can never choose its peer, only a coordinator mapping
can teach it an endpoint, and it stays registered so later joiners and spectators
can reach it the same way. The connection attempt gives up after
`kConnectTimeoutMs` (12 seconds) or your configured timeout, whichever is longer.

### Compatibility check

Compatibility is checked several times, because each check catches a different
mismatch:

- Before any network traffic, `ValidateLocalNetplayDll()`
  (`trial_caster/src/caster_app.cpp`) calls `ReadNetplayDllCompatibility()`
  (`trial_caster/src/util.h`). That reads the DLL's exported
  `TrialNetplayCompatibilityId` from the PE file without loading the DLL, so a
  launcher and DLL from different releases are refused early.
- `kCompatibilityId` (`shared/trial_compatibility.h`) is the single number that
  changes whenever the handshake, session file, wire format or simulation
  contract becomes incompatible. Product versions in `shared/trial_versions.h`
  move separately.
- The host requires an exact version match in `ConnectionRequest` and sends
  `VersionMismatchPacket` otherwise. Each side also compares network mode with
  `NetworkModesMatch()` (`trial_caster/src/protocol.h`).
- At launch, `CasterApp::PrepareAndLaunch()` checks that `EFZ.EXE`, `netplay.dll`,
  `SDL3.dll` and `ddraw.dll` are all present. `ValidateEfzExecutableLayout()`
  checks the game's PE layout rather than a whole-file hash, so data patches that
  keep the code layout still launch. `ValidateEfzDirectDrawWrapper()` checks the
  wrapper's exports without running it.
- The DLL checks again inside the game. `ValidateTrialHostImage()`
  (`trial_hitbox_viewer/src/util/win32_compat.h`) inspects the running image before
  any hook, because every hook targets one exact executable.
  `InitializeMod()` (`trial_hitbox_viewer/src/core/dllmain.cpp`) compares the
  session file's compatibility ID with its own and exits on a mismatch.

### Acknowledged config

After the host accepts a joiner, the two sides exchange nine ordered pings. The
launcher discards the slowest and averages the rest. The Auto delay button uses
this figure through `RecommendedInitialDelay()`
(`trial_caster/src/delay_recommendation.h`).

The host then sends `InitializationConfigurationPacket` every 250 ms until it
gets `InitializationAckPacket`. A lost burst therefore cannot leave the host
showing "ready" while the joiner still waits. The packet carries input delay,
game speed and capabilities. A joiner refuses a delay above 20 or a speed other
than 60 or 64. Practice shortcuts are enabled online only when both players opted
in (`PracticeKeysMutuallyEnabled()` in `trial_caster/src/protocol.h`).

### Synchronized launch

When the host presses start, `CasterApp::LaunchReady()` refreshes delay and speed
from the settings, since you can still edit them on the ready screen.
`CasterApp::StartLaunchAsync()` fixes the host's deadline *first*, then
`NetSession::SendLaunchGo()` sends ten copies of a `LaunchGoPacket` over about
200 ms. Taking the deadline after that burst would start the host about 200 ms
late.

The joiner's `NetSession::WaitForLaunchGo()` validates the packet with
`IsValidLaunchGoPacket()` and adopts its delay and speed. Both sides then wait out
the same `kLaunchSyncLeadMs` (2 seconds) with `WaitUntilLaunchAt()`. Delay and
speed are host-decided from this point on.

### Suspended launch and DLL injection

`GameLauncher::Launch()` (`trial_caster/src/game_launcher.cpp`) starts the game
so that nothing runs before the DLL is in place:

1. It creates `EFZ.EXE` with `CREATE_SUSPENDED`, passing the session file's path
   in `TRIAL_CASTER_SESSION_INI`.
2. `ResolveMainEntryPoint()` finds the image's entry point.
   `WaitForEntryPoint()` overwrites its first two bytes with a jump-to-self,
   lets the process run in small slices until the main thread is parked there,
   and restores the bytes while the thread is suspended. By then the loader has
   finished, so its module list is complete, including under Wine.
3. `InjectDll()` writes each DLL path into the process and runs `LoadLibraryW` on
   a remote thread, `SDL3.dll` first and then `netplay.dll`.
   `ResolveRemoteLoadLibrary()` finds where `LoadLibraryW` lives in the new process.
4. The launcher closes its UDP socket through the `beforeResume` callback. Because
   this happens after injection and before the game runs, the game can bind the
   same port without racing the launcher.
5. `ResumeThread` lets the game start with the DLL's early hooks already in place.

### session.ini

`WriteSessionFile()` (`trial_caster/src/session_file.cpp`) writes the handoff to
`session.ini` in a per-launch folder named launch_N under the launcher's session
directory:

| Section | Contents |
| --- | --- |
| `session` | Mode, host/side, peer address and ports, delay, rollback frames, `target_fps`, `allow_practice_keys`, measured ping, whether AutoPunch was used, compatibility ID, session ID |
| `controls_p1`, `controls_p2` | Input source and every key and pad binding |
| `shortcuts` | Every shortcut, keyed by its stable name |

`ready` is written last, then the file is flushed and read back. If any checked
field differs, the file is deleted and the launch is refused. The DLL's
`TryLoadSession()` (`trial_hitbox_viewer/src/caster/caster_session.cpp`) parses
required fields strictly. A launcher-managed game with a bad descriptor exits
rather than quietly running offline.

Host and joiner each write their own rollback preference to this file. Only the
host's value counts, because the DLL negotiates it, as described next.

## Runtime

`DllMain()` (`trial_hitbox_viewer/src/core/dllmain.cpp`) does little: it claims a
single instance, validates the host image, pins itself in memory, installs the
DirectInput focus guard (which has to be in place before `WinMain` runs) and
starts a thread. `ModThread()` initializes crash handling and Winsock, then
`WaitForDDrawReady()` holds until the game has finished its DirectDraw setup.
Only then does `InitializeMod()` install hooks and load the session.

Two of those steps carry rules worth keeping. `ClassifyCasterImage()` claims one
instance per process because separately loaded images have separate statics, and
two mod threads racing `SDL_Init` or MinHook crash. `WaitForDDrawReady()` doesn't
trust the primary surface pointer, which goes non-null early in the game's
DirectDraw setup. It waits for a lookup table that is filled last, because hooks
installed during that setup make `WinMain` exit.

A session file left behind doesn't bind a game you start by hand. Without
`TRIAL_CASTER_SESSION_INI` in the environment, an offline load that can't take over
pacing keeps the stock path instead of closing.

`ApplyToCasterBridge()` (`trial_hitbox_viewer/src/caster/caster_session.cpp`) sets
up pacing and hands players to `BeginSession()`
(`trial_hitbox_viewer/src/caster/netplay_bootstrap.cpp`). From here, the player
wire format in `trial_hitbox_viewer/src/core/trial_protocol.h` applies.

### Seed/nonce handshake and runtime session ID

The launcher's socket is gone, and a quick relaunch may reuse its port, so the
game must prove it is talking to the current run of the other game. Each side
generates a nonce with `GenerateHandshakeNonce()`, and the host picks the RNG seed
with `GenerateHostSeed()`. They then trade `ExchangeRandomSeedPacket` messages
(`BeginSeedExchange()`, `HandleSeedPacket()` and `PollSeedExchange()` in
`trial_hitbox_viewer/src/caster/netplay_sync.cpp`):

1. `ClientHello`: the joiner repeats its nonce every 200 ms. This is also how the
   host learns the game socket's real endpoint.
2. `HostOffer`: the host echoes that nonce and adds its own nonce and the seed.
3. `ClientAck`: the joiner adopts the seed and echoes all three values.
4. `HostConfirm`: the host proves it accepted that exact acknowledgement.
5. `ClientConfirmAck`: the joiner closes the exchange.

`IsDeterminismSynced()` becomes true only once the seed is adopted, the final proof
has arrived, and a session ID exists.

`MakeRuntimeSessionId()` (`trial_hitbox_viewer/src/core/trial_protocol.h`) mixes
both nonces and the seed into a 64-bit ID, and every later player packet carries
it. `IsCurrentRuntimeDatagram()` drops anything without the current ID, so a
delayed packet from an earlier run cannot be mistaken for this one, even from the
same address and port.

AutoPunch can also run here, because the game's socket is new to any NAT.
`ServiceAutopunch()` follows the same direct-first timing, then stops once the
handshake is confirmed, keeping only the registration that spectators need.

Only the host binds the well-known port. The client takes an ephemeral one, which
also lets two instances share a machine. `Poll()` is strict about what may change
the learned endpoint, and these rules are easy to break when adding a packet:

- Coordinator, punch, launcher-query and spectator traffic are consumed before any
  endpoint is learned, so a viewer's `Hello` can't become gameplay input or take over
  the client's port.
- The host learns the client's port only from a nonce-bearing hello or the current
  seed proof. Delayed launcher acknowledgements also reach this socket, and they
  teach it nothing.
- A coordinator mapping for the peer's IP can move the endpoint only until the
  handshake completes.
- Before the proof, only the seed protocol and a quit carrying the current session
  ID are accepted, so a stray datagram on a reused port can't change state or abort
  the process.

### Ready

The host then offers its rollback contract in `RollbackReadyPacket`
(`PollRollbackNegotiation()`): maximum rollback, queue size, and game speed. The
joiner checks this against its own session with `IsValidRollbackReadyContract()`,
adopts the host's rollback value and acknowledges it
(`HandleRollbackReadyPacket()`). Both sides abort on a mismatch. The agreed value
also becomes part of the spectator archive through
`CommitNegotiatedRollbackFrames()`.

### LaunchGo

`NetplayGatesOpen()` (`trial_hitbox_viewer/src/caster/netplay_bootstrap.cpp`) keeps
the game at the menu until the handshake and contract are both done. When the host
reaches a stable title or main menu screen, it starts sending a runtime
`LaunchGoPacket` (`SendLaunchGo()`) and retransmits it until the session is active.
`HandleLaunchGoPacket()` applies the delay once and aborts if the speed differs
from the session.

The launcher and the game both use action 7 for this packet, so the two are kept
apart on purpose. The launcher's version has a non-zero lead time and is 9 bytes.
The game's has zero lead time, a session ID, and is 17 bytes. A late launcher
packet is ignored.

Next `KickNativeVsHumanTransition()` sets the game's own screen state so that the
game thread runs its normal VS Human transition into character select. No
DirectDraw or UI setup is called from the worker thread.

### Lockstep frontend

Character select has no rollback. Its gate is `ShouldStallLockstepFrame()`
(`trial_hitbox_viewer/src/caster/netplay_frame_gate.cpp`), driven from the screen
hooks in `trial_hitbox_viewer/src/hooks/framestep.cpp`. It calls
`WaitRemoteIterations()` (`trial_hitbox_viewer/src/caster/netplay_lockstep.cpp`),
which allows a capture only when the remote history covers the frame the local
side needs. While it stalls, the hook pumps window messages and waits out the
frame slot, so the window stays responsive.

The delay here comes from measured ping alone: half the round trip in frames,
rounded up (`PingHalfPeriodCeilFrames()` in
`trial_hitbox_viewer/src/caster/netplay_delay.h`). Your configured input delay
applies only in battle.

Polling and capture belong to the hooked screen tick on the game thread. At character
select, `SampleSyncWireMaskForCapture()` strips B from the captured mask, so backing
out never travels to the other side. Online play always samples the P1 binding bank
(`LocalBindingPlayerIndex()`), including for a client on side P2. Don't compare that
index to the gameplay side.

`Poll()` can run on both the worker and game threads, so `g_historyMutex` guards every
access to the histories: concurrent vector growth corrupts the heap. In the same way,
`g_socketMutex` covers socket use and `g_transportOwnerMutex` covers the shared
subscriber and grant lists. `NotifySimAdvanced()` stamps each frame's screen and
generation when the frame is consumed. A stamp taken at publication would give
backlogged frames the next screen's value and send a spectator into the wrong handler,
so `ServiceConfirmedSpectatorFramesLocked()` publishes only up to the frontier that
both histories and the stamps have reached.

All input travels in `SyncDataPacket`. Each packet carries a trailing window of
`kMaxQueueSlot` (64) input records, so a lost packet is repaired by the next one.
Its `ackFrame` says how much of the other side's stream has arrived, and a sender
never runs more than one window past that acknowledgement, so it can't outrun
what the wire can still recover. The `timeline` field changes at lifecycle
boundaries, which keeps delayed packets from an earlier screen out of the new one.

`CheckPeerLiveness()` watches for a dead peer. Until the first SyncData the deadline
is absolute, 30 to 120 seconds scaled by ping. After that, the window is 8 to 60
seconds, and duplicate packets do not extend it while real progress is due. A
caught-up menu may idle far longer than that, so a fresh deadline starts when progress
becomes due again. Spectator adverts are consumed before the liveness clocks and can't
keep a wedged session alive, and subscriber fan-out runs last in the poll so a full
spectator budget can't delay a player's sends. A
session that fails ends through `AbortNetplay()`, and `OnModTick()` closes the game
instead of continuing out of sync.

### Battle-entry barrier

Inside the game's loading screen, the first intro frame already runs the full
player simulation, and input is consumed on every intro call
(`kBattleIntroFirstFrameCounter`). `kBattleIntroRawScreenFlipCounter` only flips the raw
screen value to battle, far later, so don't treat that flip as the start of gameplay.
`HookedLoadingTick()` therefore holds before that frame can run, and
`HookedBattleTick()` withholds the native tick until the handoff engages. Intro frames
that ran before the seed would race per peer and change meter, animation and hitstop
state. Meanwhile `StartNetplaySession()` (`trial_hitbox_viewer/src/rollback/rollback.cpp`)
runs a barrier:

1. `AlignForBattleHandoff()` and `EvaluateHandoff()` decide whether local histories
   are ready, and report a `HandoffBlockReason` if not.
2. Each side broadcasts its frame in `BattleEntryPacket` through `SendBattleEntry()`.
   The screen is polled asynchronously, so the two frames can differ by one. If the
   peer is ready at an earlier frame, `TrimForBattleEntry()` moves down to it.
3. Only when both are ready at the *same* frame does `CommitBattleEntry()` advance
   the timeline, retransmitting until matching SyncData proves the peer crossed too.
4. `Engine::ImportWireHistories()` takes over the lockstep histories, and rollback
   is enabled.

The barrier may wait or abort after a timeout (10 to 60 seconds, scaled by ping),
but it never proceeds from unequal frames. That would guarantee a desync.

From then on, one engine owns every screen for the rest of the session
(`ShouldDriveFrontendTick()`). Later matches do not repeat the barrier, because the
timeline is already continuous. `Engine::HandleMemorialTransition()` treats the
first intro frame of each new match as the boundary, and rounds within a match keep
the same timeline with no reset. The stage is also derived from the shared seed
(`PrepareStageForNativeLoad()`).

### 60/64 FPS pacing and the wrapper handoff

The host's speed is either 60 or 64 displayed frames per second, and the game runs
three simulation substeps per displayed battle frame, so 180 or 192 substeps. The
speed is fixed for the session and is rechecked at each stage. A wrong frame rate
would be a visibly different game, so nothing falls back to "close enough".

Exactly one clock may own the frame rate. `ConfigureSessionTargetFps()`
(`trial_hitbox_viewer/src/hooks/game_pacing.cpp`) finds `EFZDDRAW_SetFrameLimit` in
`ddraw.dll` and calls it with 0, which turns the wrapper's limiter off
(`ddraw_wrapper/ddraw.c`). If that fails, a launcher-managed session closes rather
than stacking two limiters. `ResetSessionTargetFps()` hands the limit back at the
end.

`PaceVisibleFrame()` runs once at the top of every hooked screen tick. It waits for
an absolute `QueryPerformanceCounter` deadline, sleeping for the coarse part and
yielding or spinning for the end (`WaitForQpcDeadline()`). `AdvanceDeadline()`
carries the division's remainder, so rounding never accumulates. After a long stall,
such as focus loss, the deadline resets instead of bursting frames to catch up.
`BypassNativeLimiterForOwnedFrame()` then tells the game's own limiter not to wait
a second time.

Hidden replay work doesn't count as presented frames.
`ScopedNativeFpsAccountingIsolation` protects the game's FPS counter around hidden
ticks. Passing it `false` makes it a no-op, so replay loops can scope every tick without
branching. The game counts substeps and displays a once-a-second latch divided by 3, and
its loading handler clamps that latch to 180, so `ServiceOwnedNativeFpsHud()` owns the
latch while mod pacing is active and keeps the on-screen figure honest at 64 FPS. It
normalizes by elapsed `QueryPerformanceCounter` time, because one sample window can
legitimately hold an extra frame.

## Rollback basics

The engine is `Engine` (`trial_hitbox_viewer/src/rollback/rollback_engine.cpp`), with
frame data in `Session` (`trial_hitbox_viewer/src/rollback/rollback_session.h`). Its
loop is `Engine::MainTick()`: service any pending correction, work out how many
frames to run, run them, then publish whatever became confirmed.

### Input delay

The effective delay in battle is 1 plus your configured delay, and local input is
captured that far ahead of the frame it applies to. The extra 1 is mandatory: without
it, `Engine::WaitRemote()` can't advance the confirmed floor and the session wedges. The
host's value applies to both players at match start.

After that, Keypad + and Keypad - change your own delay only
(`Engine::ServiceLiveDelayHotkeys()`). These keys never reach the wire. Lowering is
limited by `MinimumLiveInputDelay()`: half the round trip in frames, minus the
rollback allowance.

### Prediction

`Engine::AdjustPrediction()` decides each tick's work. Frames past the confirmed
remote input use the last known input again (`Session::RemoteInputAtOrHeld()`), up
to the negotiated rollback window: 6 frames by default, at most
`kMaxPredictionFrames` (20) (`trial_hitbox_viewer/src/rollback/rollback_config.h`).
`Engine::SetPredictionWindow()` also keeps delay plus window inside the SyncData
retransmit window. A window of 0 is plain delayed lockstep.

If the opponent is running ahead of you, `ShouldRepeatLocalInputForRemoteAhead()`
detects it, and `Engine::AppendRepeatedLocalInput()` queues your previous input once
more so you catch up, without sampling the controller twice in one tick.

While prediction is live, `Engine::IsInputAuthorityActive()` asks only for local
coverage of the frame. Requiring remote input there would zero both players' masks on
every predicted frame. A future input may land on a different native handler, so
`Engine::StampCurrentFrameProvenance()` records screen and generation at the pre-frame
boundary rather than when the input is captured.

Some things can't be undone, so they are never predicted:

- **Screen changes.** If a predicted frame would change the game's screen handler,
  such as a KO starting the round-end sequence, `Engine::BatchRender()` rewinds to
  that frame's snapshot and holds there until the real remote input exists.
- **ESC.** A rising ESC waits until the peer has acknowledged it and the remote input
  for that frame has arrived. Its fade then runs in confirmed lockstep through
  `Engine::WaitRemote()`, because the fade's end releases native input and sound
  objects.

### Snapshot ring and saved state

Before each match frame runs, `Engine::SaveInputs()` fills a `RollbackFrameSlot`
(`trial_hitbox_viewer/src/rollback/rollback_types.h`) in a ring of `kFrameRingSize`
(60) slots:

- **Battle memory** from `CaptureBattle()`
  (`trial_hitbox_viewer/src/rollback/rollback_memory.cpp`). This copies only the
  listed regions in `kDynamicBattleRegions`, about 32 KB, out of a battle area of
  over 8 MB. The rest is per-character data loaded at character select that doesn't
  change during a match. `trial_hitbox_viewer/src/practice/savestate_profiler.cpp`
  measures which memory changes. The screen dispatch block
  (`kVaScreenDispatchBegin` onward) is deliberately left out and must never be restored
  blindly, because a stale pacing field can trap the battle tick in its wait.
- **Auxiliary match state** from `CaptureShakeAux()`: screen shake, charge edge
  flags, and the current screen and round fields. They live outside those regions and
  are easy to underestimate. A replay without the shake state runs against a zeroed
  source, and a stale `kVaChargeEdgeFlags` starts a back-charge timer on one peer only.
- **Effects** from `CaptureVisualEffects()`, cosmetic state that is rewound locally
  but never compared between players.
- **The FPU image, both RNG states, inputs, and the frame's screen and generation.**

### Correction replay with presentation hidden

`Engine::PushRemoteInput()` compares each arriving input with what the saved slot
actually used. A match costs nothing. A mismatch records the earliest wrong frame and
leaves the live state alone until the next tick.

`Engine::ServicePendingRollback()` then restores that frame with
`Engine::ReloadSavedState()` and marks the later slots stale, since their ancestry
just changed. `Engine::BatchRender()` runs forward again, up to 32 frames per tick,
until it regains the frame you were on. A stall with no new work doesn't rewind at
all, because the speculative state stays in place.

### Render gate

Only the last frame of a batch is seen. `Engine::BatchRender()` sets
`SetPresentSuppressed()` for every other frame, and the hooks in
`trial_hitbox_viewer/src/hooks/render_gate.cpp` react:

- `HookedPresent()` skips the flip to the screen.
- `HookedBattleSceneComposite()` and `HookedSurfaceBlit()` skip drawing work.
- `HookedPlaySound()` skips sound effects. On visible frames it also ignores a repeat
  of the same sound for the same absolute frame (`SetRollbackAudioFrame()`), so
  replays don't re-trigger hits.

`Engine::BatchRender()` also skips the game's frame wait on hidden ticks, and restores
the wait's starting time before the visible one, so replay CPU time comes out of the
frame's time budget instead of adding to it.

If the sound, effect-RNG or composite hook can't be installed, `InitializeRenderGate()`
fails and the mod refuses to run rollback unsafely.

### Asset and allocation guards

The same file guards the game's loaders, which chain `CreateFileA`, GetFileSize and
`operator new` without checking results, so a failed open asks for an impossible
allocation.

- `HookedAssetFileLoad()` returns success for a file that is truly absent. Any other
  failure, such as access denied, still reaches the loader.
- `HookedCreateFileA()` upgrades exclusive read-only opens to `FILE_SHARE_READ`, because
  two instances in one folder would otherwise collide during synchronized loading.
  Remaining sharing violations are retried briefly.
- A missing read-only `OPEN_ALWAYS` file becomes a blank dummy from
  `OpenDummyAssetFile()`. `OPEN_EXISTING` files carry gameplay data and are never
  substituted, since zeros there would desync.
- `HookedOperatorNew()` writes a minidump for any request of 256 MB or more. A
  launcher-managed online session then closes, and offline keeps the native behaviour.

### Determinism

Rollback only works if both machines compute identical results from identical
inputs (`trial_hitbox_viewer/src/util/determinism.cpp`,
`trial_hitbox_viewer/src/util/rng.cpp`).

**FPU.** `det::RepinFpu()` forces the x87 control word to 53-bit precision and
round-to-nearest. `det::ResetFpuForSimulation()` clears leftover x87 stack and status
before each snapshot, so background work can't leak into a frame, and each match starts
from an empty x87 state.

**RNG.** The game's `rand` and `srand` are replaced by `RandShim()` and `SrandShim()`
through the import table, with a code scan as the alternative
(`ScanForStaticCrtRand()`). They keep the generator's state in a shadow that can be
saved and restored. Three rules keep the stream equal on both sides:

- The seed from the handshake applies on both sides through `ApplyLocalDeterminism()`.
- Menus consume `rand` at their own pace, so `Engine::ReseedMatchRng()` reseeds at the
  start of each match from the shared seed and the transport generation.
- Effect animation calls `rand` while *drawing*, and hidden frames draw differently.
  `HookedEffectAnimUpdate()` wraps that work in `PushVisualRand()` and
  `PopVisualRand()`, which serve a separate cosmetic stream, so the number of draws
  can't affect the simulation. That stream is depth-counted per thread and rewound with
  each snapshot, but never compared between players.

### Desync checks

`Engine::SaveInputs()` computes seven CRC32 hashes per frame
(`kFrameHashRegions`): one for each of the five battle regions, one for the FPU control
word plus the simulation RNG, and one for the auxiliary state. Cosmetic effects are
left out. The full FPU image is left out too, since it holds volatile details that
aren't game state.

Three independent checks use them (`trial_hitbox_viewer/src/rollback/desync_check.h`):

- **Per-frame comparison.** `Engine::ServiceConfirmedFrames()` publishes hashes only for
  confirmed, fully corrected frames and sends them as `FrameHashPacket`. Hashes from
  predicted frames are never compared.
- **A running chain.** A cumulative hash of all confirmed frames rides in every
  `SyncDataPacket`, so a lost `FrameHashPacket` can't hide a divergence for good. It
  locates a persistent divergence, not the region it began in.
- **A replay self-check.** If a frame is saved again with the same inputs and its hash
  changed, replay wasn't deterministic. This catches an incomplete snapshot on one
  machine, without any network. Corrected frames are exempt up to and including the
  replay target: that frame's inputs may be unchanged while its ancestry is not.

Any of these ends the session through `Engine::MainTick()`, because a game that looks
fine but has drifted apart is worse than a closed one. Before closing,
`Engine::ServiceDesyncDump()` can write a capped raw dump of the differing regions.

## Spectating

A spectator is a read-only consumer of confirmed input, and it has no influence on
players' acknowledgements, prediction or quit handling.

### A separate protocol

The viewer protocol has its own magic, `EFZS`, in
`trial_hitbox_viewer/src/caster/spectator_protocol.h`. It shares the player's UDP port,
but `kSimulationContractId` names the replay rules that must match separately. Bump it
whenever the replay contract changes, such as the input record, the hashed state, or
when the RNG reseeds. The viewer's own socket gets AutoPunch help the same way a joiner's
does (`trial_hitbox_viewer/src/caster/spectator_client.cpp`), and only a coordinator
mapping can change its source port, since a one-byte punch proves an address but not a
port.

A viewer attaches like this:

1. It sends `Hello`. If nothing is ready, the source answers `Waiting`.
2. The source sends `GameStart`, the full starting contract for one game: seed, the
   exact RNG states at character select, FPU and SSE control values, speed, characters,
   palettes, stage and names.
3. The viewer echoes a fingerprint in `InitAck` (`GameStartFingerprint()`). Only then
   are inputs released.
4. The source sends `ConfirmedInputBatch` messages of up to `kBatchFrames` (20) records,
   and the viewer answers with a cumulative `Ack`. Duplicates and reordering are harmless.

A lost `GameStart` or `InitAck` just produces another `Hello`, which names the first
record the viewer still needs instead of restarting from zero, so verified records are
kept.

### Who serves viewers

Each source serves up to `kMaxSpectators` (16) viewers. Once the host has
`kSpectatorRelayDirectLimit` (8) viewers, `MaybeQueueSpectatorRedirect()` can send a new
viewer to the other player, who keeps the same record. The two players agree on this
over their authenticated channel (`SpectatorGrantPacket`, `SpectatorGrantAckPacket`,
`SpectatorClaimPacket`), using a short-lived one-time token. Only the host accepts a
viewer with no token. Redirects happen once and only before `GameStart`. If one fails,
the viewer returns to the host and is served directly, naming the refused grant in
`failedGrantId`. A viewer keeps its origin fixed for that single hop, so a failed target
can't choose where the fallback goes.

### Archives

`BeginHostArchive()` (`trial_hitbox_viewer/src/caster/spectator_relay.cpp`) runs on both
players. `BeginGame()` starts a new game at each character select, so a viewer can
reproduce the character and stage choice. `PublishConfirmedFrame()` then appends one
record per confirmed frame (`ConfirmedInput` on the wire): the P1 and P2 input words
together, the screen, and the state hashes when the frame has them. Because the words
are stored as P1 and P2 rather than local and remote, both players keep the same stream.

Every game is kept for the whole connected set, at about 48 bytes per frame. A late
viewer therefore starts from the beginning of the earliest kept game rather than from a
mid-match snapshot. The repository also has a checkpoint format
(`trial_hitbox_viewer/src/caster/spectator_checkpoint.h`) with its own tests, which the
normal attach path does not use.

### Playback

`DriveScreenTick()` (`trial_hitbox_viewer/src/caster/spectator_player.cpp`) owns the
viewer's screen hooks. Viewers ignore their local settings for determinism and speed:

- `TryInitializeSessionBeforeTransition()` applies the seed, control words and speed from
  the first `GameStart`, *before* the game enters character select, so that menu RNG use
  can't poison the stream.
- `TryStartFrontGame()` waits for a startup buffer of 30 frames, then restores the RNG
  states, characters, palettes and stage.
- For each record, `SetSpectatorInputWords()` supplies both players' inputs and the
  game's own screen tick runs. `MaintainNetplayCpuFlags()` forces the human path before
  any snapshot, because the players' hashes assume it.
- The match reseed happens at the first hashed generation, not at battle start.
- A shutdown during the native tick commits nothing, leaving cleanup to `DriveScope`.
  `g_driveMutex` is held for the whole owned tick.

There is no prediction or rollback, since the input is already final. A viewer that falls
behind runs up to 8 frames per displayed frame (`CatchUpBatchSize()`), with all but the
last hidden, using the same render gate as rollback. Running out of input just waits.

### Hash verification

Trust is checked at two levels:

- **State.** `VerifyConfirmedMatchState()` recomputes the same seven hashes before each
  hashed frame and compares them to the source's. A mismatch ends playback. It checks
  before applying any F4/F5 action on that frame, which matches the point where players
  hashed it.
- **Stream.** `AdvanceInputDigest()` chains every record into a rolling digest that
  carries across games, and the client checks it on the way in. When the session ends,
  `NotifySpectatorsSourceEnded()` keeps the source alive while viewers drain the rest,
  and the `Terminal` message states the final position and digest. Playback reports
  complete only when the viewer's own position and digest match it. Otherwise it reports
  the stream as incomplete.

A screen mismatch that lasts longer than 250 ms also ends playback rather than showing a
wrong game.

### Closing the window

The window procedure in `trial_hitbox_viewer/src/caster/window_telemetry.cpp` never drains
the archive itself, since that would race the running game thread. It sends only the
player's terminal message and defers `WM_CLOSE` until the worker reposts it at a stable
boundary. A system shutdown can't wait for a long drain and skips that. `WM_QUIT` isn't
treated as completion, because the window procedure may post it on its own.

## Hitbox viewer

### Capture method

There is no capture file and no box format. The viewer reads the game's own frame data
live, at the moment the game would draw its collision boxes.

`InstallHooks()` (`trial_hitbox_viewer/src/core/dllmain.cpp`) hooks the game's collision
draw routine (`kVaDrawCollisionBoxes`) with `HookedDrawCollisionBoxes()`. The game
passes the player, camera X and camera Y. The hook returns immediately when no layer is
on. Otherwise it calls `DrawPlayerCollisionBoxes()`
(`trial_hitbox_viewer/src/hooks/collision_display.cpp`). The game's original routine
is never called, so the mod fully replaces this drawing.

Enabling any layer also sets the game's debug-draw flag through `SetDebugDrawFlag()`.

### The frame record

Each player's memory holds their character's frame table, loaded at character select.
`CopyFrameBlob()` reads the player's current move ID (`kOffMoveId`) and subframe
(`kOffSubframe`), computes that frame's location from `kFrameBlobStride` and
`kMoveBlobStride`, and copies one 200-byte record. The viewer uses the first part of it:

| Byte offset | Constant | Contents |
| --- | --- | --- |
| 0 | `kHurtRectBase` | 5 hurt rectangles (`kHurtRectCount`) |
| 80 | `kHitRectBase` | 4 hit rectangles (`kHitRectCount`) |
| 144 | `kCollisionRectBase` | 1 collision rectangle (`kCollisionRectCount`) |

Each rectangle is four integers in the game's local coordinates.

Projectiles work the same way with their own IDs. `DrawProjectileRing()` walks the
player's `kProjectileRingSize` (60) slots from the ring head, skips slots that aren't
alive, and `CopyProjectileFrameBlob()` fetches each live slot's record.

### Layers

The layer mask has three bits (`trial_hitbox_viewer/src/hooks/collision_display.h`):

| Layer | Bit | Default shortcut | Default palette index |
| --- | --- | --- | --- |
| Hit | `kLayerHit` | Page Up | 221, red |
| Hurt | `kLayerHurt` | Page Down | 219, yellow |
| Collision | `kLayerCollision` | Home | 237, green |

`DrawCharacterLayer()` draws hurt, then collision, then hit, so attack boxes are on top.
`DrawProjectileLayer()` draws collision and hit rectangles the same way. For hurt, it
draws only slots below 5, using the record's first hurt rectangle.

### Rendering through the palette draw path

The boxes are drawn by the game itself. For every rectangle:

1. The game's own transform (`kVaTransformEntity` or `kVaTransformProjectile`) converts
   the local rectangle for that player, or for that player's projectile slot.
2. The code adds the camera offset, with a fixed 210 added to the vertical position.
3. The game's palette rectangle drawer (`kVaDrawPaletteRect`) draws it using a palette
   index.

There is no separate renderer. The palette itself is never modified, only indexed.
The defaults come from `kPaletteHurt`, `kPaletteHit` and `kPaletteCollision`.

`DrawPaletteDebugPanel()` can change those indices, but the in-game debug window it
belongs to ships disabled (`Initialize()` in
`trial_hitbox_viewer/src/caster/caster_ui.cpp`).

### Toggles

`PollHotkeys()` (`trial_hitbox_viewer/src/hooks/collision_display.cpp`) flips one bit per
shortcut through `ToggleLayer()`, ignoring any shortcut that collides with a fight
binding (`ShortcutConflictsWithAnyFightBinding()`). `ModThread()` doesn't poll it online,
and `PollHotkeys()` refuses online or while spectating as well, so the overlay is
offline only. `SanitizeBattleStateAfterRestore()` also reasserts the debug-draw flag after
a savestate load, so your layers stay on.

### Practice savestates

`trial_hitbox_viewer/src/practice/savestate.cpp` saves and restores battle state to disk.
The same offline rule applies, and saving and loading only work on the battle screen.

**What a save holds.** `kRegionsV4` (`trial_hitbox_viewer/src/practice/savestate_layout.h`)
lists the regions:

| Region | Contents |
| --- | --- |
| `battle_sim` | Camera and both complete player structures, including frame tables |
| `match_aux` | Combo, camera and hitstop globals between the players and the screen dispatch block |
| `win_count` | Round wins |
| `frame_substeps` | Simulation substeps per frame |

Savestates are far larger than rollback snapshots. They are made for a person pressing a
key, not for every frame, and also hold the frame tables.

**On-disk format.** `SaveSlotLocked()` writes these in order:

| Part | Type | Contents |
| --- | --- | --- |
| Header | `SavestateHeaderV2` | `TESS` magic, version (4), payload size, both character IDs, a both-human flag in `reserved`, the human side, slot, simulation RNG state, FPU image size |
| Metadata | `SavestateHeaderV3Fields` | Payload size, region count, payload CRC32, each player's move ID and subframe |
| Region table | `RegionEntry` | Address, size, payload offset and name for each region |
| FPU image | | 108 bytes |
| Payload | | The region bytes |

**Loading.** `LoadSlotLocked()` is deliberately suspicious. It checks the magic and
version, verifies the CRC32, requires both character IDs to match the live match, and
checks the payload size. `ApplyPendingRestore()` then:

- restores the RNG, but only re-pins the FPU, since the saved FPU image is diagnostic;
- applies regions through `ApplySnapshot()`, writing `battle_sim`, `win_count` and
  `frame_substeps`. `match_aux` is saved but deliberately skipped on load, and the live
  scene and effect pool are kept;
- clears the transient event queue (`ClearTransientBattleScratch()`);
- runs `SanitizeBattleStateAfterRestore()`, which forces the screen to battle, clears
  round-end states, and resets pacing fields, since a stale timestamp could trap the
  battle tick in its wait;
- restores who controls which side.

Older file versions (1 to 3) still load, with the same selective rule.

**Digests.** `SavestateDigest` (`trial_hitbox_viewer/src/practice/savestate_digest.h`) is
a readable summary of each fighter and the match: move, HP, meter, position, combo,
screen. After a save, `CaptureLiveDigest()` feeds the log and `WriteDigestArtifact()`
writes a text file to the session log folder, alongside `WriteFullSavestateDump()`. These
are for diagnosis and don't affect behaviour. Integrity is protected by the payload
CRC32. The normal load path does not write digests.

**Slots.** `GetStatePath()` stores files in `trial_savestates/` under
`MutableStateRoot()`:

- `practice/` holds eight numbered slots (`kPracticeMinSlot` to `kPracticeMaxSlot`),
  cycled with [ and ] and used with G and B. The active slot wraps around
  (`CyclePracticeSlot()`).
- `debug.thvs` is the quick slot, used with F6 and F7.
- `netplay/` is a defined bank that no shortcut reaches.

`ResolveLoadPath()` also looks in a folder beside `EFZ.EXE`, so saves there are found
even if the library is read-only. The hotkeys run on the mod's worker thread, and a load
applies immediately.

### Frame step

`trial_hitbox_viewer/src/hooks/framestep.cpp` hooks all five screen ticks, for netplay's
benefit as much as practice's. Frame stepping uses only the battle tick:

- Space flips `g_paused`. P sets the pause if needed and requests one step
  (`PollFramestepHotkeys()`).
- While paused, `HookedBattleTick()` returns before running the game's tick, unless
  `ConsumeFramestepRequest()` finds a request, which it clears. One press is therefore
  exactly one battle tick.
- The hook still pumps window messages and paces itself, so the window stays alive and the
  CPU isn't spinning.

As with the other practice tools, the shortcuts aren't polled online or while spectating.

## Input and controls

### From device to game

SDL3 samples keyboards and controllers
(`trial_hitbox_viewer/src/caster/sdl_input.cpp`). The launcher's Controls page saves
bindings, and `ValidateControlBindings()` (`trial_caster/src/control_bindings.h`) rejects
ambiguous ones. A keyboard key can do only one job across both players and all shortcuts.
The launcher then passes them through `session.ini` to `LoadFromSessionIni()`
(`trial_hitbox_viewer/src/caster/control_config.h`).

Each player uses exactly one source, keyboard or controller, never a blend
(`trialnet::input::Source` in `shared/input_binding_tokens.h`). The sampler in
`trial_hitbox_viewer/src/caster/sdl_input.cpp` follows a few rules:

- Never parse SDL key names from a global constructor, because that runs under the loader
  lock. `trial_hitbox_viewer/src/caster/control_config.cpp` uses plain default construction
  for the same reason.
- A gamepad that fails to open falls back to raw joystick access only for an explicit
  `JOY_*` binding.
- A `__try` around gamepad setup reduces a flaky HID driver to keyboard-only.
- The keyboard is read through `GetAsyncKeyState`, not SDL's keyboard state, since the game
  owns foreground focus. SDL pumps at most once per millisecond and never drains events that
  other in-process mods may want.

Input reaches the game through two hooks:

- **Fight input.** `HookedPollPlayerInput()` (`trial_hitbox_viewer/src/hooks/control_swap.cpp`)
  writes the current mask into the player's input fields with `ApplyWireMaskToPlayer()`.
  The mask is one byte, packing Up, Down, Left, Right, A, B, C and D
  (`trial_hitbox_viewer/src/caster/trial_input_codec.h`).
- **Keyboard state.** `HookedGetDeviceState()` (`trial_hitbox_viewer/src/hooks/dinput_relay.cpp`)
  edits the 256-byte DirectInput buffer the game reads. Online, `TryOverlayNetplayBindings()`
  clears it and rebuilds only what the session allows, so physical keys can't bypass the
  synchronized state. On an unrecognized screen it neutralizes the buffer rather than
  passing it through.

Offline, `OverlayOfflineManagedBindings()` replaces each player's slot bytes with their
selected source. For controllers, `BuildOfflineDiBindingPlan()` (`shared/offline_input_routing.h`)
assigns private unused key codes, so a controller can't also fire the game's native path.
The game's joystick entries share a table with keyboard codes, so inactive slots are
disabled outright. Unrelated native keys pass through untouched.

Online, the remote mask is always written to the P2 table. The host reads it on its P2
poll, and the client reaches it on its P1 poll through a `playerIndex` swap. P1 is also the
menu navigation authority: local history on the host, confirmed remote input on the client.

### System keys

Besides the fight byte, each `PlayerInputRecord` carries a `systemState` byte
(`trial_hitbox_viewer/src/caster/menu_nav.h`). ESC is always allowed. F4 and F5 are added
only when both players opted in. `FilterOnlineSystemState()` applies both at capture and
when receiving, so a modified peer can't switch on anything else. Delay keys and mod tools
never enter the wire state. F4 and F5 run at a synchronized frame boundary
(`Engine::ApplyConfirmedPracticeSystemState()`) rather than through raw polling, so both
peers apply them on the same frame, and a late remote change rolls back.

Shortcuts share one table of semantic actions, `kDescriptors` in `shared/trial_shortcuts.h`.
The launcher stores their tokens and the DLL resolves them, so defaults and names can't
drift between the two.

### Launch focus

The game exits if its keyboard acquire fails, and a game launched in the background has no
focus at that moment. `InstallDinputFocusGuard()`
(`trial_hitbox_viewer/src/hooks/dinput_focus_guard.h`) patches the import table during
process attach and swallows only that specific failure. The game's per-frame poll acquires
again when focus arrives, so input still follows the window.

## Shared utilities

Both binaries compile `shared/`, so they can't disagree on these:

| File | Purpose |
| --- | --- |
| `shared/trial_compatibility.h` | `kCompatibilityId`, the single compatibility number |
| `shared/trial_versions.h` | Display versions for the launcher and DLL |
| `shared/autopunch_protocol.h` | Coordinator endpoint, timings and packet encoding |
| `shared/autopunch_client.cpp` | `trialnet::autopunch::Client`, used by launcher, players and viewers on a socket they own |
| `shared/trial_shortcuts.h` | The shortcut table and token normalization |
| `shared/input_binding_tokens.h` | Input source and binding token parsing |
| `shared/controller_device_routing.h` | Which physical controller belongs to which player (`PreferredControllerDeviceOrder()`) |
| `shared/offline_input_routing.h` | Private key code plan for offline controllers |

## Display wrapper

`ddraw_wrapper/ddraw.c` presents the game's DirectDraw calls through Direct3D 9
(`ddraw_wrapper/render_dx9_imgui.cpp`). `EFZDDRAW_SetFrameLimit` publishes its limit as an
interlocked scalar, because another thread can't safely write a 64-bit deadline in this
32-bit process. The window procedure forwards focus changes so the game can release and
reacquire DirectInput. DPI functions are looked up at runtime, since they may be absent on
older systems. Overlays declare `wantsDraw` and `isVisible` separately, so one can draw
without taking input. Before a device reset, the wrapper notifies the mod's separate ImGui
backend first, and each `ImGui_ImplDX9` call needs its own context current.

## Wine and Proton

Under Wine the mod runs the same Windows code, with extra care around timing and the things
Wine implements differently. Detection uses `DetectCompatibilityRuntime()`
(`trial_caster/src/compatibility_runtime.cpp`) in the launcher and `IsWineRuntime()`
(`trial_hitbox_viewer/src/util/win32_compat.h`) in the DLL. Both look for Wine's exports in
`ntdll`, and Proton is recognized from its environment variables.

- **Native `ddraw.dll`.** The wrapper must load ahead of Wine's own. For the game process,
  `EnsureNativeDirectDrawOverride()` adds a native-first rule to `WINEDLLOVERRIDES` and keeps
  unrelated entries. `scripts/launch_wine.sh` does the same for the launcher. The DLL refuses a
  managed session under Wine if the custom exports are missing.
- **Slow startup.** `WaitForDDrawReady()` allows much longer under Wine, since a cold prefix can
  spend a long time creating graphics devices.
- **Hook installation.** `InstallHooks()` queues all hooks and enables them together with
  `MH_ApplyQueued`, so all threads are suspended only once. `CreateFileA` is patched only in the
  game's import table (`PatchImportAddressSlot()`), because system exports under Wine may be
  thunks that can't be safely rewritten.
- **Loader lock.** Winsock starts on the worker thread (`InitializeSocketRuntime()`), because
  its startup may load provider DLLs. `PinActiveModule()` keeps the DLL loaded for the life of
  the process.
- **Socket handoff.** The launcher closes its socket before the game resumes. `BindLocalPort()`
  also retries briefly on a managed host if the port is still busy.
- **Timing.** `WaitForQpcDeadline()` yields with a real scheduling point during the last
  milliseconds of a frame, because sub-millisecond waits are unreliable there.
- **Name lookups.** `GetAddrInfoW` can't be cancelled on older Wine, so
  `trial_caster/src/util.cpp` allows at most one abandoned resolver at a time.
- **Random and crypto.** `trial_hitbox_viewer/src/util/win32_compat.cpp` loads `bcrypt.dll` at
  runtime rather than as a load-time dependency, and `InitializeRuntimeCompatibility()` proves
  that a secure random provider exists before play starts.
- **Writable storage.** `MutableStateRoot()` (`trial_hitbox_viewer/src/util/state_paths.h`)
  probes for a writable folder, trying `TRIAL_NETPLAY_STATE_DIR`, the game folder, the launcher's
  session folder, `LOCALAPPDATA`, then Temp. A read-only Steam library is fine.
- **Helpers.** `scripts/wine_preflight.sh` checks a setup without starting the game, and
  `--network` sends one registration to the coordinator and validates the mapping it returns.
  The `ddraw=n,b` rule keeps Wine's built-in DirectDraw as a diagnostic fallback. Launcher errors also add advice through
  `CompatibilityLaunchAdvice()`.

## Build and test

Everything is 32-bit, because the game is. Build in this order, since later steps use earlier
results: the launcher's tests inspect the built `netplay.dll` and `ddraw.dll`.

```powershell
cmake -S ddraw_wrapper -B ddraw_wrapper/build -A Win32
cmake --build ddraw_wrapper/build --config Release

cmake -S trial_hitbox_viewer -B trial_hitbox_viewer/build -A Win32
cmake --build trial_hitbox_viewer/build --config Release

cmake -S trial_caster -B trial_caster/build -A Win32
cmake --build trial_caster/build --config Release
```

The wrapper also builds with MinGW-w64, using the MinGW Makefiles generator and `-m32` in
the C flags. Its install step copies `ddraw.dll` to the prefix you give it. That link bundles libgcc, so the game folder needs no extra runtime, and strips stdcall
suffixes from the exports.

Release builds of `netplay.dll` link with `/DEBUG` for symbolized crash reports, with `/OPT:REF`
and `/OPT:ICF` restoring the size that `/DEBUG` would give up. C++ and CRT fatals aren't structured
exceptions, so they reach `WriteCrashArtifacts()` through synthetic codes
(`trial_hitbox_viewer/src/util/crash_handler.cpp`). Logs open through `_wfsopen` with `_SH_DENYWR`,
so tails and the harness can read them while non-ASCII Wine paths still work. Memory-write logging
(`trial_hitbox_viewer/src/util/mem_write_log.cpp`) costs real frame rate and stays off unless you
set `THV_MEMLOG`. Developer-only telemetry polling is skipped in player and spectator sessions,
since it can perturb timing.

Run tests per project:

```powershell
ctest --test-dir trial_hitbox_viewer/build -C Release --output-on-failure
ctest --test-dir trial_caster/build -C Release --output-on-failure
```

`ddraw_wrapper` registers no tests of its own, and the launcher's tests check its exports.

| Project | CTest names |
| --- | --- |
| `trial_hitbox_viewer` | `spectator_checkpoint_codec`, `control_config_aliases`, `win32_wine_compat`, `netplay_live_delay`, `debug_log_policy` |
| `trial_caster` | `trial_protocol_codec`, `trial_launcher_session`, `trial_control_bindings`, `trial_autopunch_client`, `trial_component_versions`, `trial_game_injection` |

The game isn't in this repository, so the executable layout check is optional. To add it, pass
`-DTRIAL_EFZ_EXE_FOR_TEST=<path to EFZ.EXE>` when configuring `trial_caster`.

`scripts/deploy_game.ps1` packages the build into `dist/game`, refusing a diagnostic `ddraw.dll`.
`-GameDir` also copies the files to a game folder, and `-DeploySymbols` adds the PDB. Without
symbols, crash reports still carry module and offset for lookup later.

### The test harness

`scripts/run_harness_session.ps1` is the end-to-end test. It uses a local copy of the game in
`game/`, which git ignores, and starts a host and client on one machine. The DLL's harness
(`trial_hitbox_viewer/src/util/test_harness.h`) plays scripted matches under artificial delay,
jitter and loss, and each instance reports a `HARNESS VERDICT` for the runner to check.
`scripts/analyze_harness.py` helps compare logs afterwards.

The runner writes a `[test]` section into each instance's session file, and `input_seed` is shared
while `net_shim_seed` differs per side:

| Key | Default | Meaning |
| --- | --- | --- |
| `input_script` | 0 | Play scripted, hands-free input |
| `input_seed` | built in | Shared seed; each side derives its own pattern |
| `net_delay_ms` | 0 | One-way delay added to each outgoing logical send |
| `net_jitter_ms` | 0 | Uniform plus-or-minus jitter on that delay |
| `net_loss_pct` | 0 | Percent of logical sends dropped, capped at 90 |
| `net_shim_seed` | built in | Impairment RNG; use a different value per direction |
| `force_ko_after_frames` | 0 | Zero P2's HP this many frames into each round. Not recorded input |
| `required_matches` | 1 | Best-of-3 matches to complete before the next one starts, 1 to 32 |
| `gameplay_esc_after_frames` | 0 | Send ESC this many frames into the first match |
| `early_system_key_probe` | 0 | Send F4 and F5 in the first match |
| `max_battle_frames` | 0 | Total battle frame cap. Zero emits no verdict, and reaching the cap reports `FAIL` |

Impairment is applied to the logical send, above the overlapping input window, because dropping
single datagrams below it would barely register. Identical seeds would make both peers drop and
delay together and under-test one-sided outages.

A `PASS` needs a completed match, a synchronized return to character select and 120 frames of the
next match, with no state or replay mismatches. Both peers must report it for the requested set
length, and each process contributes one log line, so one peer can't satisfy the condition twice.
Missing evidence is never a pass. Only P1 originates scripted system keys, and P2 must see them over
SyncData, which proves `systemState` is carried. The last match is only a liveness check, so F4's
practice flag can't affect a winner the verdict reports. After its verdict a process stays alive
briefly so both sides see the terminal state.

Useful runner options:

| Option | What it proves |
| --- | --- |
| `-NoStress` | A clean link, for a baseline determinism run |
| `-RequiredMatches`, `-Frames` | Set length, and the safety frame cap. `-TimeoutSec 0` derives the time budget from them |
| `-AllowPracticeKeys` | `$false` proves mod keys are suppressed, `$true` proves F4 and F5 propagate. Delay keys stay local either way |
| `-DisconnectAfterSec` | Kills the client, then checks the host fails closed. With a viewer, it must also report an authenticated terminal |
| `-Spectator`, `-SpectatorCount` | Independent read-only viewers, each replaying the whole player lifecycle |
| `-SpectatorSource` | `Host`, `Client` or `Alternate`. Alternating shows that both players serve the same confirmed stream |
| `-SpectatorRedirectProof` | `Auto`, `Required` or `Disabled`, covering the host-to-client redirect |
| `-SpectatorLaunchDelaySec`, `-SpectatorStaggerSec` | Late joins against the retained stream |
| `-SpectatorDelayMs` and the other `-Spectator*` impairments | Per-viewer, bidirectional faults through `scripts/spectator_udp_proxy.ps1`. The player link is untouched |
| `-RequirePairedInputDigest` | Matching digest checkpoints between source and viewer |
| `-EarlySystemKeyProbe` | F4 and F5 in the first battle of a spectator disconnect run |
| `-MinHostVisibleHzP10`, `-MaxHostMainP95Us` | Frame rate and tick time from per-second battle samples, enforced on both peers despite the names |

Keep these in mind when changing it:

- Forced KO writes HP outside the input stream, so `-ForceKoAfterFrames` must be 0 for spectator
  fidelity runs. The runner refuses otherwise, and clears it itself for disconnect runs.
- A disconnect run uses a single viewer on the host, because a redirected viewer can't outlive a
  deliberately killed client.
- `$runLock` is a named mutex. Runners share UDP ports and would otherwise cross-pair game processes
  into a false pass.
- `$compatId` is read from `shared/trial_compatibility.h`, so synthetic sessions follow the real
  contract. `TRIAL_NETPLAY_STATE_DIR` is forced to the game folder, so logs stay where the runner
  looks, and `TRIAL_CASTER_ONLY` makes any stale second DLL image inert.
- Under the harness, `SpectatorRelaySoftLimit()` drops to 1, so a second host viewer exercises the
  redirect path without a crowd of real viewers.
- The game exiting early isn't proof of a graphics failure, since validation deliberately closes it.
  Launches retry with an escalating cooldown, and a failed pair restarts as a whole. A real client's
  port is chosen by the system, so tests read it from the log rather than from `port`.

Three smaller scripts support this. `scripts/spectator_udp_proxy.ps1` runs one process per viewer
and holds reordered packets long enough for later ones to overtake them, which reliable ACKs must
then repair. `scripts/test_spectator_udp_proxy.ps1` tests that proxy, and requires every configured
fault to actually fire. `scripts/capture_launcher.ps1` screenshots the launcher, and `-Maximize`
checks that the UI grows along with the window.

## Where would I change this

| I want to change... | Look at |
| --- | --- |
| A game address, or support a different build | `trial_hitbox_viewer/src/core/trial_addresses.h`, `ValidateTrialHostIdentity()` in `trial_hitbox_viewer/src/util/win32_compat.h` |
| The launcher UI | `trial_caster/src/caster_ui.cpp` |
| The launcher's handshake or packets | `NetSession::HostHandshake()` in `trial_caster/src/net_session.cpp`, `trial_caster/src/protocol.h` |
| What the launcher passes to the game | `WriteSessionFile()` in `trial_caster/src/session_file.cpp`, `TryLoadSession()` in `trial_hitbox_viewer/src/caster/caster_session.cpp` |
| How the game process starts | `GameLauncher::Launch()` in `trial_caster/src/game_launcher.cpp` |
| Anything that breaks compatibility | Bump `kCompatibilityVersion` in `shared/trial_compatibility.h` |
| Connection assistance | `shared/autopunch_client.cpp`, `shared/autopunch_protocol.h` |
| The player wire format | `trial_hitbox_viewer/src/core/trial_protocol.h` |
| Transport, handshake or timeouts in game | `trial_hitbox_viewer/src/caster/netplay_sync.cpp` |
| Menu navigation into a match | `OnModTick()` in `trial_hitbox_viewer/src/caster/netplay_bootstrap.cpp` |
| Battle-entry alignment | `StartNetplaySession()` in `trial_hitbox_viewer/src/rollback/rollback.cpp` |
| Prediction or replay behaviour | `Engine::AdjustPrediction()` and `Engine::BatchRender()` in `trial_hitbox_viewer/src/rollback/rollback_engine.cpp` |
| What rollback saves | `Engine::SaveInputs()`, `kDynamicBattleRegions`, `trial_hitbox_viewer/src/rollback/rollback_memory.cpp` |
| What hidden frames skip | `trial_hitbox_viewer/src/hooks/render_gate.cpp` |
| Frame rate | `trial_hitbox_viewer/src/hooks/game_pacing.cpp`, `IsSupportedTargetFps()` in `trial_caster/src/protocol.h` and `trial_hitbox_viewer/src/core/trial_protocol.h` |
| The display layer | `ddraw_wrapper/ddraw.c`, `ddraw_wrapper/render_dx9_imgui.cpp` |
| Desync detection | `trial_hitbox_viewer/src/rollback/desync_check.cpp` |
| The spectator protocol or playback | `trial_hitbox_viewer/src/caster/spectator_protocol.h`, `trial_hitbox_viewer/src/caster/spectator_player.cpp` |
| Hitbox drawing or colors | `trial_hitbox_viewer/src/hooks/collision_display.cpp` |
| Savestate contents or file layout | `trial_hitbox_viewer/src/practice/savestate_layout.h`, `trial_hitbox_viewer/src/practice/savestate.cpp` |
| Frame stepping | `HookedBattleTick()` in `trial_hitbox_viewer/src/hooks/framestep.cpp` |
| Shortcuts and their defaults | `kDescriptors` in `shared/trial_shortcuts.h` |
| Bindings and input routing | `trial_caster/src/control_bindings.cpp`, `trial_hitbox_viewer/src/hooks/dinput_relay.cpp`, `trial_hitbox_viewer/src/hooks/control_swap.cpp` |
| Packaging | `scripts/deploy_game.ps1` |
| The end-to-end test | `scripts/run_harness_session.ps1`, `trial_hitbox_viewer/src/util/test_harness.cpp` |
