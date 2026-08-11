# Eight-player local multiplayer — status

Branch `8p-local`, forked from HarbourMasters/SpaghettiKart at `33817cd11`.

## Where this stands

Five-to-eight-player local multiplayer is playable end to end: pick the count
on the main menu, choose characters on the player-select screen, and race a
live Grand Prix with the human karts split across the screen and the remaining
karts driven by CPU.

Confirmed working this session:

- **Eight controller ports, and they persist.** Settings → Controls exposes all
  eight; each physical pad's port assignment is saved to the config and restored
  on launch, keyed by the device's USB path so several identical controllers
  stay distinct (see the libultraship `controller: persist ...` commit). Players
  two through eight are confirmed drivable on their own pads.
- **The main menu offers 5–8 players.** Scroll past 4P and the count keeps
  climbing to 8, drawn as the 4P icon plus a "N PLAYERS" label.
- **Duplicate characters are allowed.** Two players can pick the same character;
  their karts are told apart by a per-player hue shift on the saturated palette
  entries (`tint_duplicate_kart_palette` in `kart_dma.c`).
- **Character select works for all eight players** — each acts on its own
  controller, with its own tinted, numbered cursor.
- **Grand Prix fills the empty karts with CPUs.** A five-player GP races five
  humans against three CPU opponents on the unused characters.
- **The per-player HUD draws on all eight screens**, positioned from each cell's
  live rectangle (`hud_place_x/y/scale` in `math_util_2.c`), so it tracks a
  window resized mid-race.
- **A stock 2P 150cc Grand Prix no longer crashes under ASan** — a pre-existing
  texture over-read, fixed engine-side (see libultraship notes).

## Resume in one command

```sh
./tools/run-asan.sh          # builds are already made; this just launches
```

**Fastest path to a race:** on the title screen press **P** — a dev shortcut
that jumps straight into a Grand Prix, skipping every menu and auto-assigning
characters. Player count and CC come from CVars `gAutoRacePlayers` (default 5)
and `gAutoRaceCC` (default 2 = 150cc). Set either in
`~/Library/Application Support/SpaghettiKart/spaghettify.cfg.json` under `CVars`.

Manual path: Debug Mode → set the player count → Start; or the normal main-menu
flow.

ASan reports land in `/tmp/mk64-asan/report.<pid>`, stdout in
`/tmp/mk64-asan/stdout.log`. The `[8P-ASSERT]` lines at race start print the
roster, screen mode, and each player's `characterId`; a five-player GP should
now report "5 human, 3 CPU".

After **any** rebuild, run `./tools/asan-fixup.sh` — the bundle rules re-sign
with `--options runtime` every build and the hardened runtime refuses to load
the sanitizer runtime. Without it the app dies at launch with a dyld "different
Team IDs" error that looks nothing like a signing problem.

## The recurring bug class

Nearly every defect has been the same shape: **something sized for four, indexed
by a value that now reaches eight.** Check this list before theorising:

1. Integer literals standing in for a constant (`77`, `157`, `237`)
2. Array subscripts, which are not branches and never show up grepping `switch`
3. Index *names* that disagree with loop *bounds*
4. Function-local statics
5. Incomplete type lists in a sweep
6. Validation clamps (`if (n >= 5) n = 4`)
7. **Sized `extern` disagreeing with its definition** — header wins at the use
   site, so `ARRAY_COUNT` lies (capped the player picker at four)
8. **Unsized `extern`** — the definition wins, so widening `NUM_PLAYERS` never
   reaches the table (the audio voice tables; and `gPlayerModeSelection`, whose
   four-entry definition crashed the mode column at five players)
9. Second array dimensions
10. **A sentinel slot whose index equals the old player count.** `gControllers[4]`
    was the combined-input aggregate; index 4 of the transition tables means "the
    whole screen". Both silently became player five.
11. **Per-screen arrays *inside* a struct.** `animGroupSelector[4]` sat on top of
    `characterId`. **Invisible to AddressSanitizer** — the write stays in one
    `Player` object, so there is no redzone. Found only by reading field sizes.
12. **A switch whose arms are named constants meeting a contiguous id run.** The
    eight-screen render ids are a range (12–19), not per-screen constants, so the
    HUD/menu dispatch switches needed a range arm, not eight more cases.
13. **Count-ladders** (`case 1: … case 4:`) that silently do nothing past four —
    replace with a loop bounded by the count and `NUM_PLAYERS`.
14. **A table indexed by screen mode or player count with no eighth-screen row.**
    `D_800E7188` (character staging) is indexed `screenMode * 4`; `SCREEN_MODE_8P`
    (mode 4) read past its end and crashed a five-player start. Gets its own row.

Two engine-level over-reads this session were **not** player-count bugs but the
same "descriptor claims more than the buffer holds" family: a display list can
hand the graphics backend a raw pointer into texture memory with no resource
handle, and the importer then trusts an over-claiming size. Both are benign
without ASan (they read mapped heap) and pre-existing upstream. Fixed by
bounding to the real allocation via a texture-buffer registry.

## Tooling notes

- `tools/run-asan.sh` — launcher with reliable report capture (`log_path`,
  `abort_on_error=0`).
- `tools/asan-fixup.sh` — re-signs without the hardened runtime. Required after
  every build.
- `tools/asan-suppressions.txt` — one live entry, for a latent `prism` shader
  bug, pre-existing upstream.
- `src/debug/assert_8p.c` — runtime invariants plus the race-start roster dump.

`interceptor_via_fun` suppressions only work for the `wrap_memcpy` interceptor.
A frame #0 of `__asan_memcpy` is the compiler-inserted copy inside instrumented
code and no suppression can touch it. Also: macOS's `__asan_locate_address`
aborts internally on some inputs — do not use it to bound reads.

## Local libultraship changes

`libultraship` is a submodule. Six commits exist **only in the local clone** and
are not pushed anywhere. A `git submodule update` against the upstream URL cannot
fetch them — they would be lost. All are worth sending to Kenix3/libultraship.

| Commit | What |
|---|---|
| `fad75e7d` | Bound TLUT copies by the palette resource's real size. |
| `428125fa` | `LUS_MAX_PORTS` for the ControlDeck port count. |
| `ec0b53c6` | Default the per-port ignore list for every port, and only once. |
| `a74cdfac` | Distinct ImGui ids for identically named devices. |
| `1c73c4d8` | Bound raw-pointer texture reads via a live buffer registry. |
| `57814df4` | Persist per-port device assignments across runs (by USB path). |

`LUS_MAX_PORTS=8` is set PUBLIC in the root `CMakeLists.txt`, with a
`_Static_assert` in `main.c` tying it to `NUM_PLAYERS`.

## Known-outstanding

- **Viewport reflow (in progress).** 5–8 players all render the fixed 4×2 grid
  of eight cells, so a five-player race shows three CPU spectator cells. The next
  change makes the grid track the human count — 5–6 → 3×2, 7–8 → 4×2 — and fills
  the odd-count spare cell with a dedicated minimap. This touches the split-
  screen core: `screen_grid.h` dimensions, `set_screen` cell centres, the
  viewport-size state machine in `race_logic.c`, `spawn_multiplayer_cameras`, the
  dividers in `func_802A4300`, the render loop in `main.c`, and a new minimap-in-
  cell render path.
- `func_802A7728` / `func_802A7940` grab a 128-wide framebuffer region into a
  narrower cell.
- Two item-centring sites in `render_objects.c` still need a screen id plumbed
  through the generic draw helpers.
- A top-row viewport was once seen rendering mostly black; not observed recently,
  watch for it.

## Things that are settled, so do not re-derive them

- `NUM_PLAYERS 8`, `Player gPlayers[8]`, `gControllerFive..Eight`,
  `gItemWindowObjectByPlayerId[8]` all existed in vanilla. Grand Prix already
  spawned eight karts (one human, seven CPU) — this is a CPU→human conversion
  plus presentation work, not a simulation extension.
- `NUM_CAMERAS = NUM_PLAYERS * 2 + 4 = 20`. **A camera id is not a player id** —
  use `camera_owner_player()` in `code_80005FD0.c`.
- Ten arrays in `audio/external.c` are defined unsized with four initialisers but
  declared `[NUM_PLAYERS]` in the header. These are **fine** — a sized
  declaration in scope completes the type and the compiler allocates eight. Do
  not "fix" them.
- The centre divider of any split is at 320-space x=160 at every aspect ratio,
  which is why 2P/3P/4P dividers use literals safely. Only non-central boundaries
  need the aspect math.
- The eight-screen HUD stores no coordinates; placement is derived from each
  cell's live rectangle at draw time. Do not reintroduce a stored layout — two
  sources drift apart.
