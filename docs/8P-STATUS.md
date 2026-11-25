# Eight-player local multiplayer — status

> Kept as the [8-player fork](https://github.com/chrisgilpin/MK64-8p-local) wrote it, for its list of bug classes.
> Parts are out of date in this repository, where `docs/NETPLAY.md` is current: the grid follows the human count with
> a minimap in a free cell, the item boxes draw over the whole window, Game Select reads 5P GAME to 8P GAME instead of
> a "N PLAYERS" label, players 5 to 8 have hue-turned cursors, and above 4 players character cursors pass each other.

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
  controller, with its own tinted, numbered cursor. A cursor sounds only when it
  moves (with spots shared, the original replayed the move sound every frame).
- **Grand Prix fills the empty karts with CPUs.** A five-player GP races five
  humans against three CPU opponents on the unused characters.
- **The per-player HUD draws on all eight screens**, positioned from each cell's
  live rectangle (`hud_place_x/y/scale` in `math_util_2.c`), so it tracks a
  window resized mid-race.
- **Races and battles treat players five to eight as the first four** (2026-11-24):
  red shells target them, palm trees stop them, DK Jungle throws fruit at them,
  the lightning flashes their screens, they can pause (the pause menu is drawn
  over the whole window on the eight-screen split), their look-behind camera and
  camera effects work, and C-right toggles the HUD. Actors remember which of
  cameras five to eight drew them (`AActor::SeenByCamerasFiveToEight`). Checked
  by the simulator (`pause-6p`) and the rollback stress run, not yet by hand.
- **Versus for five to eight players hands out items** from the four-player
  table, the ranks spread over its four rows (`RaceManager::GetItemTableRank()`):
  before, every item box gave nothing. A Grand Prix's CPU karts get their tyre
  spin, colour effects, hit spins, hops, smoke and sparks on the eight-screen
  split, and the split fades in and out. (2026-11-24)
- **The VS and Battle results show five to eight players**: smaller portraits
  in two rows of up to four, each with its tallies to the right (1st, 2nd and
  3rd places in VS, wins in Battle), kept through a soft reset in the NMI
  buffer's last 32 bytes. Before, one portrait showed in a corner. Checked with
  pictures of 5 to 8 player races (2026-11-24); see `results_portrait_position()`
  in `menu_items.c`.
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
   four-entry definition crashed the mode column at five players). The reverse
   holds too: a sized `extern` in scope at an unsized definition makes the
   definition that size, so `f32 D_800E9DD4[] = { 0, 0, 0, 0 }` in
   `audio/external.c` is 8 long. Check the header before "fixing" one.
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
    replace with a loop bounded by the count and `NUM_PLAYERS`. Watch for the
    ones that set state: past four they keep the last race's values (the
    race-start Lakitu timing in `func_8006FA94()`), or set nothing at all (the
    Versus item table in `RaceManager::SetItemTables()`). A screen-mode `switch`
    with no `SCREEN_MODE_8P` arm and no `default` draws nothing on the split (the
    fades in `menu_items.c`).
14. **A table indexed by screen mode or player count with no eighth-screen row.**
    `D_800E7188` (character staging) is indexed `screenMode * 4`; `SCREEN_MODE_8P`
    (mode 4) read past its end and crashed a five-player start. Gets its own row,
    or, where a row makes no sense (the pause menu in a cell too small for it),
    the one-player layout over the whole window.
15. **Plain `for (i = 0; i < 4; i++)` over players or screens.** Grep the
    loops, not only the arrays: red shell targets, palm trees, DK fruit, START,
    the lightning flash, camera switches and camera-effect resets all stopped at
    four with arrays already sized eight. Where the original covered the first
    four karts whatever drove them, widen it for humans only, so CPUs behave as
    before.
16. **One bit per camera with no room for more.** An actor's flags keep which of
    cameras one to four drew it in bits 0–3, and bits 4–7 are taken. Cameras
    five to eight get their own field on `AActor`, read together through
    `actor_seen_by_a_camera()`.
17. **Loops bounded by the human count where the karts are meant.** In a Grand
    Prix the karts after the human players are CPUs: an eight-screen arm that
    loops to `gPlayerCountSelection1` leaves them out. Loop to `NUM_PLAYERS` and
    let the callee skip karts that do not exist.
18. **A global overflowed into its neighbours.** `gLakituTextureBuffer[4][2]`
    took Lakitu textures for all eight racers. The AddressSanitizer build has
    global redzones off (`-asan-globals=0`, snapshots copy whole sections), so it
    never reports this kind: only reading the sizes finds it.
19. **Values made from the player index that leave their range past four.** The
    results' portraits were menu items of type `0xB1 + player` (only four such
    types existed) drawn at priority `MENU_ITEM_PRIORITY_5 - player` (below 0
    for players seven and eight, never drawn), and the face queue
    `D_8018E0E8` held four entries and an end marker: five to eight portraits
    wrote past it into `sMenuTextureListIndex`, and with seven or eight no face
    showed.

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

- With six or eight players no cell is free, so the split has no minimap (with
  five or seven it fills the spare cell; the grid is 3×2 for five and six, 4×2
  for seven and eight, `screen_grid.h`).
- `func_802A7728` / `func_802A7940` grab a 128-wide framebuffer region into a
  narrower cell.
- Two item-centring sites in `render_objects.c` still need a screen id plumbed
  through the generic draw helpers.
- A top-row viewport was once seen rendering mostly black; not observed recently,
  watch for it.
- Left as they are, unless the owner asks: Moo Moo Farm spawns its trees with
  five to eight players, though the original drops them with four
  (`MooMooFarm::BeginPlay()`, `gPlayerCountSelection1 != 4`).

## Things that are settled, so do not re-derive them

- The original game's quirks stay unless online play or five to eight players
  need a change (the owner's call). The Battle red shell's distance check adds
  the Z gap instead of squaring it (`func_802B51E8`), so it can pick someone who
  is not the nearest: left as it is.

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
