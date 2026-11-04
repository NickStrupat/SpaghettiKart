# Notes for coding agents

This fork of SpaghettiKart (the MK64 PC port) adds online multiplayer with rollback netcode, on the `netplay` branch.
Read `docs/NETPLAY.md` first: it records the design, the owner's decisions, the state of each part and the pitfalls
found so far. Each header in `src/port/netplay/` describes its own part.

## Build

```sh
cmake --build build-cmake                    # the game: build-cmake/SpaghettiKart.app on macOS
cmake --build build-relay                    # the relay server: build-relay/netplay-relay
cmake build-cmake                            # after adding a source file (sources are globbed)
```

The game needs the extracted `mk64.o2r` (on macOS in `~/Library/Application Support/SpaghettiKart/`).

## Before calling a change done

Any change to simulation code (anything under `src/` that runs during a race) or to `src/port/netplay/` can break
determinism, which only shows up between two running games.

1. `python3 tools/netplay-sim.py`: 13 scenarios, about 4 minutes; every one must pass.
2. For relay or protocol changes: `python3 tools/netplay-relay/test_relay.py build-relay/netplay-relay`.
3. For a new kind of race state: a rollback stress run with the determinism harness (`SK_DETERMINISM_ROLLBACK=8`, see
   `src/port/netplay/DeterminismHarness.h`).

When a scenario desyncs, diff the peers' `*.kept.txt` reports in its log folder, or rerun with `SK_NETPLAY_DUMP=1` and
compare the start states with `tools/netplay-diff.py`.

## Rules

- Check the pitfalls in `docs/NETPLAY.md` before touching simulation code. The usual ones: draw code writes simulation
  state; memory-pool buffers are not in snapshots; allocations inside a simulated frame go to the sim arena (use
  `SimHeapPause` for anything else); nothing machine-local (time, audio randomness, frame-buffer indices) may feed
  back into the simulation.
- Mutable globals in simulation files are snapshotted automatically (`SimSection.h`). Wrap anything that is not
  simulation state in `NOT_SIM_STATE_BEGIN` / `NOT_SIM_STATE_END`.
- Test with throwaway settings folders (`SHIP_HOME`); the simulator makes its own. Never edit the owner's own config
  or saves.
- The owner may be playing the game while you work: stop or capture only processes you started, by their PID, never
  by name. Capture game windows only, never the whole screen.
- Match the surrounding code: its comment density, naming and idiom. New netplay code follows the style of the
  existing files in `src/port/netplay/` (doc comments on declarations, plain-language comments on why).
- Commit messages start with `Netplay: ` for netplay work. Commit and push only when asked.
- Mac to Mac is the only target for now; don't spend effort on Windows or Linux unless asked.
