#!/usr/bin/env python3
"""
Netplay simulator: plays netplay races between bot-driven copies of the game on this computer, fast-forwarded, and
checks that every copy stayed in sync and that the races reached their end.

Each scenario starts a relay (tools/netplay-relay) and one game per player. The games meet in a room, and bots drive:
they follow the track, use items, hunt opponents in Battle and confirm the screens after a race
(SK_NETPLAY_BOT=1, see src/port/netplay/SyntheticDriver.h). The games run without frame pacing or sound, drawing one
frame in --speed (SK_FAST), so a three-lap race takes seconds.

A scenario passes when every game exits cleanly, every race segment's per-frame checksums match on all peers, and the
race events it expects were logged (laps, finishes, eliminations, the podium).

Usage:
  tools/netplay-sim.py                 Run every scenario
  tools/netplay-sim.py vs-2p gp-2p     Run these
  tools/netplay-sim.py --list          List the scenarios

Each game gets a fresh settings folder under --work, so your own settings and saves are never touched. The games need
the extracted mk64.o2r (--o2r; by default the one the game uses on macOS).
"""

import argparse
import dataclasses
import json
import os
import platform
import random
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

if platform.system() == "Darwin":
    DEFAULT_EXE = REPO / "build-cmake/SpaghettiKart.app/Contents/MacOS/SpaghettiKart"
    DEFAULT_O2R = Path.home() / "Library/Application Support/SpaghettiKart/mk64.o2r"
else:
    DEFAULT_EXE = REPO / "build-cmake/Spaghettify"
    DEFAULT_O2R = REPO / "mk64.o2r"
DEFAULT_RELAY = REPO / "build-relay/netplay-relay"

# Lines that mean a game crashed rather than reported a result
CRASH_PATTERNS = ("Segmentation fault", "Bus error", "Abort trap", "terminate called", "Assertion failed")


@dataclasses.dataclass
class Scenario:
    name: str
    description: str
    players: int
    mode: str  # vs, gp or battle
    track: str = ""  # VS and Battle
    cup: int = 0  # Grand Prix
    races: int = 1  # Lobby races (series) the host starts
    series_races: int = 0  # Leave a series after this many races; versus and Battle retry forever otherwise
    frames: int = 0  # End the series after this many shown frames; 0 lets it end by itself
    lag: tuple = (0, 0, 0)  # Relay delay ms, jitter ms, loss percent
    peer_env: dict = dataclasses.field(default_factory=dict)  # Extra environment, by peer index
    expect: dict = dataclasses.field(default_factory=dict)  # Minimum count of each event, on every peer
    timeout: int = 300  # Seconds


# Event names for Scenario.expect, and the log lines that count as one
EVENTS = {
    "segments": r"Segment \d+ started",
    "laps": r"Race: player \d+ started lap",
    "finishes": r"Race: player \d+ finished",
    "eliminations": r"Race: player \d+ was eliminated",
    "races_over": r"Race: the race is over",
    "podiums": r"Leaving the podium",
    "cups": r"PASS: The Grand Prix is over",
}
# The game prints each line to stdout, then again through its log; count the plain copy only.
EVENTS = {event: re.compile(r"^\[Netplay\] " + pattern, re.MULTILINE) for event, pattern in EVENTS.items()}

SCENARIOS = [
    Scenario("vs-2p", "2 players, versus, two full races", 2, "vs", track="mk:luigi_raceway", series_races=2,
             expect={"races_over": 2, "finishes": 2}),
    Scenario("vs-4p", "4 players, versus, one full race", 4, "vs", track="mk:mario_raceway", series_races=1,
             expect={"races_over": 1, "finishes": 3}),
    Scenario("gp-2p", "2 players and 6 CPUs, the Mushroom Cup twice, with the podium in between", 2, "gp", cup=0,
             races=2, expect={"cups": 2, "races_over": 8, "podiums": 1}, timeout=600),
    Scenario("battle-2p", "2 players, Battle on Block Fort, until one is out", 2, "battle", track="mk:block_fort",
             series_races=1, expect={"eliminations": 1, "races_over": 1}),
    Scenario("battle-4p", "4 players, Battle on Big Donut, until one is left", 4, "battle", track="mk:big_donut",
             series_races=1, expect={"eliminations": 3, "races_over": 1}),
    Scenario("restart", "2 players, versus; the host restarts the race twice", 2, "vs", track="mk:royal_raceway",
             series_races=2, peer_env={0: {"SK_NETPLAY_TEST_CONTROLS": "restart@400"}}, expect={"segments": 3}),
    Scenario("guest-leaves", "3 players, versus; player 3 leaves mid-race and everyone returns to the room", 3, "vs",
             track="mk:koopa_troopa_beach", peer_env={2: {"SK_NETPLAY_TEST_CONTROLS": "leave@600"}},
             expect={"segments": 1}),
    Scenario("demo-first", "2 players; the host idles into the title screen's demo race first", 2, "vs",
             track="mk:luigi_raceway", series_races=1,
             peer_env={0: {"SK_NETPLAY_TEST_START_DELAY": "900"}}, expect={"races_over": 1}),
    Scenario("two-series", "2 players, two lobby races in one room", 2, "vs", track="mk:wario_stadium", races=2,
             series_races=1, expect={"races_over": 2}),
    Scenario("lag", "2 players, versus at 60 ms +-20 ms and 5% loss, in real time", 2, "vs",
             track="mk:frappe_snowland", frames=1800, lag=(60, 20, 5), peer_env={-1: {"SK_FAST": "0"}},
             expect={"laps": 2}),
]


def write_home(home: Path, o2r: Path, index: int):
    """Creates a settings folder: windows small and tiled, no intro, no vsync."""
    home.mkdir(parents=True, exist_ok=True)
    width, height = 400, 300
    config = {
        "CVars": {"gSkipIntro": 3, "gVsyncEnabled": 0, "gInterpolationFPS": 30},
        "Window": {
            "Width": width,
            "Height": height,
            "PositionX": 40 + (index % 2) * (width + 20),
            "PositionY": 60 + (index // 2) * (height + 40),
        },
    }
    (home / "spaghettify.cfg.json").write_text(json.dumps(config, indent=4))
    link = home / "mk64.o2r"
    if not link.exists():
        link.symlink_to(o2r)


def compare_checksum_logs(directory: Path, players: int):
    """@returns (segments compared, list of mismatch descriptions)."""
    mismatches = []
    # One checksum per frame, in p<peer>.log.race<id>.seg<segment>; other files there are reports.
    host_logs = sorted(log for log in directory.glob("p0.log.race*.seg*")
                       if re.fullmatch(r"p0\.log\.race\d+\.seg\d+", log.name))
    for host_log in host_logs:
        suffix = host_log.name[len("p0.log"):]
        logs = [directory / f"p{i}.log{suffix}" for i in range(players)]
        missing = [log.name for log in logs if not log.exists()]
        if missing:
            mismatches.append(f"{suffix}: no log from {', '.join(missing)}")
            continue
        lines = [log.read_text().splitlines() for log in logs]
        # The last frames of a segment can still be predictions on the peer that ended it first.
        compared = max(min(len(peer) for peer in lines) - 10, 0)
        for i in range(1, players):
            differing = sum(1 for a, b in zip(lines[0][:compared], lines[i][:compared]) if a != b)
            if differing:
                first = next(n for n, (a, b) in enumerate(zip(lines[0], lines[i])) if a != b)
                mismatches.append(f"{suffix}: p0 vs p{i}: {differing} of {compared} frames differ, first at line "
                                  f"{first + 1}")
    return len(host_logs), mismatches


def run_scenario(scenario: Scenario, args, run_dir: Path):
    directory = run_dir / scenario.name
    directory.mkdir(parents=True)
    port = random.randint(20000, 40000)
    relay_args = [str(args.relay), "--port", str(port), "--no-rate-limits"]
    delay, jitter, loss = scenario.lag
    if delay or jitter or loss:
        relay_args += ["--delay-ms", str(delay), "--jitter-ms", str(jitter), "--loss", str(loss)]
    relay_out = open(directory / "relay.out", "w")
    relay = subprocess.Popen(relay_args, stdout=relay_out, stderr=subprocess.STDOUT)
    time.sleep(0.3)

    started = time.monotonic()
    games = []
    outputs = []
    for i in range(scenario.players):
        home = directory / f"home{i}"
        write_home(home, args.o2r, i)
        env = dict(os.environ)
        env.update({
            "SHIP_HOME": str(home),
            "SK_FAST": str(args.speed),
            "SK_NETPLAY_RELAY": f"127.0.0.1:{port}",
            "SK_NETPLAY_HOST": "1" if i == 0 else "0",
            "SK_NETPLAY_ROOM_FILE": str(directory / "room"),
            "SK_NETPLAY_PLAYERS": str(scenario.players),
            "SK_NETPLAY_RACES": str(scenario.races),
            "SK_NETPLAY_MODE": scenario.mode,
            "SK_NETPLAY_CUP": str(scenario.cup),
            "SK_NETPLAY_BOT": "1",
            "SK_NETPLAY_FRAMES": str(scenario.frames),
            "SK_NETPLAY_LOG": str(directory / f"p{i}.log"),
            "SK_NETPLAY_TEST_SERIES_RACES": str(scenario.series_races),
        })
        if scenario.track:
            env["SK_NETPLAY_TRACK"] = scenario.track
        env.update(scenario.peer_env.get(-1, {}))
        env.update(scenario.peer_env.get(i, {}))
        output_path = directory / f"p{i}.out"
        outputs.append(output_path)
        with open(output_path, "w") as output:
            games.append(subprocess.Popen([str(args.exe)], env=env, stdout=output, stderr=subprocess.STDOUT,
                                          start_new_session=True))
        # The host creates the room and writes its code; guests wait for it.
        time.sleep(1.0 if i == 0 else 0.3)

    timed_out = False
    deadline = started + scenario.timeout
    for game in games:
        try:
            game.wait(timeout=max(deadline - time.monotonic(), 0.1))
        except subprocess.TimeoutExpired:
            timed_out = True
    for game in games:
        if game.poll() is None:
            os.killpg(game.pid, signal.SIGKILL)
            game.wait()
    relay.kill()
    relay.wait()
    relay_out.close()
    elapsed = time.monotonic() - started

    problems = []
    if timed_out:
        problems.append(f"timed out after {scenario.timeout} s")
    texts = [path.read_text(errors="replace") for path in outputs]
    for i, (game, text) in enumerate(zip(games, texts)):
        lines = [line for line in text.splitlines() if line.startswith("[Netplay]")]
        if any(pattern in text for pattern in CRASH_PATTERNS) or game.returncode < 0:
            problems.append(f"p{i} crashed (exit {game.returncode})")
        elif game.returncode != 0 and not timed_out:
            failure = next((line for line in reversed(lines) if "FAIL" in line or "DESYNC" in line), "")
            problems.append(f"p{i} exit {game.returncode}: {failure[10:]}")
        for line in lines:
            if "DESYNC" in line:
                problems.append(f"p{i}: {line[10:]}")
        for event, minimum in scenario.expect.items():
            count = len(EVENTS[event].findall(text))
            if count < minimum:
                problems.append(f"p{i}: {count} {event}, expected at least {minimum}")

    segments, mismatches = compare_checksum_logs(directory, scenario.players)
    problems += mismatches
    if segments == 0:
        problems.append("no race segment was logged")

    host = texts[0] if texts else ""
    counts = {event: len(pattern.findall(host)) for event, pattern in EVENTS.items()}
    return {
        "name": scenario.name,
        "passed": not problems,
        "problems": problems,
        "elapsed": elapsed,
        "segments": segments,
        "counts": counts,
        "directory": directory,
    }


def summarize_events(counts):
    parts = []
    for event in ("segments", "laps", "finishes", "eliminations", "races_over", "cups", "podiums"):
        if counts.get(event):
            parts.append(f"{counts[event]} {event.replace('_', ' ')}")
    return ", ".join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenarios", nargs="*", help="scenario names (default: all)")
    parser.add_argument("--list", action="store_true", help="list the scenarios and exit")
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE, help="game executable")
    parser.add_argument("--relay", type=Path, default=DEFAULT_RELAY, help="relay executable")
    parser.add_argument("--o2r", type=Path, default=DEFAULT_O2R, help="extracted mk64.o2r")
    parser.add_argument("--work", type=Path, default=Path(os.environ.get("TMPDIR", "/tmp")) / "netplay-sim",
                        help="folder for settings folders and logs")
    parser.add_argument("--speed", type=int, default=10,
                        help="draw one frame in this many and skip pacing; 0 plays in real time")
    args = parser.parse_args()

    if args.list:
        for scenario in SCENARIOS:
            print(f"{scenario.name:14} {scenario.description}")
        return 0
    selected = [s for s in SCENARIOS if not args.scenarios or s.name in args.scenarios]
    unknown = set(args.scenarios) - {s.name for s in SCENARIOS}
    if unknown:
        print(f"Unknown scenario: {', '.join(sorted(unknown))}. See --list.")
        return 2
    for path, what in ((args.exe, "game"), (args.relay, "relay"), (args.o2r, "mk64.o2r")):
        if not path.exists():
            print(f"No {what} at {path}")
            if what == "relay":
                print("Build it: cmake -S tools/netplay-relay -B build-relay && cmake --build build-relay")
            return 2

    run_dir = args.work / time.strftime("run-%Y%m%d-%H%M%S")
    print(f"Logs: {run_dir}")
    results = []
    for scenario in selected:
        print(f"{scenario.name:14} ", end="", flush=True)
        result = run_scenario(scenario, args, run_dir)
        results.append(result)
        verdict = "PASS" if result["passed"] else "FAIL"
        print(f"{verdict}  {result['elapsed']:5.0f} s  {summarize_events(result['counts'])}")
        for problem in result["problems"]:
            print(f"{'':16}- {problem}")

    failed = [r for r in results if not r["passed"]]
    print(f"\n{len(results) - len(failed)} of {len(results)} scenarios passed.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
