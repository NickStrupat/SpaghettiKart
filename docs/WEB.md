# Browser build

SpaghettiKart compiled to WebAssembly with Emscripten, on the `wasm` branch: the game runs in a browser tab. This
file records how to build and serve it, what was changed to make it run, and what is still open.

## Status

- **Works**: start-up, the menus, races with CPUs, sound, the keyboard, the settings menu (Esc), frame interpolation
  (Settings > Graphics > Current FPS), settings and saves kept between visits. Checked in Chrome on an Apple Silicon
  Mac, by `tools/web-serve.py` and a headless Chrome driven over its debugging protocol.
- **Not checked**: gamepads (SDL2's browser support should cover them), Firefox, Safari, phones, split screen.
- **Rollback works, without a network** (step 1 of online play, see [Online play](#online-play)): the determinism
  harness's rollback stress run passes on all 24 races of `tools/netplay-stress.sh`, and the same races recorded in
  the browser replay frame for frame on the macOS build.
- **Online play works between browsers**, and with desktop players through the relay (step 2, see
  [Online play](#online-play)). It needs a relay new enough to accept WebSockets; the public one does not yet.
- **Does not work**: making `mk64.o2r` from a ROM in the browser; direct connections between a browser and a desktop
  game (they play through the relay).

## Building

Needs Emscripten (`brew install emscripten`; 7.0.0 was used), CMake and Ninja, and the two archives a desktop build
makes, `mk64.o2r` and `spaghetti.o2r`, in the repository's root (see `docs/BUILDING.md`).

```sh
emcmake cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-web                      # build-web/web/: index.html, index.js, index.wasm and the archives
python3 tools/web-serve.py                   # http://localhost:8064/
```

The first configure downloads the libraries listed in `cmake/dependencies/emscripten.cmake`, and the first build
makes Emscripten's SDL2 and zlib ports. `-DCMAKE_BUILD_TYPE=RelWithDebInfo` gives function names in the browser's
stack traces and Emscripten's run-time checks, in a 92 MB `index.wasm` instead of 8 MB.

## Serving it

- The page must be "cross-origin isolated", because the game uses threads: the server has to send
  `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp` with it.
  `tools/web-serve.py` does; a host that cannot set headers (GitHub Pages) cannot serve the game as it is.
- Anywhere but `localhost`, browsers also want HTTPS for this.
- `mk64.o2r` holds Nintendo's data and must not be put on a public server. Without it next to the page, the page
  asks the player for their own file and keeps it in the browser's storage. `spaghetti.o2r` is the port's own and
  has to be there.

## Testing

`tools/web-check/check.mjs` loads the page in a headless Chrome of its own (not the browser anyone has open),
prints the page's console, presses keys and saves pictures of the page. Its header lists the steps. This starts a
race on Luigi Raceway from a fresh profile and holds the accelerator:

```sh
cd tools/web-check && npm install && cd ../..        # once
python3 tools/web-serve.py &
node tools/web-check/check.mjs http://127.0.0.1:8064/ wait:11000 key:Enter wait:2500 \
    key:Enter wait:1500 key:Enter wait:1500 key:Enter wait:1500 key:Enter wait:1500 key:Enter wait:1500 \
    key:Enter wait:1500 key:Enter wait:1500 key:Enter wait:1500 key:Enter wait:1500 key:Enter wait:1500 \
    key:Enter wait:8000 hold:Shift wait:10000 shot:/tmp/race.png
```

It exits with 1 when the page ends up showing a message instead of the game. There is no sound to hear and no
gamepad in a headless browser: those need a person.

The game's test settings (`SK_FAST`, `SK_DETERMINISM_...`, see `src/port/netplay/DeterminismHarness.h`) go in the
page's address: `index.html?SK_FAST=10&SK_DETERMINISM_RECORD=/tmp/race.skdt`. When a harness run ends, the page
stops and the console says `[Web] Finished with exit code N`. `/tmp` is the game's file system, in memory.

`tools/web-check/stress.mjs` is `tools/netplay-stress.sh` for the browser: 24 races, each rolled back 8 frames after
every frame, about 10 s each. With `NATIVE=build-cmake/SpaghettiKart.app/Contents/MacOS/SpaghettiKart` it also
replays each race on the desktop build, which shows whether the two compute the same game:

```sh
NATIVE=build-cmake/SpaghettiKart.app/Contents/MacOS/SpaghettiKart node tools/web-check/stress.mjs
```

`tools/web-check/netplay.mjs` is a `tools/netplay-sim.py` scenario for the browser: a local relay
(`build-relay/netplay-relay`) and one headless Chrome per player, each its own process; bots race. Its header lists
the options (players, mode, relay only, a direct connection that fails, a desktop game as the last player):

```sh
node tools/web-check/netplay.mjs                                   # 2 browsers, direct over WebRTC
PLAYERS=4 MODE=battle TRACK=mk:block_fort node tools/web-check/netplay.mjs
NATIVE=build-cmake/SpaghettiKart.app/Contents/MacOS/SpaghettiKart SPEED=0 node tools/web-check/netplay.mjs
```

With `SK_FAST` (the default `SPEED=10`), rollbacks and stalls are far above the desktop's: a page receives network
messages only between calls from the browser, and fast mode runs many game frames in one. At normal speed
(`SPEED=0`, a full race in about 2.5 minutes) a 2-player race had no stalls and rollbacks of 1 or 2 frames.

## How it differs from the desktop game

| Part | Desktop | Browser |
| --- | --- | --- |
| Main loop | `while` loop in `main()` | the browser calls `WebFrame()` (`src/port/Game.cpp`) once per screen refresh |
| Frame pacing | libultraship sleeps between pictures | `WebFrame()` skips refreshes until a picture is due |
| Interpolated pictures | all drawn inside the game frame | one per refresh (`GameEngine::DrawPendingFrame()`) |
| Graphics | Metal, OpenGL or Direct3D | WebGL 2, through libultraship's OpenGL ES path |
| Files | the settings folder | a folder in memory; `src/port/web/shell.html` copies changes to IndexedDB |
| `mk64.o2r` | made from a ROM by Torch | loaded by the page; Torch is not built |
| Online play | UDP to the relay; to other players directly, punching through routers | a WebSocket to the relay; WebRTC to other browsers |

Everything specific to the browser is behind `__EMSCRIPTEN__` in the code and `SPAGHETTIKART_WEB` in CMake
(`cmake/web/`, `cmake/dependencies/emscripten.cmake`). The page itself is `src/port/web/shell.html`.

## Pitfalls found so far

- **libultraship skips textures at addresses below `0x10000000`** (`gfx_set_timg_handler_rdp`): it takes them for
  N64 addresses that no segment resolved. A 64-bit program's pointers are far above that; WebAssembly's start at 0,
  so the karts, whose palettes are in the game's own memory, were not drawn. The link puts all of the game's memory
  above 256 MB instead (`-sGLOBAL_BASE`, see `cmake/web/WebTarget.cmake`).
- **A call through a wrong declaration traps.** `framebuffer_effects.c` declared `gfx_create_framebuffer()` with
  five arguments where libultraship takes six. On the desktop the sixth was whatever the register held; in
  WebAssembly the call stops the program. The linker warns about these ("function signature mismatch"): fix every
  one.
- **The page's main thread must not wait.** Sleeping there is a busy loop that also holds up the browser, and a
  thread only starts once the main thread returns to the browser. Hence the pacing in `WebFrame()`, and the workers
  made before `main()` (`-sPTHREAD_POOL_SIZE`). The game still waits for its sound thread once per frame, which
  Emscripten warns about at start-up; it is a short wait.
- **Only the last picture drawn in one call from the browser is shown**, so a game frame cannot draw its
  interpolated pictures in a row as on the desktop.
- **libultraship is a submodule that this fork does not own.** What it needs to compile with Emscripten is in
  `src/port/web/LibultrashipShim.h`, which the build includes into each of its files, rather than in the submodule.
- **The sound mixer uses SSE2 intrinsics.** Emscripten translates them (`-msimd128 -msse2`), except the MMX ones.
- **Python's `http.server` looks up the computer's network name** when it starts, which can take a minute;
  `tools/web-serve.py` skips that.
- **Clang's WebAssembly backend drops the section names of `#pragma clang section`** (`SimSection.h`): it keeps a
  file's simulation globals together, initialized or not, but in a segment with an empty name, so the linker neither
  groups them across files nor marks where they are. The build compiles through `tools/web/sim-segments.py`, which
  names that segment `sim_data` (C) or `sim_cxx_data` (C++) in each object file; `SimState.cpp` then finds them with
  the linker's `__start_`/`__stop_` symbols. There is no separate bss part. An explicit `__attribute__((section))`
  would work without the tool, but on every global.
- **Pointers and `size_t` are 4 bytes.** Anything hashed for the checksum must have the same size on both sides: a
  `size_t` menu index made every frame's checksum differ from the Mac's while every part of it matched.
- **Two headless Chromes on one computer cannot connect with WebRTC** unless started with
  `--disable-features=WebRtcHideLocalIpsWithMdns`: Chrome hides each one's local addresses behind `.local` names the
  other cannot look up (`tools/web-check/netplay.mjs` passes it). Players on different computers do not need it.
- **Uninitialized memory differs.** The Mac's happened to be zero where the browser's held leftovers: see the
  kiwano pitfall in `docs/NETPLAY.md`.

## Open

- **Online play**, below.
- **Making `mk64.o2r` in the browser** from the player's ROM. Torch has Emscripten support of its own.
- **Size.** The 256 MB memory pool (`sMemoryPool` in `src/racing/memory.c`) and the unused 256 MB below it make the
  page reserve 1 GB of address space, more than phones allow.
- **Threads.** Without them the page would not need the two headers. libultraship's resource pool and logger, and
  the game's sound thread, would have to run on the main thread.
- **Touch controls**, and a way to add mods (the page has no way to put files into the game's folder).

## Online play

The plan, in three steps:

1. **Rollback in the browser, without a network. Done.** Snapshots hold 411 KB of simulation globals (C and C++)
   and the sim arena; the stress run and the browser-to-Mac replay pass on every race (see [Testing](#testing)). The
   replay passing also means a browser and a Mac compute the same game, floating point included, on these races.
2. **Browser to browser. Done.** `RelayClient` moves its packets through a `RelayLink` (`src/port/netplay/RelayLink.h`):
   UDP on the desktop (`RelayLinkUdp.cpp`, the code that was in `RelayClient.cpp`), and in a browser
   (`RelayLinkWeb.cpp`) a WebSocket to the relay plus a WebRTC data channel to each other browser, unordered and
   without retransmits (`ordered: false, maxRetransmits: 0`, the nearest thing to UDP; `src/port/web/WebRtcLink.js`).
   Lobby, NetSession, GekkoNet and rollback are unchanged.
   - The relay accepts WebSockets on TCP, on the same port number as its UDP (`tools/netplay-relay/WebSocket.h`), and
     each binary message is one datagram of the usual protocol. So the relay barely knows the difference, and browser
     and desktop players can share a room.
   - WebRTC's setup messages (offer, answer, candidates) travel as a third payload channel through the relay
     (`kSignalChannel` in `RelayClient.cpp`), in parts when longer than one payload. The lower slot makes the offer.
     A public STUN server (`stun.l.google.com`, `kIceServers` in `RelayLinkWeb.cpp`) gives each browser its public
     address. Players whose routers still cannot connect stay on the relay; there is no TURN server.
   - A ping over the channel every 500 ms gives the round trip (for the automatic input delay) and notices a lost
     connection after 1.5 s, as on the desktop.
   - Checked with `tools/web-check/netplay.mjs`: 2 and 4 browsers, VS, Battle and Grand Prix, relay only, and a direct
     connection blocked mid-race; all in sync.
3. **Browser to desktop.** Already works through the relay (checked: a browser host and a desktop guest, a full race
   in sync). What is missing is a direct connection between the two: the desktop game would need WebRTC
   (libdatachannel). Desync recovery between the two would not work either: it copies raw snapshots, pointers
   included, and pointers are 4 bytes in the browser.

Still open for playing online from a browser:

- **The public relay** (`netkart64.duckdns.org:7777`) needs the new relay, run with `-p 7777:7777/tcp` as well.
- **A page served over HTTPS** may only open `wss://` WebSockets, so the relay would need a proxy that adds TLS in
  front of its TCP port (the relay address then starts with `wss://`). Pages on `http://localhost` can use `ws://`.
- Network messages reach a page only between frames, so a WebRTC round trip on one computer measures about 30 ms
  instead of 1. Rollback absorbs it; receiving in a worker would not have to wait.

Desync recovery between browsers needs no fixed addresses: every copy of the page has the same memory layout.
