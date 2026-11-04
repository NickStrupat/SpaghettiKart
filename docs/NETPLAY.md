# Netplay

Online races for SpaghettiKart, on the `netplay` branch of this fork: rollback netcode, every mode (Versus, Battle,
Grand Prix with CPUs), players meeting by room code on a small relay server and then connecting directly.

This file records what was built, why it was built that way, how to test it and what is still open. Code-level
details live in the headers under `src/port/netplay/`, which each start with a description of their part.

## Status

| Area | State |
|---|---|
| Versus 2–4 players, Battle 2–4, Grand Prix 1–2 players + CPUs (whole cups, podium) | Works, tested by the simulator and by hand |
| Rollback, desync detection | Works; an 8-frame rollback every frame stays in sync on every track and mode |
| Room codes, lobby, relay server | Works |
| Direct connections (UDP hole punching), relay fallback | Works on one machine; not yet tried across two home networks |
| Own view (each player sees and hears only their own player) | Works |
| Automatic input delay, connection stats overlay | Works |
| Sound effects across rollbacks | Works |
| Platforms | macOS on Apple Silicon only. See [Platforms](#platforms) |

## Playing

1. Run a relay server where both players can reach it: on one of the players' computers on the same network, or on a
   small server for players in different places. See `tools/netplay-relay/README.md`.
2. In the game: **Enhancements > Netplay > Open Netplay Lobby**. Enter the relay's `host:port`, then **Host**, or
   **Join** with the host's room code.
3. Pick a character, get ready; the host picks the mode, speed and track or cup, and starts.

Both players need the same build of the game and the same game files and mods; the relay refuses a mismatch. The relay
must be as new as the game (its protocol version is checked).

Settings in the netplay window, each per player:

| Setting | Default | Effect |
|---|---|---|
| Show only my player | on | Full-screen view of your own player instead of the split screen |
| Hear only my player | on | Only your camera's sounds, in proper stereo |
| Show connection stats | on | Ping per player, input delay, rollbacks and waits, top right during races |
| Input delay | Automatic | Frames your own input is held back. Automatic picks it from the ping at each race start |
| Connect directly to other players | on | Race traffic skips the relay when routers allow it. Shares your IP address with the room |

The host's gameplay settings (enhancements, rules, vehicle counts) apply to everyone for the race. The host can restart
a race; anyone can leave it, which returns everyone to the room.

## Decisions

Made by the fork's owner, in this order. Later work assumes them.

1. **Rollback netcode from the start**, not lockstep first. Each player's own kart reacts immediately; the network
   delay shows up as small corrections of the other karts instead. Library: [GekkoNet](https://github.com/HeatXD/GekkoNet)
   (BSD-2, C++20, used by the 3sx port), built from a pinned commit.
2. **Every mode**: Versus, Battle and Grand Prix with CPU drivers.
3. **Room codes and a relay**, not typing in IP addresses: players need no port forwarding, and the relay checks
   builds and game files before a race. Direct connections were added later on top of it.
4. **Cross-platform** was the first goal (Mac, Windows, Linux), which is why floating point is built for determinism
   (below). On 2026-11-03 the target narrowed to **Mac to Mac** (two Apple Silicon Macs); Windows and Linux are on
   hold.
5. **Responsive controls over smooth remote karts**: "jumps are fine". The automatic input delay rounds down, and goes
   to 0 frames on a local network.
6. **Sounds must not be wrong after a rollback**: re-simulated sounds are compared with the ones already played,
   rather than muted.

## How it works

```
thread5_iteration (src/main.c)
 └─ race frame, when a netplay race runs: Netplay_RunRaceFrame()
       ├─ local pad → PadInput {s8 x, s8 y, u16 buttons} → GekkoNet
       ├─ GekkoNet events, in order:
       │    Save    → SimState::Save   (sim sections + sim arena + World ranges)
       │    Load    → SimState::Load   (a rollback)
       │    Advance → race_sim_frame() + an off-screen race_render_frame()
       └─ SimState_DrawFrame(): the frame shown, drawn once more and then undone
```

### Determinism

Every peer runs the same simulation from the same inputs, so the simulation must give bit-identical results.

- The game is built with `-fno-fast-math -ffp-contract=off` (`/fp:precise` on MSVC), in `cmake/SetFlags.cmake`.
- The port's decorative ship actors (`AShip`, `ASpaghettiShip`, `AStarship`, which are not in the original game) moved
  from the C library's trigonometry to the game's own lookup tables, which the original code uses throughout, with
  explicit float-to-int casts (ARM and x86 convert out-of-range values differently). Other C library math remains,
  such as the podium trophy's, which runs outside netplay sessions. Only a determinism harness run between two
  platforms would show what else matters there; none has been done.
- `Sim_GameplayChecksum()` (`Checksum.cpp`) hashes players, actors, objects, cameras, the random seed, the race
  timer and the race's flow (results, menu items, transitions), skipping every pointer. GekkoNet compares it between
  peers to detect a desync.

### Savestates

A rollback needs to save and restore the whole simulation many times a second. The game has 275 MB of globals, almost
all of it memory pools and frame buffers; the simulation's own state is under 1 MB. So it is separated out:

- **Sim sections.** `SimSection.h` is force-included into every source file that holds simulation state. With clang's
  `#pragma clang section`, their globals and static locals land in their own linker sections, which are copied in one
  go. Anything that is not simulation state (render buffers, OS objects, memory pools) is wrapped in
  `NOT_SIM_STATE_BEGIN` / `NOT_SIM_STATE_END`. C and C++ files use separate sections, because C++ statics can own heap
  memory.
- **Sim arena.** Thrown shells, bananas and other C++ objects are created and destroyed during races. During race setup
  and each simulated frame in netplay, the global `operator new` allocates from `SimArena`, one fixed block whose
  bookkeeping lives inside it, so copying the block up to its high-water mark saves the whole heap. Code that makes
  long-lived, non-simulation allocations from inside a frame wraps them in `SimHeapPause`.
- **World ranges.** The containers of the C++ World are rebuilt inside the arena at race setup; their headers are saved
  through `World::ForEachSimStateRange`.
- A snapshot is about 570 KB. GekkoNet keeps only frame numbers; 32 snapshots live in a ring in `NetSession.cpp`.
- **Baseline.** Races leave values behind that the next race's setup never resets, and each peer has its own history
  (attract demos, offline races). Every netplay race series starts by putting the C sections back to how they were
  before the run's first race setup (`SimState_RestoreBaseline`), keeping the gamestate, save data and a few others.
  `RaceConfig_StartRace` then sets the race up the same way on every peer.

### The session

`NetSession.cpp` wraps GekkoNet.

- Each frame is exactly one of: a re-simulated frame (drawn off-screen, sounds compared afterwards), the newest frame
  (simulated, drawn, saved) or, while waiting for a remote input, the current frame drawn again.
- MK64's draw code writes simulation state (`init_render_player()`, menu items on the results screens), so every
  simulated frame, re-simulations included, is also drawn, split screen, into the gfx pool that is not on screen.
  What the player sees is a second draw that is undone after the display list runs (`SimState_DrawFrame` /
  `SimState_EndDisplay`).
- Prediction window 8 frames (266 ms). Time sync waits a frame now and then when this peer runs ahead.
- **Input delay.** Each peer picks its own at every race start: the one-way latency to its slowest peer in whole
  frames (33 ms each), at most 3. The latency comes from the direct ping, or from both players' relay pings when they
  are not connected directly.
- **Controls.** Leaving and restarting travel inside the inputs (two unused button bits), so every peer acts on the
  same frame.

### Race series

A lobby race is a series of segments, one GekkoNet session per loaded race (each course of a cup, each restart).

- Every way out of a race starts the game's quit transition. The segment freezes on that frame.
- Once that frame can no longer be rolled back (prediction window + 1 frames later), each peer tells the room. When all
  agree on the same frame, each ends its session; the fade and the next load then run outside any session, so a load
  is never rolled back.
- Back to the menus ends the series; the podium after a cup is shown by each peer locally.
- Menus that the original game lets anyone confirm are confirmed by player 1 (the host) only in netplay.

### Relay, lobby and direct connections

- `tools/netplay-relay/`: a standalone UDP server (C++20, standalone ASIO, Dockerfile). Wire format in
  `RelayProtocol.h`, shared with the game. Rooms by 6-character code; the relay forwards packets and checks the build
  id (commit plus a hash of local changes) and a hash of the game files.
- `RelayClient.cpp`: the game's side, two channels: lobby messages and GekkoNet packets (tagged per race, so stragglers
  from an earlier race are dropped).
- `Lobby.cpp`: no acknowledgements. Every member re-sends its full state every 250 ms; the host re-sends the room's
  settings and the race to run. A lost packet is replaced by the next one.
- **Direct connections.** The relay tells each member where the others are (the address it sees, and the address each
  reports on its own network) and a random per-member key. Each game punches those addresses from its relay socket;
  the first authenticated answer opens the direct way, and race traffic goes there. After 1.5 s of silence it falls
  back to the relay. Lobby messages stay on the relay.

### Own view and sound

- **Own view** (`race_render_own_view` in `src/main.c`): the player's screen scaled to the window with the one-player
  HUD (`gOwnViewPlayer`), minimap and rank. The simulation keeps its split-screen mode.
- **Own audio** (`audio_set_only_camera` in `src/audio/external.c`): other cameras' sounds are dropped; the kept camera
  gets one-player stereo.
- **Sounds across rollbacks** (`SoundRollback.cpp`): each played sound is logged with its frame. After a rollback, the
  re-simulated frames' sounds are compared with the log: the same sound from the same camera within 3 frames keeps
  playing; a new one plays now, slightly late; one that no longer happens is stopped.
- Positional sounds hold one of 50 audio slots until the sound bank is done with them; a request that is never queued
  frees its slot with `release_sound_slot()`, or the slots run out and positional sounds go silent.

## Testing

| Tool | What it does |
|---|---|
| `tools/netplay-sim.py` | Plays whole races, cups and battles between bot-driven copies of the game through a relay, fast-forwarded (about 70× real time), and checks that every peer stayed in sync and the races ended as expected. 15 scenarios, about 4 minutes. **Run it after any change to netplay or simulation code.** |
| Determinism harness | `SK_DETERMINISM_RECORD` / `_VERIFY` / `_ROLLBACK=8`: records a race's inputs and per-frame checksums, replays them, or rolls back 8 frames after every frame. See `DeterminismHarness.h`. |
| `tools/netplay-relay/test_relay.py` | Protocol tests for the relay. |
| `tools/netplay-diff.py` | Names the variables that differ between two peers' start states (`SK_NETPLAY_DUMP=1`) or in a rollback trace. |

The test environment variables (direct peers, lobby bots, simulated lag and loss, scripted leave/restart, bot
drivers) are listed in `src/port/netplay/Netplay.h`. `SK_FAST=n` runs any of them without frame pacing or sound
output, drawing one frame in n.

When a session fails, each peer writes `<prefix>.seg<N>.kept.txt`, the checksum breakdown of every frame it still
kept; diffing the peers' files shows the first component that differs. A peer that detects a desync also writes
`<prefix>.desync-p<player>-s<segment>-f<frame>.txt`. The prefix is the test log path (`SK_NETPLAY_LOG`), or else
`netplay/<date-time>` in the game's folder (`~/Library/Application Support/SpaghettiKart/` on macOS), so a desync
between two computers leaves a report on each.

If the game stops responding for 5 s while in a netplay room or race, `HangWatchdog` runs macOS's `sample` tool on it
and writes every thread's call stack to `netplay/<date-time>.hang.txt` in the same folder (wait about 10 s before
force-quitting).

## Pitfalls found so far

Each of these cost a debugging round.

- **Draw code changes simulation state** (`init_render_player()`, results-screen menu items, the GP points count).
  Never skip drawing a simulated frame.
- **Memory-pool allocations are not in snapshots.** A pool buffer that changes during a race must move to a global
  (`sGPPointsCopy` did).
- **Allocating from inside a simulated frame lands in the sim arena.** Non-simulation code reached from there must
  use `SimHeapPause` (C: `SimHeap_Pause` / `SimHeap_Resume`), or a rollback frees its memory from under it. Found so
  far: the sound log (`play_sound`), the music player (`HMAS_*`), and the music volume setting the race writes at its
  start and when L is pressed. That last one corrupted the settings table on every race with fresh settings; the next
  settings save (opening the menu) then never finished.
- **Machine-local values fed back into the simulation**: the kart animation once read the frame-buffer index; a voice
  clip variant came from the audio engine's random number. Use simulation state instead.
- **The course-select menu rewrites the track every frame**: apply race settings after the frame's game logic.
- **Attract demos** leave flags behind (`gDemoMode`, `gDemoUseController`), and the baseline can be copied during a
  demo's setup; the race start clears them. The demo only starts from the title screen (`gSkipIntro` 1), so tests need
  that setting to reach it.
- **Pointers in raw-byte hashes**: `RailroadCrossing` overlays a pointer on other fields; the checksum skips it.
- **Leftovers from each peer's own history in hashed data**: menu items keep a texture slot and fields that
  `add_menu_item()` never sets, and a cleared item keeps all of them. Hashing the whole `gMenuItems` array made two
  computers with different settings (one starting at the title screen, one playing the intro) desync on frame 0. The
  checksum hashes only active items' state.
- **C++ statics that own heap memory** dangle after a section restore; the baseline restores C sections only.
- **The audio thread must not read CVars** (it raced the main thread's CVar map).
- Not yet seen but possible: music sequence scripts write a simulation global (`func_8001AAAC`).

## Platforms

- **macOS, Apple Silicon**: the only platform built and tested. The app runs on macOS 15 and later; building it so
  needs `tools/macos-build-deps.sh` (see `docs/BUILDING.md`).
- **Linux**: needs a clang build (GCC cannot place globals in sections); untested.
- **Windows**: MSVC cannot place globals in sections, so its builds refuse to start netplay ("build it with clang").
  A clang-cl build would be needed; untested.
- Mixed platforms need the determinism harness run between them first.

## Open

- Direct connections across two real home networks. Routers that use a new public port per destination (symmetric
  NAT, common on mobile networks) stay on the relay.
- The rollback stress test under AddressSanitizer.
- Own view: the one-player Grand Prix rank portraits and the speedometer are not drawn; the end-of-race banners have
  not been checked visually.
- With "Hear only my player", the other player's kart sounds are not heard.
- A looping sound that a misprediction stopped is not restarted.

## History

| Commit | Change |
|---|---|
| `143bb54d` | Deterministic simulation, savestates, GekkoNet session (phases 0–2) |
| `39057f0b` | Relay server, room codes, lobby window (phase 3) |
| `6fe7a97b` | Race series with a transition barrier, Grand Prix cups, leave and restart (phase 4) |
| `4f95544e` | Fix a Metal shader compile error on macOS 27 |
| `8b0deb4d` | Let player 1 use one chosen gamepad |
| `51dbd11d` | Start every race series from a clean baseline |
| `c1fcaf94` | Own view (phase 5) |
| `9f7df5e9` | Simulator, racing bots, fast mode; GP results desync fix |
| `1ee60274` | Hear only your own player |
| `62c5c104` | Bots brake for bends and follow the racing line |
| `3fbf25df` | Automatic input delay, stats overlay, sounds across rollbacks (phase 6) |
| `499e3b9b` | Direct connections (phase 6) |
| `51a204fd` | Fix races quitting at once after an attract demo |
