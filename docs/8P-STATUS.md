# Eight-player local multiplayer — status

Branch `8p-local`, forked from HarbourMasters/SpaghettiKart at `33817cd11`.

## Where this stands

Eight viewports render a live Grand Prix race with eight distinct characters,
correct portraits, working karts, and a 4×2 grid that tracks window resizing.
Settings → Controls now exposes eight controller ports.

**Player 1 is confirmed drivable. Players 2–8 have ports but have not been
verified end to end** — the ports exist and `read_controllers()` fills all eight,
but whether each kart actually reads its own port has not been tested with a
second physical controller. That is the first thing to check on resume.

## Resume in one command

```sh
./tools/run-asan.sh          # builds are already made; this just launches
```

Debug Mode → D-pad down ×3 → right to `8players` → Start.

ASan reports land in `/tmp/mk64-asan/report.<pid>`, stdout in
`/tmp/mk64-asan/stdout.log`. Look for the `[8P-ASSERT]` lines at race start —
they print the roster, the screen mode, and each player's `characterId`.

After **any** rebuild, run `./tools/asan-fixup.sh`. The bundle rules re-sign with
`--options runtime` every build, and the hardened runtime refuses to load the
sanitizer runtime. The script strips that flag; without it the app dies at launch
with a dyld "different Team IDs" error that looks nothing like a signing problem.

## The recurring bug class

Nearly every defect has been the same shape: **something sized for four,
indexed by a value that now reaches eight.** It has appeared in eleven distinct
disguises so far. When something breaks, check this list before theorising:

1. Integer literals standing in for a constant (`77`, `157`, `237`)
2. Array subscripts, which are not branches and so never show up when grepping
   for `switch`
3. Index *names* that disagree with loop *bounds*
4. Function-local statics
5. Incomplete type lists in a sweep
6. Validation clamps (`if (n >= 5) n = 4`)
7. **Sized `extern` disagreeing with its definition** — the header wins at the
   use site, so `ARRAY_COUNT` lies (capped the player picker at four)
8. **Unsized `extern`** — the definition wins, so widening `NUM_PLAYERS` never
   reaches the table (the audio voice-limit tables)
9. Second array dimensions
10. **A sentinel slot whose index equals the old player count.** Twice:
    `gControllers[4]` was the combined-input aggregate, and index 4 of the
    transition tables means "the whole screen". Both silently became player five.
11. **Per-screen arrays *inside* a struct.** `animGroupSelector[4]` sat directly
    on top of `characterId`; writing screen 4 changed a player's character.

Number 11 is the important one for future work: it is **invisible to
AddressSanitizer**. The write stays inside one `Player` object, so there is no
redzone to trip. A clean ASan run says nothing about this class — it has to be
found by reading field sizes.

## Tooling notes

- `tools/run-asan.sh` — launcher with reliable report capture. Uses `log_path`
  so reports survive however the process ends, and `abort_on_error=0` so macOS
  does not raise a crash dialog over the more useful ASan text.
- `tools/asan-fixup.sh` — re-signs without the hardened runtime. Required after
  every build.
- `tools/asan-suppressions.txt` — one live entry, for a genuine latent bug in
  the `prism` shader preprocessor (`parse_header` reads `m_lines[0]` after
  erasing the first element). Pre-existing upstream, unrelated to player count.
- `src/debug/assert_8p.c` — runtime invariants plus the race-start dump. Hooked
  into `start_race()` and `update_race_position_data()`.

`interceptor_via_fun` suppressions only work for the `wrap_memcpy` interceptor.
If a report's frame #0 is `__asan_memcpy`, that is the compiler-inserted copy
inside instrumented code and **no suppression can touch it** — fix the bug or
mark the function `no_sanitize`.

## Local libultraship changes

`libultraship` is a submodule. Two commits exist **only in the local clone** and
are not pushed anywhere:

| Commit | What |
|---|---|
| `fad75e7d` | Bound TLUT copies by the palette resource's real size. `GfxDpLoadTlut` copied 512 bytes for every CI8 palette; at least one asset is 464, so it over-read by 48 bytes and aborted every ASan run. |
| `428125fa` | `LUS_MAX_PORTS` for the ControlDeck port count, defaulting to `MAXCONTROLLERS`. |

Both are worth sending upstream to Kenix3/libultraship. A `git submodule update`
against the upstream URL cannot fetch them — they would be lost.

`LUS_MAX_PORTS=8` is set in the root `CMakeLists.txt`, PUBLIC, so the game sees
the same value. `main.c` has a `_Static_assert` tying it to `NUM_PLAYERS`,
because `osContGetReadData` memsets `sizeof(OSContPad) * LUS_MAX_PORTS` into an
array the *game* declares — the two size one buffer from opposite ends.

## Known-outstanding

- **Players 2–8 not verified drivable.** Ports exist; end-to-end untested.
- **HUD position icons bunch toward the centre.** They are still placed for a
  2×2 grid. Same family as the divider bug fixed in `func_802A4300`: a
  *horizontal* element only needs its extent widened, a *vertical* or
  *positioned* one needs its coordinate remapped through
  `OTRGetDimensionFromLeftEdge`. Only the first had been done.
- **One viewport occasionally renders mostly black** (seen in the top row).
  Viewport geometry for that cell specifically.
- `func_802A7728` / `func_802A7940` grab a 128-wide framebuffer region into an
  80-wide cell.
- Two item-centring sites in `render_objects.c` still need a screen id plumbed
  through the generic draw helpers.
- ~19 switch ladders without 8P arms; most are correct by default, none known
  to be reached.

## Things that are settled, so do not re-derive them

- `NUM_PLAYERS 8`, `Player gPlayers[8]`, `gControllerFive..Eight`,
  `gItemWindowObjectByPlayerId[8]` all existed in vanilla. Grand Prix already
  spawned eight karts. This is a CPU→human conversion plus presentation work,
  not a simulation extension.
- `NUM_CAMERAS = NUM_PLAYERS * 2 + 4 = 20`. **A camera id is not a player id** —
  `spawn_multiplayer_cameras` makes two cameras per screen. Use
  `camera_owner_player()` in `code_80005FD0.c`.
- Ten arrays in `audio/external.c` are defined unsized with four initialisers
  but declared `[NUM_PLAYERS]` in the header. These are **fine** — a sized
  declaration in scope completes the type and the compiler allocates eight,
  zero-filling. Verified by compiling the pattern. Do not "fix" them.
- The centre divider of any split is at 320-space x=160 at *every* aspect ratio,
  which is why 2P and 3P/4P dividers use literals safely. Only non-central
  boundaries need the aspect math.
