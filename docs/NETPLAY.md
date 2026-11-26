# Netplay

Online races for SpaghettiKart, on the `netplay` branch of this fork: rollback netcode, every mode (Versus, Battle,
Grand Prix with CPUs), players meeting through a small relay server (a public room from its list, a private room by its
code, or matchmaking), then connecting directly.
In a room, everyone plays on one shared console, as if each player's controller were plugged into it: the game's own
menus pick the mode, characters and course.

This file records what was built, why it was built that way, how to test it and what is still open. Code-level
details live in the headers under `src/port/netplay/`, which each start with a description of their part.

## Status

| Area | State |
|---|---|
| Versus 2–4 players, Battle 2–4, Grand Prix 1–2 players + CPUs (whole cups, podium) | Works, tested by the simulator and by hand |
| Grand Prix 3–4 players + CPUs (the port's own: the original game had none) | Works in the simulator; not yet played by hand |
| 5–8 players: Versus, Battle, Grand Prix + CPUs, in an eight-screen split (`SCREEN_MODE_8P`), from up to 8 controllers on one computer | Works in the simulator (`gp-5p`, `battle-6p`, `pause-6p`, `room-8`, `local-6p`) and the rollback stress run; not yet played by hand. Online each computer shows its own player full screen; between Grand Prix races, on a shared screen, every player's screen shows beside the results, a quarter of the size, with 3 to 8 players |
| Rollback, desync detection | Works; an 8-frame rollback every frame stays in sync on every track and mode |
| Desync recovery (everyone takes over the host's state, the race goes on) | Works, tested by the simulator on one Mac; not yet tried between two |
| Room list, ONLINE screen (Z on Game Select), relay server | Works; tested with scripted buttons |
| Public and private rooms, room codes of stick directions, matchmaking in the background | Works in the simulator, the relay's tests and screenshots of the ONLINE screen; not yet tried by people. The netplay window's new parts and the bottom-of-screen notice are not seen yet (ImGui is not in test screenshots) |
| Shared console: Game Select, character select and course select in lockstep, then the race | Works, tested by the simulator |
| Direct connections (UDP hole punching), relay fallback | Works on one machine and between two Macs on one wifi; not yet tried across two home networks |
| Own view (each player sees and hears only their own player) | Works |
| START pauses an online race for everyone; the pause menu's choices | Works in the simulator (continue, course change) and in pictures of both views; not yet tried by people |
| Players at different frame rates (Settings > Graphics > Current FPS) | Works: frame interpolation only draws, every game simulates 30 frames a second (`mixed-fps` scenario) |
| Automatic input delay, connection stats overlay | Works |
| Sound effects across rollbacks | Works |
| Voice chat between players connected directly | Works in the simulator and the browser checks, with stand-in microphones and the Mac's real one; not yet heard by people, and echo on speakers is not measured yet. See [Voice chat](#voice-chat) |
| Platforms | macOS on Apple Silicon only. See [Platforms](#platforms) |

## Playing

1. The game uses the owner's relay, `netkart64.duckdns.org:7777`, by default. To use another one, run a relay server
   where both players can reach it: on one of the players' computers on the same network, or on a small server for
   players in different places. See `tools/netplay-relay/README.md`.
2. For another relay only: **Enhancements > Netplay > Open Netplay Lobby**, and enter the relay's `host:port` there.
3. On Game Select, press **Z** (**ONLINE**, beside L OPTION and R DATA). The ONLINE screen offers:
   - **QUICK MATCH**: look for other players. Press B and keep playing: the menus, or offline races. The search goes
     on in the background, with a notice at the bottom of the screen, and the window's Dock icon bounces when it
     finds someone. A match takes you into the room from the menus; if you are racing offline, the room waits for you
     to finish or quit the race (3 minutes at most, then you leave it). The relay first fills an open public room, and
     otherwise pairs you with another player looking; if that room closes before you played, the search starts
     again. **CANCEL MATCH** stops it.
   - **HOST PUBLIC ROOM**: a room listed under your name, which matchmaking also fills.
   - **HOST PRIVATE ROOM**: a room that is never listed. Its code shows beside Z LEAVE on Game Select.
   - **JOIN BY CODE**: type a room's code, 7 directions, by flicking the stick or pressing the D-pad, the C buttons or
     the keyboard's arrow keys (the C buttons by default). B erases.
   - The relay's open public rooms, by their host's name, to pick from. **NAME** sets the name others see.
4. In a room, every player's game shows the same screens. You are the controller of your place in the room (the host
   is controller 1); the names along the bottom of Game Select say who is who. Pick the player count, mode,
   characters and course in the normal menus, as on one console: anyone can move the Game Select and course cursors,
   each player moves their own character cursor. Right from 4P on Game Select goes on to 5P to 8P.
5. **Z** on Game Select (now **LEAVE**) leaves the room and comes back to the ONLINE screen.

A room holds 8 players; a computer brings one player per controller port in use, up to 8. All 8 can race: 5 to 8
players share the console's screen in a 3x2 or 4x2 grid (an odd count leaves a cell for the minimap), and each
computer shows its own player full screen, as with fewer. Game Select offers no more
players than the room has, and players beyond the chosen player count sit the race out and watch player 1. OPTION, DATA and Time Trials are not
available in a room: they use each player's own saves and ghosts. Someone joining during a race waits for it to end;
whenever the players change, everyone starts over on Game Select.

A computer's players are counted from its controllers (`Lobby::GetLocalPorts`): port 1 always, plus each of ports 2
to 4 that has a connected gamepad turned on for it in the Input Editor and gamepad bindings. The netplay window lists
them. A change counts after 2 seconds, so a gamepad that drops out briefly doesn't restart the console. With 2 or more
racing, the computer shows the split screen and plays every camera's sound, as one console does. Tests have no
gamepads: `SK_TEST_LOCAL_PLAYERS=N` stands for ports 1 to N.

**START** in a race pauses it for everyone, as on one console. Only the player who paused controls the pause menu;
the others see their race stopped with a "PLAYER 2 PAUSED" line. Course Change, Driver Change and Quit take everyone
to that menu together.

Both players need the same build of the game and the same game files and mods; the relay refuses a mismatch. The relay
must be as new as the game (its protocol version is checked). Players in a browser (https://netkart64.duckdns.org/,
see `docs/WEB.md`) share rooms with desktop players of the same build.

Settings in the netplay window, each per player:

| Setting | Default | Effect |
|---|---|---|
| Show only my player | on | Full-screen view of your own player instead of the split screen |
| Hear only my player | on | Only your camera's sounds, in proper stereo |
| Show connection stats | on | Ping per player (network, and frame wait: packets waiting for a frame to read them), input delay, rollbacks and waits, top right during races |
| Smooth other karts' corrections | on | After a rollback, other karts glide to where they now are instead of jumping |
| Input delay | Automatic | Frames your own input is held back. Automatic picks it from the ping at each race start |
| Connect directly to other players | on | Race traffic skips the relay when routers allow it. Shares your IP address with the room |
| Voice chat | on | An open microphone in a room, in the menus and in races. Only between players connected directly: voice never goes through the relay. The system asks for the microphone the first time |
| Mute my microphone | off | Keep hearing the others, send nothing |
| Voice volume | 100% | How loud the others' voices are |
| Your controller | All controllers | The one gamepad this copy of the game reads, by its serial number, so two copies on one Mac each keep their own |

Each player's row in the netplay window says where voice stands with them (talking, off, no direct connection) and
has a box to mute them; a corner of the screen names who is talking.

The host's gameplay settings (enhancements, rules, vehicle counts, unique characters) and whether the host unlocked
Extra mode apply to everyone in the room. Unique characters is on unless the host turned it off: with up to 4 players
a character cursor cannot move onto another; with more, cursors pass each other and A on a character another player
has picked is refused (`character_taken` in `src/menus.c`), as 6 cursors on 8 characters left most moves blocked. A cursor plays its move sound only when it moves: the original relied on its own spot never being free, which stopped holding once spots could be shared (above 4 players, or unique characters off), and the sound replayed every frame. With unique characters off, two players can drive one character, so Grand Prix points and the overall ranking are kept by kart (`gGPPointsByPlayerId`, `gPlayerIdByGPOverallRank` in `src/menu_items.c`), where the original kept them by character: two Marios shared one total, and the standings listed a character nobody drove (`console-gp-same`). During a race, the netplay window can leave it, which takes everyone back to
the menus.

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
7. **One shared console per room** (2026-11-05): the relay lists rooms, so nobody types a code, and inside a room the
   game behaves as if all players were at one console with a controller each. Mode, characters and course come from
   the game's own menus, not a separate lobby screen. The player count stays free, as on a console.
8. **Address space layout randomization off** (2026-11-06), so that desync recovery can copy the host's state, raw
   pointers and all, into the other games, rather than guess which numbers in it are pointers. The game restarts
   itself without it at launch. This weakens the protection against someone in the room, or the relay's operator,
   exploiting a memory bug in netplay's packet handling; accepted for playing with friends on one's own relay.
9. **Public rooms, private rooms and matchmaking** (2026-11-17). Matchmaking runs in the background while the player
   plays offline; a match never interrupts an offline race (the room waits, as a console start restores the boot
   state, which only the menus are ready for). It makes rooms of 2 that stay open to more, and fills open public rooms
   first. Room codes are 7 stick directions (up, down, left, right: perpendicular, so easy to hit, and typed with the
   keyboard's arrow keys too), chosen for their rhythm; that and the new messages are protocol 4.
10. **START pauses an online race for everyone** (2026-11-17), as on one console, where only the player who paused
    controls the pause menu. Until then START was dropped from race input, as pausing one peer alone made no sense.
11. **5 to 8 players from [chrisgilpin/MK64-8p-local](https://github.com/chrisgilpin/MK64-8p-local)** (2026-11-20):
    a fork of the same upstream with local 8-player split screen. Its 8-player commits were cherry-picked (author
    kept, `cherry picked from` noted), and only those: not its custom tracks and cups, 400cc, item balance, track
    editor, debug keyboard shortcuts, or its libultraship changes, which are not published (controller ports 5 to 8
    came later from the owner's libultraship fork, decision 12). Where it overlapped the port's own 3-4 player Grand Prix, the port's version was kept. Its
    `docs/8P-STATUS.md` lists the bug classes it met; the pitfall below adds the ones found adopting it.
12. **libultraship from the owner's fork** (2026-11-20): controller ports 5 to 8 need libultraship's ControlDeck to
    have 8 ports. The submodule follows branch `eight-ports` of
    [NickStrupat/libultraship](https://github.com/NickStrupat/libultraship), upstream plus one commit: a
    `LUS_MAX_PORTS` CMake setting (default 4) that sets `MAXCONTROLLERS`, the ports that ignore a new gamepad, and the
    Input Editor's port tabs. The game sets it to 8 (`cmake/dependencies/common.cmake`). It can go upstream as a PR.

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
- **Ping and frame wait.** Pings are timed from when a reply reached the computer (the system's receive stamp on the
  desktop, the event's time in a browser), not from when the game read it. GekkoNet reads its packets only once a
  frame, so its ping in the stats overlay also holds that wait at both ends, up to a frame each: each peer averages how
  long GekkoNet's packets waited (`RelayClient::GetGekkoWaitMs`), shares it in its member state, and the overlay
  splits the ping into network and frame wait.
- **Controls.** Leaving and restarting travel inside the inputs (two unused button bits), so every peer acts on the
  same frame.
- **Stick positions.** GekkoNet compares inputs byte for byte, so every wobble of a held stick would be a wrong
  prediction and a rollback. Before a race input is sent, its stick becomes one fixed position among those the race
  treats the same (`CanonicalStick.cpp`): MK64 ignores the stick near the centre, steering stops growing near the edge,
  and the Y axis only matters for backing up and throwing items.
- **Smoothing.** After a rollback, a kart can be somewhere else than the shown frames led up to. The shown frame
  draws it offset by how far the rollback moved it, and the offset fades over about 150 ms (`KartSmoothing.cpp`);
  `SimState_EndDisplay` undoes the move with the rest of the draw. Karts that a camera on screen follows are left
  alone, and an offset past 36 units (four frames at top speed) is shown as a jump. Between bots, rollbacks move a
  kart under 5 units, a kart being about 11 wide.

### Desync recovery

When the peers' simulations go apart, every player takes over the host's state and the race goes on (`Resync.cpp`,
`NetSession.cpp`), after a freeze of well under a second.

- A snapshot is raw memory. Every copy of the game restarts itself at launch without address space layout
  randomization (`SimState_DisableAddressRandomization`, in `main()`), so the image sits at `0x100000000`, and the sim
  arena sits at `0x500000000000`; their pointers then hold in another copy of the same build. Pointers into the
  regular heap (the loaded game files, about 90 in a race's state) differ between copies: the host marks each one,
  and the other players keep their own values there.
- A peer that detects a desync tells the others (a race-data message through the room) and every peer freezes. The
  host goes back to its state of the newest frame whose inputs are final, packs it with zlib (560 KB to about 75 KB)
  and serves it in 1 KB chunks; each other player pulls them, unpacks the host's state onto its own and checks that
  its checksum is now the host's. Everyone then confirms that frame through the segment barrier, and a new segment
  (GekkoNet session) starts from it.
- If any step fails or takes over 15 s, the race ends as before. The desynced segment's checksum log is written as
  `.seg<N>.desynced`, along with the usual desync and kept-frames reports: a recovered desync is still a bug to find.

### Race series

A lobby race is a series of segments, one GekkoNet session per loaded race (each course of a cup, each restart).

- Every way out of a race starts the game's quit transition. The segment freezes on that frame.
- Once that frame can no longer be rolled back (prediction window + 1 frames later), each peer tells the room. When all
  agree on the same frame, each ends its session; the fade and the next load then run outside any session, so a load
  is never rolled back.
- Back to the menus ends the series; the podium after a cup is shown by each peer locally.
- Menus that the original game lets anyone confirm are confirmed by player 1 (the host) only in netplay.
- **Pausing:** START is race input like any other button, so every peer pauses on the same frame, and the pause menu
  reads the controller of the player who paused, as the game does. Its choices start the quit transition like the
  results menu's. A pause can never be predicted wrongly: prediction repeats a player's last input, which never makes
  a new press. With "Show only my player", the pause menu belongs to a split screen nobody sees: the player who paused
  gets it as a one-player game's, the others a line saying who paused (`render_own_view_menus()`). Any of 8 players
  can pause. On the eight-screen split, and in Grand Prix with 3 or 4 players, the menu's position tables have no
  row, so it is drawn over the whole window as a one-player game's (`pause_menu_screen()` in `menu_items.c`).

### Shared console

A room plays on one shared console (`Lobby::ConsoleStart`). The host starts one whenever the members change, once
every member's state has arrived and nobody is racing; it lists the members in controller order and carries the
host's rules (whitelisted gameplay settings, the Extra mode unlock). `Netplay.cpp` then runs the console's sessions
one at a time, numbered in one sequence:

- **Menus in lockstep** (`MenuSession.cpp`): a GekkoNet session with no prediction (GekkoNet's lockstep mode). A menu
  frame runs only once every player's input for it has arrived; until then the game shows the last frame again
  (`Netplay_IsWaitingForMenuInput`). Nothing is rolled back, so menu loads and the draws that change menu state run
  exactly once per frame on every peer. A new console starts from the globals as they were at boot
  (`SimState_RestoreBootState`), resets to Game Select and restarts the frame counter the menus' flashing follows.
- **Menus to race:** when the menus set up a race, the menu session freezes on its last frame and a rollback session
  takes the race from its first frame (`RaceSetUpByMenus`). The frozen session keeps handing out input until every
  peer confirmed that same last frame through the race barrier. The relay client keeps two GekkoNet sessions apart by
  their tags (two lanes), as both run for a moment.
- **Race to menus:** when a race series ends in the menus, a menu session starts on the agreed frozen frame, without
  a reset: every peer has the same state there. After the Grand Prix podium, which each peer shows on its own, the
  menus start over from Game Select.
- Players beyond the race's player count are full session peers whose input no player reads; they watch player 1.

The **ONLINE screen** (`OnlineMenu.cpp`) is a menu screen of the port's own (`ONLINE_MENU`), drawn with the menus'
helpers. Game Select's "Z ONLINE" button is the port's own art (`tools/make-online-button.py`, converted with
`tools/png-to-texture.py`), in the style of OPTION and DATA.

The **5 to 8 player menu art** is made at runtime from the game's own textures, so nothing derived from the ROM is in
the repository (`src/menu_items.c`). Game Select's fourth box stands for 4 to 8 players: at 5 to 8 it reads 5P GAME to
8P GAME, its digit redrawn in the style of the box's own 1 to 4 (`make_game_boxes`), and keeps its GP / VS / BATTLE
list (`game_select_column_type`). Character select's cursors for players 5 to 8 are the cursors of players 1 to 4
turned in hue, with their number redrawn in the corner (`make_extra_cursor_borders`). The 5P to 8P box still shows the
4P box's four faces.

### Relay, lobby and direct connections

- `tools/netplay-relay/`: a standalone UDP server (C++20, standalone ASIO, Dockerfile), which also accepts browsers'
  WebSockets on the same port number over TCP (`WebSocket.h`, see `docs/WEB.md`). Wire format in
  `RelayProtocol.h`, shared with the game. Rooms of up to 8 by a code of 7 directions (protocol 5); public ones are listed
  (`ListRooms`) with the host's name to clients of the same build, plus counts of the open rooms of other builds and
  of this build with other game files, which the ONLINE screen shows (the browser build updates itself, so a friend is
  often one restart behind, `PageVersion.h`; game files made by an older extractor stay in use until remade,
  `AssetsVersion.h`); the relay forwards packets and checks the build id (commit plus a hash of local changes) and a
  hash of the game files.
  Private rooms are never listed. **Matchmaking** (`Matchmake`, resent every second as a keep-alive): the relay puts
  the player into the fullest open public room of its build and game files that is not racing, else pairs it with the
  player that has waited longest, who hosts the new room. Joins of rooms that do not exist have their own rate limit,
  which keeps guessing codes slow.
- **Away players** (`Lobby::Member::Away`): a player in a room but outside the menus, with no console running (matched
  while racing offline, or during the title screen's attract demo), is away. The host starts no console while anyone
  is, and an away player who stays away 3 minutes leaves the room. `Netplay::GetMatchNotice()` says what is going on
  at the bottom of the screen (`NetplayMatchOverlay`).
- `RelayClient.cpp`: the game's side, two channels: lobby messages and GekkoNet packets (tagged per race, so stragglers
  from an earlier race are dropped), plus a third the browser build sets up WebRTC connections with, and a fourth for
  voice, which only ever goes directly ([Voice chat](#voice-chat)). It moves packets
  through a `RelayLink` (`RelayLink.h`): UDP on the desktop (`RelayLinkUdp.cpp`), a WebSocket and WebRTC in a browser
  (`RelayLinkWeb.cpp`). Desktop games also answer browsers' WebRTC connections, through libdatachannel
  (`WebRtcPeers.cpp`, `WebRtcTransportDesktop.cpp`; see `docs/WEB.md`).
- `Lobby.cpp`: no acknowledgements. Every member re-sends its full state every 250 ms; the host re-sends the room's
  settings and the race to run. A lost packet is replaced by the next one.
- **Direct connections.** The relay tells each member where the others are (the address it sees, and the address each
  reports on its own network) and a random per-member key. Each game punches those addresses from its relay socket;
  the first authenticated answer opens the direct way, and race traffic goes there. After 1.5 s of silence it falls
  back to the relay. Lobby messages stay on the relay. A member on the relay's own computer (whoever hosts the relay
  and connects to it at 127.0.0.1) has only loopback addresses; the others send to the relay's address instead, with
  that member's port.

### Own view and sound

- **Own view** (`race_render_own_view` in `src/main.c`): the player's screen scaled to the window with the one-player
  HUD (`gOwnViewPlayer`), rank, and either the minimap or the speedometer (C-right switches, as the two-player
  minimap toggle does; two-player races start with the minimap off). Grand Prix adds the one-player top-four column
  (`render_own_view_ranking`). The simulation keeps its split-screen mode, so whatever only the one-player race
  animates (speedometer needle, the column's slides) is drawn from the current state instead, without the animation.
  `render_own_view_overlay` draws over the whole window: the screen's own HUD pass is clipped to its split-screen size.
- **Own audio** (`audio_set_only_camera` in `src/audio/external.c`): other cameras' sounds are dropped; the kept camera
  gets one-player stereo. Another player's engine belongs to their camera, so `func_800C94A4` also starts it as heard
  from the kept camera, from where their kart is, the way the game plays a CPU's engine.
- **Sounds across rollbacks** (`SoundRollback.cpp`): each played sound is logged with its frame. After a rollback, the
  re-simulated frames' sounds are compared with the log: the same sound from the same camera within 3 frames keeps
  playing; a new one plays now, slightly late; one that no longer happens is stopped.
- **Stops across rollbacks**, the same way: each stop the simulation makes records what it silenced. A stop that a
  rollback no longer makes starts those sounds again (a shell's hum, the star), in a new slot for a positional sound;
  a new one is carried out late. Stops that first look up a sound's slot (`stop_slot_sounds`: vehicles, actors, the
  star) are tracked by what the sound follows, because finding the slot depends on this computer's audio: a
  re-simulated frame may no longer find a slot its first run already freed.
- Positional sounds hold one of 50 audio slots until the sound bank is done with them; a request that is never queued
  frees its slot with `release_sound_slot()`, or the slots run out and positional sounds go silent.

### Voice chat

An open microphone among a room's members (`VoiceChat.h`), by the owner's decision of 2026-11-16: no push-to-talk,
and **only over direct connections**, so that the relay, a Raspberry Pi, carries none of it. Two members whose routers
keep them on the relay do not hear each other, and the netplay window says so. A room holds 8 members: at most about
280 kbps each way per player with everyone talking, and next to nothing in silence.

- **Transport.** A fourth payload channel in `RelayClient.cpp`. `SendVoice()` has no way through the relay, and voice
  that arrives through it is dropped. Punched UDP and WebRTC data channels both carry it: a browser and a desktop game
  talk too. The relay and its protocol are unchanged.
- **Codec.** Opus (libopus 1.5.2, built from source, `cmake/dependencies/common.cmake`): 48 kHz, one channel, 20 ms
  frames at 24 kbps. Silence is not sent (the codec's DTX). Nothing is resent: each frame carries a coarse copy of
  the one before, which patches over one lost frame, and the codec guesses at up to two more.
- **Each game frame, on the game thread** (`VoiceChat::Update()`, from `Netplay_BeginFrame()`): the microphone's
  samples are packed and sent to every member who uses voice chat and is reached directly (members say so in their
  lobby state); what arrived is unpacked into a queue per member, and the queues are mixed into the device, which is
  kept about 60 ms ahead. A member's voice starts once 40 ms of it has arrived.
- **The microphone opens only when there is someone to talk to**, and closes 5 s after the last such member is gone.
- **Devices** (`VoiceDevice.h`). macOS: the voice processing audio unit (`VoiceDeviceMac.cpp`), which removes what
  the Mac plays from what the microphone hears, so players on speakers do not echo; it uses the system's selected
  microphone. Browser: `getUserMedia` with the browser's echo cancellation and an AudioWorklet
  (`VoiceDeviceWeb.cpp`, `src/port/web/VoiceDevice.js`, `VoiceWorklet.js`). Tests: `VoiceDeviceTest.cpp` hums a note
  per player and reports the notes that reach its speakers.
- **Not simulation state.** Nothing in voice chat reads or writes the simulation, and it runs outside simulated
  frames. Test runs never open a microphone unless `SK_NETPLAY_TEST_VOICE=mic` asks for it.

## Testing

| Tool | What it does |
|---|---|
| `tools/netplay-sim.py` | Plays whole races, cups and battles between bot-driven copies of the game through a relay, fast-forwarded (about 70× real time), and checks that every peer stayed in sync and the races ended as expected. Shared-console scenarios drive the menus with scripted buttons and compare every menu frame. 39 scenarios, run side by side: about 10 minutes on a 10-core Mac (see "Keeping the simulator fast"). **Run it after any change to netplay, menu or simulation code.** |
| Determinism harness | `SK_DETERMINISM_RECORD` / `_VERIFY` / `_ROLLBACK=8`: records a race's inputs and per-frame checksums, replays them, or rolls back 8 frames after every frame. See `DeterminismHarness.h`. |
| `tools/netplay-stress.sh` | The rollback stress run on every track and arena, 4 at a time: 16 versus tracks, 4 battle arenas, 4 Grand Prix courses with 2 players and 2 with 4, and 4 races with 5 to 8 players. Give it an AddressSanitizer build (below) to look for memory errors too. |
| `tools/netplay-relay/test_relay.py` | Protocol tests for the relay. |
| `tools/netplay-diff.py` | Names the variables that differ between two peers' start states (`SK_NETPLAY_DUMP=1`) or in a rollback trace. |

The test environment variables (direct peers, lobby bots, simulated lag and loss, scripted leave/restart, bot
drivers, scripted buttons) are listed in `src/port/netplay/Netplay.h`. `SK_FAST=n` runs any of them without frame
pacing or sound output, drawing one frame in n. `SK_TEST_SCREENSHOTS=name@frame,...` saves what the game draws as
PNG files (`TestScreenshots.h`): it reads the game's own framebuffer, so it needs no screen-recording permission.
It also pictures races (netplay or not), but not ImGui windows. Pictures of simulator races were taken with
`--speed 0` (real time).

The tests play no sound, as they run on a computer that people use. The simulator's games run at volume 0
(`gGameMasterVolume` in each settings folder, `write_home()`), which also silences the real-time `lag` scenario; a
scenario can set its own. The browser checks start Chrome with `--mute-audio`: the sound is mixed but not played.

When a session fails, each peer writes `<prefix>.seg<N>.kept.txt`, the checksum breakdown of every frame it still
kept; diffing the peers' files shows the first component that differs. A peer that detects a desync also writes
`<prefix>.desync-p<player>-s<segment>-f<frame>.txt`. The prefix is the test log path (`SK_NETPLAY_LOG`), or else
`netplay/<date-time>` in the game's folder (`~/Library/Application Support/SpaghettiKart/` on macOS), so a desync
between two computers leaves a report on each.

**AddressSanitizer.** A separate build directory, so the normal one is untouched:

```sh
ASAN="-fsanitize=address -fsanitize-recover=address -fno-omit-frame-pointer -mllvm -asan-globals=0"
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL2_DIR="$PWD/build-deps/prefix/lib/cmake/SDL2" \
    -Dlibzip_DIR="$PWD/build-deps/prefix/lib/cmake/libzip" -DCMAKE_C_FLAGS="$ASAN" -DCMAKE_CXX_FLAGS="$ASAN" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address -Wl,-mllvm,-asan-globals=0" -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address"
cmake --build build-asan
tools/netplay-stress.sh build-asan/SpaghettiKart.app/Contents/MacOS/SpaghettiKart
```

Global redzones are off because a snapshot copies the whole sim section, and the script turns container-overflow
checks off at run time because a snapshot copies vector buffers in the sim arena; recovery is on because
libultraship's renderer reports two errors on every run (see Open). Last run (2000 frames each, 8-frame rollback
after every frame): every race in sync, no other errors.

If the game stops responding for 5 s while in a netplay room or race, `HangWatchdog` runs macOS's `sample` tool on it
and writes every thread's call stack to `netplay/<date-time>.hang.txt` in the same folder (wait about 10 s before
force-quitting).

### Keeping the simulator fast (2026-11-19)

By November 2026 a full `netplay-sim.py` run took about 60 minutes: 39 scenarios one after another (3,574 s in the
last such run). The 6 Grand Prix scenarios took half of that, about 4 minutes per whole cup. The owner asked for it
to be shorter without losing meaningful coverage. These were the choices.

**1. Scenarios run side by side.** Nothing is lost. Each scenario already had its own relay port, settings folders
and room file, so the only change is in the runner. `--max-games` limits how many games run at once across all
scenarios: by default the number of cores less 2, at most 8. A scenario with more players than that, such as
`room-8`, waits until it can run by itself. Scenarios that depend on the wall clock rather than on game frames have
`alone=True` and also run by themselves, because a busy CPU would shift them:
- `voice-direct-lost`: the direct connection is blocked after 5 s, by the clock.
- `lag`: real time (`SK_FAST=0`), with the relay delaying packets by milliseconds.

The tradeoffs:
- Each scenario takes longer while others run beside it. Time limits are wall-clock, so a slow Grand Prix (retried
  races) is closer to its limit than before.
- The run keeps most cores busy, so someone playing the game on the same Mac will notice. `--max-games 1` brings
  back one scenario at a time.

**2. Three duplicate scenarios were removed.** Nothing is lost: each was a strict subset of another scenario.

| Removed | Covered by |
|---|---|
| `two-series` (two lobby races in one room) | `demo-first`: the same two lobby races, plus the title-screen demo first |
| `relay-only` (a race with direct connections off) | `voice-relayed`: the same race and the same "no direct connection" check, plus voice |
| `direct-lost` (the direct connection dies mid-race) | `voice-direct-lost`: the same race and checks, plus voice |

**3. The Grand Prix scenarios were trimmed.** A little coverage is lost.
- `gp-2p` plays one cup instead of the Mushroom Cup twice, and plays the Flower Cup, since `console-gp` already plays
  the Mushroom Cup. What's lost: starting a second cup in the same race session, right after a podium, in the
  host-picks-races mode. `console-gp` still checks what follows a podium on the shared console, the mode players use.
  This also makes `gp-2p` less flaky: a race where neither bot finishes in the top 4 is raced again, and two cups'
  worth of retries could reach the old 900 s limit.
- `gp-3p` (3 players, the Flower Cup, host-picks-races mode) was removed. `console-gp-3p` plays a whole 3-player cup
  through the shared console's menus, which is the path players use. What's lost: 3-player Grand Prix in the
  host-picks-races mode, which players can't reach. The Flower Cup's tracks are still raced by `gp-2p`, and every
  track is raced by `tools/netplay-stress.sh`.

**4. `console-too-many` was merged into `console-2p`.** Nothing is lost. `console-2p` now tries 3 players first,
is refused (the room has 2 places), goes back to 2 players and carries on as before
(`CONSOLE_3P_REFUSED_2P_VS`).

**Running side by side exposed one flaky check.** A scenario counts as stuck when no game logs anything for 90 s
(`quiet_limit`). In a 2-player battle nothing was logged between the start and the first elimination, and two bots
can go over a minute without a hit: one battle went 78 s with nothing else running, and under load one went past
90 s. Battles now log each lost balloon (`RaceEvents.cpp`). The 2-player battle scenarios (`battle-2p`,
`mixed-settings`) also have a 180 s limit, because even with balloons logged, a gap of 63 s was seen.

Result: 34 scenarios, and a full run takes about 19 minutes (see the Testing table).

When adding a scenario:
- First check whether an existing scenario can take the new check, through its scripted buttons or another
  `expect`. Each scenario costs at least a minute; a whole cup costs about 4.
- If the check happens early, end the scenario there with `frames=` rather than running the whole race.
- If it depends on the wall clock (a delay in milliseconds, `SK_FAST=0`), mark it `alone=True`.

If a bug turns up that a removed scenario would have caught, bring that scenario back. The removed definitions are in
`tools/netplay-sim.py` at commit `64094a18`.

## Pitfalls found so far

Each of these cost a debugging round.

- **Anything sized or counted 4 that means "the players"** breaks at 5 to 8, silently: the game runs on and looks
  plausible. Found while adopting the 8-player fork (2026-11-20), beyond the fork's own list: menu fades left in
  transition slot 8 (the loops that cleared slots 0-4), versus and Battle end checks that stopped at 4 players, the
  Boo's list of players to steal from (a 4-entry local array: 6 players overflowed the stack, and the game crashed on
  a return address made of floats), the Battle bomb karts of eliminated players, netplay choosing the 4-screen mode
  for 5 players, which spawned player 5 as nothing, and the controllers combined into "any controller" (results and
  menus) and the determinism harness's pads, both still 4. Search for `4` and `< 5` in any per-player code you touch, local
  arrays included, and test with 5 or 6 players, not only 8: half-filled grids find the "every slot is a player" bugs.
  Found later by searching for loops of 4 (2026-11-24): Battle red shells looked for their target among players 1
  to 4; palm trees, DK Jungle's fruit, START, the lightning's flash, the look-behind camera and the reset of the
  camera effects (boost, Thwomp) skipped players or screens 5 to 8; C-right did not toggle the HUD past 4 players;
  the pause menu's position tables had no eight-screen row. Two kinds needed more than a wider loop:
  - A loop that covered the first four karts whatever drove them (palm trees) now covers humans past the fourth
    only, so a Grand Prix's computer players still pass through, as in the original.
  - A field with one bit per camera, with no room for four more: an actor's flags hold which of cameras 1 to 4 drew
    it in their low four bits, and the bits above mean other things. Cameras 5 to 8 have
    `AActor::SeenByCamerasFiveToEight`, and the simulation reads both through `actor_seen_by_a_camera()`.
  Found by a wider search (2026-11-24): Versus for 5 to 8 players had no item table, so every item box gave nothing
  (it now uses the 4-player table, ranks spread over its 4 rows: `RaceManager::GetItemTableRank()`); every racer has
  a Lakitu, and those of players 5 to 8, CPUs in any Grand Prix included, wrote their textures past a 4-player
  `gLakituTextureBuffer`; in the eight-screen split the per-kart updates (tyre spin, colour effects, hit spins, hops,
  smoke and sparks) looped over the human players only, leaving a Grand Prix's CPU karts out, and with 3 or 4 players
  no kart got smoke or sparks in Grand Prix; a `switch` on the player count stopped at 4 and left the race-start
  Lakitu's timing from the last race; the fades had no eight-screen arm, so leaving the results cut to black. The
  AddressSanitizer build cannot see the Lakitu kind of overflow: its global redzones are off (below). The VS and
  Battle results showed one portrait for 5 to 8 players: their positions read past a table with rows for 2 to 4, the
  portraits of players 5 to 8 were item types that did not exist, and their faces overflowed a 4-entry queue; they
  now get a smaller layout of their own, with tallies (`results_portrait_position()` in `menu_items.c`).
  The audio tables in `audio/external.c` look 4 long (`f32 D_800E9DD4[] = { 0.0f, 0.0f, 0.0f, 0.0f }`) but are 8:
  `audio/external.h` declares them with `NUM_PLAYERS` entries before the definitions, which makes the definitions
  that size.

- **libultraship's `Config::GetInt` / `GetFloat` rebuild the whole settings JSON on every call.** Fine once, ruinous
  per frame: `GameplayCVarOverride::KeepPlayerValuesSaved()`, run every frame of a room, made a dozen such calls and
  took 80% of a simulated frame once the 8 controller ports' bindings were in the file (2026-11-20; a stress race
  went from 45 s to 8 s after it read `GetNestedJson()` once instead). Read per-frame settings from one copy.

- **State the menus keep outside the C globals survives a reset to Game Select.** The boot-state restore covers the
  sim sections only. `TrackBrowser_SetTrackFromCup()` remembered the last track it picked and skipped picking it
  again, though a reset loads Mario Raceway behind its back: players whose last console had reached the track screen
  raced Mario Raceway, a player who joined fresh raced the cup's track, and the race desynced on frame 0 (the flaky
  `console-spectator`, 1 run in 4, 2026-11-20). The menu checksum missed it, since the race's track is the World's,
  not the globals'; it now hashes the World's cup and track as well.
- **A screen's own HUD pass draws inside its split screen**, so anything placed by matrix is squeezed into that
  screen's share of the window. The quadrant's item box was made for half the window both ways; in the eight-screen
  grid it came out a third of its size and out of place (2026-11-21). The grid's item boxes are drawn with the rank
  labels over the whole window instead (`draw_8p_item_windows`), as netplay's own view does
  (`render_own_view_overlay`).
- **The player count is not the kart count.** A Grand Prix races 8 karts whatever the player count; counts set from
  `gPlayerCount` for 5 to 8 players left the karts ranked below it off the minimap, humans among them, and let CPUs
  past it drive through the course's hazards (`D_8018D158`, 2026-11-21). With 3 and 4 players it was already 8.

- **A battle arena has no track path**: `gTrackPaths[0]` is null there. Lakitu moved a kart held in Big Donut's lava
  for about 250 frames to the path's first point, and every game crashed on the same frame (2026-11-24, found with
  an idle player in a test); it now leaves the kart where it is. Race code that reads the path must check it first.
- **Draw code changes simulation state** (`init_render_player()`, results-screen menu items, the GP points count,
  which cameras drew an actor: `actor_rendered()`, read by triple green shells and the hit voice lines).
  Never skip drawing a simulated frame. Settings that change drawing change that state too: `gNoCulling` and
  `gDisableLod` are room rules, not each player's own. Two Macs desynced on frame 0 because one had culling off. The
  debug switches that skip drawing (`gDraw*`, `gRenderCollisionMesh`) and debug mode are forced to their defaults in
  a room (`RaceConfig.cpp`). Any CVar the simulation or the menus read belongs in that list. The settings menu greys
  out every widget for a CVar on it while a room session runs (`Menu::MenuDrawItem`).
- **Memory-pool allocations are not in snapshots.** A pool buffer that changes during a race must move to a global
  (`sGPPointsCopy` did).
- **Allocating from inside a simulated frame lands in the sim arena.** Non-simulation code reached from there must
  use `SimHeapPause` (C: `SimHeap_Pause` / `SimHeap_Resume`), or a rollback frees its memory from under it. Found so
  far: the sound log (`play_sound`), the music player (`HMAS_*`), and the music volume setting the race writes at its
  start and when L is pressed. That last one corrupted the settings table on every race with fresh settings; the next
  settings save (opening the menu) then never finished.
  Also resource lookups (2026-11-13): a lookup that misses libultraship's cache allocates on the calling thread, and
  whether it misses depends on what that machine loaded before (each player draws their own view). The peers' arenas
  then drifted apart, and desync recovery, which keeps each player's own heap pointers at the host's offsets, failed
  about half the time ("this game's state is too far from the host's"), more with an archive whose display lists name
  more files. Game code looks resources up through `GameEngine_GetResourceDataByName()` and its siblings
  (`include/align_asset_macro.h`, which `LOAD_ASSET` uses), never libultraship's `ResourceGet*` directly. To find
  such allocations, record the call stacks of arena allocations in `SimArena.cpp`'s `NewImpl` and symbolize them
  with `atos -l 0x100000000` (ASLR is off).
- **Machine-local values fed back into the simulation**: the kart animation once read the frame-buffer index; a voice
  clip variant came from the audio engine's random number. Use simulation state instead.
- **The course-select menu rewrites the track every frame**: apply race settings after the frame's game logic.
- **Attract demos** leave flags behind (`gDemoMode`, `gDemoUseController`), and the baseline can be copied during a
  demo's setup; the race start clears them. The demo only starts from the title screen (`gSkipIntro` 1), so tests need
  that setting to reach it.
- **Pointers in raw-byte hashes**: `RailroadCrossing` overlays a pointer on other fields; the checksum skips it.
  For the browser build, anything hashed must also have the same size there, where pointers and `size_t` are 4
  bytes (a menu index was hashed as `size_t`).
- **Uninitialized locals**: `init_kiwano_fruit()` (DK Jungle) passed unset stack variables as the fruits' position,
  rotation and velocity. Zero on macOS by luck, leftovers in the browser build, which desynced from frame 0. Two
  different builds or compilers can differ the same way; it showed up by replaying a browser race on a Mac.
- **Leftovers from each peer's own history in hashed data**: menu items keep a texture slot and fields that
  `add_menu_item()` never sets, and a cleared item keeps all of them. Hashing the whole `gMenuItems` array made two
  computers with different settings (one starting at the title screen, one playing the intro) desync on frame 0. The
  checksum hashes only active items' state.
- **C++ statics that own heap memory** dangle after a section restore; the baseline restores C sections only.
- **The audio thread must not read CVars** (it raced the main thread's CVar map).
- **Race code that reads the stick in a new way** (`rawStickX`, `rawStickY`) sees the canonical position standing
  for the player's, not the player's own. It cannot desync, but add it to `GetMeaning()` in `CanonicalStick.cpp`.
- **Audio state is per computer and not rolled back**, so simulation code that asks it something (is this sound's
  slot still there?) can answer differently when a frame is simulated again. Track such calls by what the simulation
  passed in (see stops across rollbacks).
- **The determinism harness's re-simulations clear the matrix pool** that the frame's display list, built before them
  and not run yet, points into (AddressSanitizer found it); they get a pool of their own. Netplay draws the shown frame
  after its re-simulations, so it was never affected.
- **After a game crashes, macOS asks at the next start whether to reopen its windows**, and the game waits for the
  answer. Test scripts start the game with `-ApplePersistenceIgnoreState YES`.
- **Menu screen numbers past `COURSE_SELECT_MENU` start a race** when a fade ends (`gMenuSelection >= 0xE`); the
  port's `ONLINE_MENU` is excluded there.
- **Asset paths at odd addresses** are taken for raw texture data by the renderer; declare them aligned
  (`ALIGN_ASSET(2)`, `alignas(8)`).
- **`PLATFORM_IOS` is defined by `<mach-o/loader.h>`** (a platform number), so `!defined(PLATFORM_IOS)` is false in
  any file that includes the Mach-O headers, even on macOS. Use `TARGET_OS_OSX` there.
- **The frame counter is not simulation state** (`gGlobalTimer`, it picks the gfx pool), but the menus' flashing
  colors follow it; a shared console restarts it at each menu session.
- **A reset to Game Select only takes effect at the top of the next game frame** (`CM_RequestReset`). A menu session
  that resets restores the globals from boot at once; if its first frame ran in the same loop, the race on the results
  screen ran on them and crashed (a new console after a player closed their game). Menu frames wait for the reset.
- **A frozen menu session and a race session sharing one GekkoNet adapter** steal each other's packets; the relay
  client gives each tag its own lane.
- **Music cues wrote a simulation global**: after the finish, the music's sound script picks which racer each camera
  shows (`func_800C2274` → `func_8001AAAC` → `D_801646D0`), read by the camera code. It runs once per shown frame
  from this computer's audio, so peers applied it on different frames; `demo-first` desynced after the race once
  netplay played one more sound. Netplay skips those cues: after the finish, the camera stays on the player.

- **`AVAudioEngine`'s voice processing did not work for voice chat**: with its input node's voice processing on, the
  engine failed to start (-10875) unless the mixer was connected to the output by hand, and then its input tap
  delivered nothing. The voice processing audio unit used directly (`VoiceDeviceMac.cpp`) starts and takes the
  format it is given.
- **The build id covers `cmake/`**: after editing a CMake file, a desktop game built before the edit no longer pairs
  with a browser build made after it (`tools/web-check/netplay.mjs` with `NATIVE=` then waits until its timeout).
  Rebuild both.

## Platforms

- **macOS, Apple Silicon**: the only platform built and tested. The app runs on macOS 15 and later; building it so
  needs `tools/macos-build-deps.sh` (see `docs/BUILDING.md`).
- **Linux**: needs a clang build (GCC cannot place globals in sections); untested.
- **Windows**: MSVC cannot place globals in sections, so its builds refuse to start netplay ("build it with clang").
  A clang-cl build would be needed; untested.
- Mixed platforms need the determinism harness run between them first.
- Desync recovery needs macOS: it relies on starting the game without address randomization.

## Open

- Desync recovery between two Macs: tested only between copies on one Mac. The two Macs run different macOS versions,
  whose memory layouts could differ; a failed recovery ends the race as before, with the reason in the log.
- Desync recovery failed about 1 time in 8 on one Mac ("this game's state is too far from the host's"). Since
  resource lookups stay out of the sim arena (2026-11-13), `desync-recover` passed 20 of 20; freed arena blocks are
  still marked like live ones, so a leftover in one could still fail a recovery. A connection silent for over 5 s
  ends the race with no rejoin. `NETPLAY-DESYNC.md` has the causes, the measurements and the direction chosen.

- Direct connections across two real home networks. Routers that use a new public port per destination (symmetric
  NAT, common on mobile networks) stay on the relay.
- Voice chat has not been heard by people yet. To check between two Macs on speakers: nobody hears their own voice
  or the other's game sound come back. The first measurement of that on one Mac failed for an unrelated reason
  (headphones were the output); if the voice processing unit turns out not to cancel the game's own sound, which SDL
  plays separately, or turns it down too much, the game's sound has to go through the unit while voice is on.
- Voice chat: no choice of microphone in the game (the system's selected one is used; with AirPods as the microphone
  macOS switches all sound to call quality), no keyboard shortcut for mute, and it pauses while the game thread stalls
  (a course loading). In a room where some pairs are relayed, those pairs do not hear each other while both hear the
  rest. iPhone Safari is not checked.
- Own view: at the end of a Grand Prix race everyone sees the shared split-screen end (two screens in corners, the
  results and points in the others), not their own view; the other end-of-race banners have not been checked
  visually; the one-player HUD's slide animations and the progress-meter HUD mode are not there.
- With "Hear only my player", another player's engine is heard, but their own voice and item sounds (their camera's)
  are not.
- Shared console: while a menu frame waits for another player's input, the window shows the last frame and the game
  does not respond; with a few frames of ping this is not noticeable, a long freeze would be. The in-room screens have
  been checked in window captures, not yet by people on two Macs.
- Matchmaking, private rooms and code entry have not been tried by people. Typing a code with the stick is not
  covered by scripted buttons (they press buttons, not the stick); the D-pad and C buttons are. The netplay window's
  code entry with a controller, its arrows, and the notice at the bottom of the screen are not seen in a picture yet.
- Not covered by a scenario: the 3-minute limit for an away player, and a matched player going back to looking when
  the room closes before its first console (its host left).
- A player matched while on an offline menu other than the ONLINE screen is taken to Game Select at once, from
  wherever they were (a console start resets the menus). Two matched players can each start an offline race while
  waiting for the other; the room then waits for both.
- The room code on Game Select uses small 10-pixel arrows beside Z LEAVE, the only free place; at the N64's
  resolution they are small.
- 5 to 8 players: controllers 5 to 8 have been tested only with scripted input (`local-6p`), not real gamepads.
  The fork's own between-race points screen and 5 to 8 player menu art were not taken: the results reuse the 3 and
  4 player path, with every player's screen drawn small in players 1 and 2's places (`render_gp_results_screens()`,
  which runs the menus once after them: run after each screen, the results went several steps a frame; and with the
  effect count two screens leave, as menu text stops at 759 effect matrices), and the
  menu art is made at runtime (see [Shared console](#shared-console)). With 6 or 8 players no cell is free for the
  minimap, so the grid has none; with 5 or 7 it is drawn at its usual size in the free cell, placed in window space.
- libultraship's renderer reads past two texture buffers (`ImportTextureI4` at boot, `gfx_load_tlut` on some
  courses), reported by AddressSanitizer on every run; not netplay's, not fixed.

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
| `c6190fbb` | This file |
| `2e7502a6` | The macOS app runs on macOS 15 and later |
| `49da1276` | Fix frame-0 desyncs between computers with different settings |
| `325a9aa5` | Fix the freeze when opening the menu during a race |
| `e1c88b7d` | Direct connections to whoever runs the relay |
| `8a0da7e0` | Restart wrongly stopped sounds, hear other karts' engines, own-view HUD, AddressSanitizer stress run |
| `8b6ca8e9` | One shared console per room, rooms picked from a list |
| `6e3e9ed6`, `7f4dffd9` | Every setting the race or the menus read is a room rule; the settings menu locks them |
| `acaa11f1` | The controller is picked by serial number |
| `3fd986db` | Canonical stick positions |
| `878c93e4` | Other karts glide after a rollback |
| `898fa70f` | Desync recovery |
| `b1c99651` | The default relay is `netkart64.duckdns.org:7777` |
| `20f4c4b9` | Why desync recovery fails, dropped connections (`NETPLAY-DESYNC.md`) |
| `0193e2f0`, `2b3562a1` | The browser build, with rollback; browsers play online (`docs/WEB.md`) |
| `7a3c054b`, `d6babace` | Browsers and desktop games connect directly over WebRTC; HTTPS pages reach the relay with `wss://` |
| `aff2af79` | The ONLINE screen says why rooms are hidden; game files carry the version that made them |
| `cb3610e5` | Resource lookups stay out of the sim arena |
| `e5f4af54` | The overlay's ping split into network and frame wait |
| `a3b0c337` | Voice chat |
| `5d3b9b14` | The ROM is kept, and old game files are made again from it |
| `67805a7c` | A release with the builds on every push to `netplay` |
| `89bdc437` | Public rooms, private rooms and matchmaking |
| `d1aaca81` | START pauses an online race for everyone |
| `0890b1f4` | Browsers connect directly to desktop games in matched rooms |
| `e1205fc9` | Grand Prix for 3 and 4 players, with CPUs in the free karts |
| `71932538`, `64094a18` | Rooms of 8 places; a computer's players counted from its controllers |
| `a5d04ce5`, `3cfa5fb6` | 5 to 8 players online, from the 8-player fork (decision 11): results, own view, stress coverage |
| `cf3b20d9`, `9d29f309` | Controller ports 5 to 8 (decision 12), settings read once per frame, the gamepad name button fix |
| `1d6f41b3` | Every peer races the track the menus picked |
| `4adeb71c` | Game Select and character select cursors for 5 to 8 players |
| `991ed9d6` | Item boxes and minimap for 5 to 8 players |
| `6b63810f` | Character select cursors pass each other above 4 players |
| `b2e3c4f2` | Red shells, palm trees, fruit and pausing for players 5 to 8; the pause menu over the whole window where its tables have no row |
| `30ece859` | Lightning, look-behind camera, camera effects, seen actors and the HUD toggle for players 5 to 8; silent tests |
| `af4e1673` | Versus items, Lakitu textures, CPU karts and fades for 5 to 8 players |
| `b044e480` | VS and Battle results screens for 5 to 8 players, with tallies; Big Donut lava rescue without a track path |
| `8d777c35` | Character select cursors sound only when they move |
| `8c66bca4` | Grand Prix points per kart, not per character |
