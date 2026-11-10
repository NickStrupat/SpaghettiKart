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
- **Online play works between browsers and with desktop players**, connecting directly over WebRTC (steps 2 and 3,
  see [Online play](#online-play)). It needs a relay new enough to accept WebSockets, as the public one does since
  2026-11-10.
- **The player's ROM becomes `mk64.o2r` in the browser** (`.z64`, `.n64` or `.v64`), in about 20 seconds on the
  first visit; see [The game's archive](#the-games-archive).
- **Does not work**: desync recovery when a browser and a desktop game share a room.

## Building

Needs Emscripten (`brew install emscripten`; 7.0.0 was used), CMake and Ninja, and the two archives a desktop build
makes, `spaghetti.o2r` and (optionally, to skip asking for the ROM on a local page) `mk64.o2r`, in the repository's
root (see `docs/BUILDING.md`).

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
  asks the player for their ROM and makes it (below). `spaghetti.o2r` is the port's own and has to be there.
- **The public page** is https://netkart64.duckdns.org/, on the public relay's computer since 2026-11-10.
  `tools/web-deploy.sh` copies `build-web/web/` there (never `mk64.o2r`) and runs nginx with
  `tools/web-host/nginx.conf`. Traefik in front adds HTTPS and routes requests for that name by kind: WebSockets to
  the relay, everything else to nginx; so the page's default relay address is its own server.
  `node tools/web-check/check.mjs https://netkart64.duckdns.org/ pick:<ROM> wait:35000` checks it.

## The game's archive

On the desktop, the game's first start makes `mk64.o2r` from the player's ROM with Torch (`src/port/GameExtractor.cpp`).
The page does the same with Torch compiled to WebAssembly on its own (`tools/web/extractor/`), built by the
`WebExtractor` step of `cmake/web/WebTarget.cmake`: `extractor.wasm` (1.5 MB), and `extractor.data` (8 MB, Torch's
asset descriptions from `config.yml`, `yamls/`, `include/defines.h` and `meta/mods.toml`). The page loads them only
when it has no archive.

- `src/port/web/shell.html` puts the ROM in `.z64` byte order and checks its SHA-1 against the US version's; then
  `src/port/web/ExtractWorker.js` runs Torch on a worker, so the page keeps drawing its message. The archive is kept
  in the browser's storage, as before; the ROM is not. An `mk64.o2r` from the desktop version is accepted too.
- The archive is not byte for byte the desktop's (the zip's dates differ), but every file's name, size and CRC
  match, which is what netplay's content check compares (`NetplayIdentity.h`).
- Torch is a submodule this fork does not own: `tools/web/extractor/CMakeLists.txt` works around what it needs here
  (a missing `<cstdlib>`, a thread check its link flags break).

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
| `mk64.o2r` | made from a ROM by Torch | made from a ROM by Torch in WebAssembly, on a worker |
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
   - Who offers, and the peer logic both sides share, are in `WebRtcPeers.cpp` since step 3.
   - The relay accepts WebSockets on TCP, on the same port number as its UDP (`tools/netplay-relay/WebSocket.h`), and
     each binary message is one datagram of the usual protocol. So the relay barely knows the difference, and browser
     and desktop players can share a room.
   - WebRTC's setup messages (offer, answer, candidates) travel as a third payload channel through the relay
     (`kSignalChannel` in `RelayClient.cpp`), in parts when longer than one payload. Between browsers the lower slot
     makes the offer. A public STUN server (`stun.l.google.com`, `kIceServers` in `WebRtcPeers.cpp`) gives each side
     its public address. Players whose routers still cannot connect stay on the relay; there is no TURN server.
   - A ping over the channel every 500 ms gives the round trip (for the automatic input delay) and notices a lost
     connection after 1.5 s, as on the desktop.
   - Checked with `tools/web-check/netplay.mjs`: 2 and 4 browsers, VS, Battle and Grand Prix, relay only, and a direct
     connection blocked 3 s into a race, which then went on through the relay; all in sync.
3. **Browser to desktop. Done.** The desktop game has WebRTC too: libdatachannel 0.24 with mbedTLS, built from source
   with the game, statically (`cmake/dependencies/common.cmake`; mbedTLS needs DTLS-SRTP turned on,
   `cmake/mbedtls/mbedtls_user_config.h`). `WebRtcTransportDesktop.cpp` gives it the same small API the browser's
   `WebRtcLink.js` has (`WebRtcTransport.h`), and `RelayLinkUdp.cpp` runs `WebRtcPeers` next to its UDP punching.
   - The relay protocol did not change. A desktop game never offers: it keeps punching to other desktops, which
     never start WebRTC, and answers browsers. A browser offers to every member the relay shows an address for: only
     desktop games that share their addresses. A desktop game with direct connections off ignores offers, so its
     address stays private, as with punching.
   - Checked with `tools/web-check/netplay.mjs` (`NATIVE=`, `NATIVE_PLAYER=`): a browser host with a desktop guest, a
     desktop host with a browser guest, two browsers and a desktop game in one room (every pair direct), and the
     desktop's direct connection blocked 3 s into a race; all in sync. `netplay-sim.py`: 23 of 23, and desktop games
     with no browser in the room start no WebRTC at all.
   - Desync recovery would not work between the two: it copies raw snapshots, pointers included, and pointers are 4
     bytes in the browser.

Still open for playing online from a browser:

- **Pages served over HTTPS** may only open `wss://` WebSockets. On such a page the game turns the relay address
  `host:port` into `wss://host/`: a proxy that adds TLS on the standard port of the relay's host
  (`tools/netplay-relay/README.md`). The public relay has one since 2026-11-10 (Traefik on its computer), so
  `wss://netkart64.duckdns.org/` works and the default relay address works from HTTPS pages. Pages on
  `http://localhost` use `ws://host:port/` directly. `RELAY_ADDRESS=wss://netkart64.duckdns.org/` makes
  `tools/web-check/netplay.mjs` race through it.
- Network messages reach a page only between frames, so a WebRTC round trip on one computer measures about 30 ms
  instead of 1. Rollback absorbs it; receiving in a worker would not have to wait.

Desync recovery between browsers needs no fixed addresses: every copy of the page has the same memory layout.
