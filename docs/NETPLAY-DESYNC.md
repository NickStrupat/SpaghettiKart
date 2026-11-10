# Netplay: desyncs, recovery and dropped connections

Notes from the investigation of 2026-11-06 to 2026-11-09. Nothing here is built yet: this records what was found,
what was measured, the options and the direction chosen. `NETPLAY.md` describes how the parts work today.

## Summary

- A bad connection cannot cause a desync by itself. A desync is a bug in the game; recovery is the safety net.
- Recovery fails about 1 run in 8 on one Mac, and will fail more often between two Macs. Both causes are in the sim
  arena, and both come from memory that is not game state ending up in it.
- Two short fixes were measured and work. Neither is built.
- The direction chosen: game state lives in its own arena, holds only game state, and refers to anything outside by
  key instead of by pointer. Rough estimate 2 to 4 days; the inventory that would firm it up has not been done.
- A connection that drops for under 5 s carries on. Over 5 s the race ends for everyone; there is no rejoin. Neither
  case has a simulator test.

## What a desync is

The games send each other button inputs and nothing else. A late or lost input costs a rollback or a short wait; it
cannot make two copies disagree. A desync means two copies computed different results from the same inputs. There are
two defences:

1. Find and fix each such bug. The simulator, the desync reports and the pitfalls list in `NETPLAY.md` serve this.
2. Recovery: when a desync happens anyway, every player takes over the host's state and the race goes on
   (`Resync.cpp`, see "Desync recovery" in `NETPLAY.md`). A recovered desync is still a bug to find.

## Why recovery fails

The simulator's `desync-recover` scenario fails about 1 run in 8 with "this game's state is too far from the host's".

How recovery handles pointers: a snapshot is raw memory. Pointers into the game's image and into the sim arena hold in
every copy, because address randomization is off. Pointers into the regular heap (the loaded game files) differ
between copies, so the host marks every 8-byte word of its snapshot that looks like a pointer into its own heap, and
each guest keeps its own value at those places. A guest whose value there is not a heap pointer gives up: "too far".

### Cause 1: leftovers in freed arena blocks

Freeing an arena block clears its magic number and writes the free-list link into its first word. The rest keeps its
old contents. Some of those contents are resource-loader objects holding heap pointers and path strings, and they
differ between copies. The host marks the heap-looking leftovers; the guest has something else at that place (in the
failures caught, the text of a resource path), which is not a heap pointer.

All four failures caught were this.

### Cause 2: the arena layout differs between copies

Race setup and every simulated frame run inside a sim heap scope, where every `operator new` on the game thread goes
to the arena (`SimArena.cpp`). Loading a resource in that scope goes through libultraship's
`ResourceManager::LoadResource`, which is `LoadResourceAsync(...).get()`:

- On a cache hit, the promise is made and freed on the game thread. That is the same in every copy.
- On a cache miss, the load goes to a thread-pool worker. The task and its promise are allocated on the game thread,
  so in the arena, and a worker frees them when it gets to it. The order of the free lists then depends on timing, and
  so does every later address.

The first difference found was at allocation 56, in `generate_collision_mesh`. Without a fix, the host's and the
guests' arena layouts differed in 8 of 8 comparisons at the recovery frame, including the runs where recovery
"succeeded". Two Macs whose games loaded different files before the race will differ more.

### A crash this can cause

A cache miss inside a simulated frame leaves an arena block that a worker will free later. If a rollback restores the
arena before the worker frees it, the worker's free is a double free and the arena aborts. Not seen yet.

### The pattern

This is the second time memory that is not game state ended up in the arena: first the music volume CVar (the host
froze on ESC, 2026-11-04), now the resource loader's task and promise. Today's rule is "everything allocated during a simulated
frame goes to the arena unless it opts out" (`SimHeapPause`), so every library the race code calls has to know about
the arena.

## What was measured

All on one Mac, `desync-recover` over 127.0.0.1, comparing the host's arena with each guest's at the recovery frame.
The arena there is about 75 KB in about 80 blocks.

| Setup | Runs passed | Guests with the host's arena layout | Differing leftovers in freed blocks |
|---|---|---|---|
| No fix | about 7 of 8 | 0 of 8 | yes |
| Every resource loaded at launch | not counted | 8 of 8 | not measured |
| Sim heap paused inside `LoadResourceAsync` | 8 of 8 | 16 of 16 | none |
| The same, and only the host loaded every resource at launch | 8 of 8 | 16 of 16 | none |

- Loading every resource at launch: `LoadResources(ResourceFilter({ "*/*" }, { "*.*" }, 0, nullptr))` after
  `CustomEngineInit()`. 32,693 resources, about 1 s, 84 MB more memory. `LoadResources("*")` crashes libultraship: it
  parses `mods.toml` and `version` as resources.
- Pausing the sim heap inside the loader: `SimHeap_Pause()` around the body of
  `ResourceManager::LoadResourceAsync(const ResourceIdentifier&, ...)`, about 10 lines. The last row is the two-Mac
  case in small: one copy with every file cached, the others with none.
- How to trace arena allocations, should it be needed again: record `backtrace()` per `Allocate` and write the records
  before `Game.cpp`'s `_Exit` (atexit handlers do not run), then symbolize with `atos -l 0x100000000`. Raw traces stop
  being comparable at the first rollback; compare block layouts at a common frame instead.

## Options considered

| | Option | Verdict |
|---|---|---|
| 1 | Skip freed blocks when marking heap pointers, and pause the sim heap inside libultraship's resource manager | Measured, works. Needs a libultraship change: a direct call upstream will not take, or a hook around loads |
| 2 | Only skip freed blocks | Removes the failures seen, leaves the layouts different |
| 3 | Skip freed blocks, and wrap the 114 resource call sites in game code | No libultraship change, but every new call site must remember |
| 4 | Snapshots that copy only live objects in a fixed order, matched across arenas | Sound, but needs the loader out of the arena anyway |
| 5 | Load every resource at launch, skip freed blocks, and fail tests on a cross-thread arena free | Measured, works, about 1 to 1.5 hours. Depends on the cache staying complete |
| 6 | An arena that game state opts in to | The cleanest end state. A missed allocation fails silently |

The owner's view of the hook in option 1: leaky. The game should put its own state in one place, and nothing else
should need to know.

## Direction: self-contained game state

Chosen by the owner on 2026-11-08, not started.

1. **Game state allocates in its own arena, by choice.** Everything else allocates wherever it likes. No library
   needs to know about the arena.
2. **Everything game state owns is in the arena, including what its containers hold:** a vector's buffer, a string's
   text, a map's nodes, a `shared_ptr`'s control block. A plain `std::vector` inside an arena object keeps its buffer
   on the regular heap, where a rollback does not restore it.
3. **Game state holds no pointers to anything outside it.** It refers to a loaded resource by its path or hash and to
   a registry entry by its ID. Code outside the game state likewise holds an actor's ID, not a pointer to it, or
   fetches the pointer again after a rollback.

What this gives:

- Recovery is exact. The host no longer guesses which words are heap pointers and the guest no longer needs its own
  state to be close: "too far" goes away, also when the guest's state is badly off, which is when recovery matters.
- A test can check it exactly: any regular-heap pointer in a snapshot is a bug.
- The CVar and resource-loader class of bug cannot happen.

What it costs:

- **Failures become silent.** Today a missed opt-out crashes or hangs. With opt-in, a missed allocation just does not
  roll back, and shows up later as a desync or a rare use of freed memory. To make that loud again: arena versions of
  the common containers so the safe choice is the easy one, the snapshot check above, and the rollback stress run.
- **The original C game code keeps raw pointers to loaded track data** (display lists, vertices, textures) in globals
  and dereferences them all over. Switching every use to a key lookup is a large change to decompiled code and drifts
  from upstream SpaghettiKart.
- **Address randomization stays off.** Snapshots also hold pointers into the game's image (vtables, function
  pointers) and within the arena. Keys remove only the pointers to the regular heap.

A middle way for the C code: running code keeps its pointers; when the host packs a snapshot for recovery it turns
each outside pointer into a key (which resource, and the offset inside it), and the guest turns each key back into its
own pointer. New and converted C++ code stores real keys. The rule a test enforces becomes "game state may point
outside only into loaded resources". The catch: finding the pointers still means scanning for values that look like
heap addresses, with a tiny but not zero chance that plain data matches.

Which way fits depends on where today's outside pointers (56 at the recovery frame) live: in C globals, the middle
way; in C++ objects, keys outright.

## Dropped connections

Not designed in this discussion. What the code does today (`NetSession.cpp`, `RelayClient.cpp`, GekkoNet):

| The connection is silent for | What happens |
|---|---|
| Up to 8 frames | The game predicts the missing inputs and rolls back when they arrive (`kPredictionWindow`) |
| Longer, up to 5 s | The game waits, then carries on |
| 1.5 s, on a direct connection | It is given up for the relay (`kDirectTimeout`) and the race carries on |
| Over 5 s | GekkoNet reports the player disconnected; the race ends for everyone. There is no rejoin |
| 15 s, towards the relay | The relay drops the player from the room (`kClientTimeoutMs`) |

- A network change, such as from Wi-Fi to a phone hotspot, changes the player's address. The relay knows a player by
  its token, so the player keeps its slot; direct connections die and fall back to the relay. A change that takes
  under 5 s should therefore survive. Not tested.
- The simulator covers delay, jitter, 5% loss (`lag`) and a direct connection that dies (`direct-lost`). It has no
  scenario for a few seconds of silence, or for an address change.
- Rejoining a race after a long drop would be a new feature. It needs the same exact state transfer as recovery, so
  the work above comes first.

The owner recalled a desync after moving one Mac to a phone hotspot on 2026-11-06. That Mac's log has none that day:
both sessions ended with "The game closed", and its last recorded desync is from 2026-11-05 14:53, at frame 0. If it
happened, the report is on the other Mac, under `netplay/` in its settings folder.

## Next steps

1. Inventory, read-only: every C++ game-state type and container to convert, and where the 56 outside pointers live.
   This turns "2 to 4 days" into a plan and decides between keys outright and the middle way.
2. A simulator scenario for a few seconds of silence (the relay needs an option for it), and one for an address
   change.
3. Until the arena work lands, recovery keeps failing about 1 time in 8. Option 5 is the stopgap that needs no
   libultraship change.
4. Recovery between two Macs is still untested (see "Open" in `NETPLAY.md`).
