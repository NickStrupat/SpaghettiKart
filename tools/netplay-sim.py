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
import socket
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
    cvars: dict = dataclasses.field(default_factory=dict)  # Extra settings for every peer
    peer_cvars: dict = dataclasses.field(default_factory=dict)  # Extra settings, by peer index
    expect: dict = dataclasses.field(default_factory=dict)  # Minimum count of each event, on every peer
    expect_none: tuple = ()  # Events no peer may log
    timeout: int = 300  # Seconds
    quiet_limit: int = 90  # Seconds without a new log line from any game before the scenario counts as stuck

    def env_for(self, peer: int) -> dict:
        return {**self.peer_env.get(-1, {}), **self.peer_env.get(peer, {})}


# Event names for Scenario.expect, and the log lines that count as one
EVENTS = {
    "segments": r"Segment \d+ started",
    "laps": r"Race: player \d+ started lap",
    "finishes": r"Race: player \d+ finished",
    "eliminations": r"Race: player \d+ was eliminated",
    "races_over": r"Race: the race is over",
    "podiums": r"Leaving the podium",
    "cups": r"PASS: The Grand Prix is over",
    "direct": r"Direct connection to slot \d+ at",
    "direct_lost": r"Direct connection to slot \d+ lost",
    "network_address": r"Slot \d+ can be reached at (?!127\.)",
    "sound_self_test": r"Test: sound rollback self-test passed",
    "engines": r"Test: the other players' engines are heard",
    "consoles": r"Console \d+: player",
    "menus_in_sync": r"Test: menus in sync",
    "left_room": r"Leaving the room",
}
# The game prints each line to stdout, then again through its log; count the plain copy only.
EVENTS = {event: re.compile(r"^\[Netplay\] " + pattern, re.MULTILINE) for event, pattern in EVENTS.items()}

# Scripted buttons for player 1 of a shared console, counted from the console's start: 2P on Game Select, then
# versus (or Mario GP), 150cc, OK; on character select, OK once both picked; then the course (cup) and OK.
CONSOLE_2P_VS = "R_JPAD@60,A@90,D_JPAD@120,A@150,A@180,A@210,A@280,A@350,A@420,A@450,A@480"
CONSOLE_2P_GP = "R_JPAD@60,A@90,A@120,A@150,A@180,A@250,A@320,A@390,A@420"

SCENARIOS = [
    Scenario("vs-2p", "2 players, versus, two full races, connected directly; each hears the other's engine", 2, "vs",
             track="mk:luigi_raceway", series_races=2, peer_env={-1: {"SK_NETPLAY_TEST_ENGINES_AT": "300"}},
             expect={"races_over": 2, "finishes": 2, "direct": 1, "engines": 1}, expect_none=("direct_lost",)),
    Scenario("vs-4p", "4 players, versus, one full race; each hears the others' engines", 4, "vs",
             track="mk:mario_raceway", series_races=1, peer_env={-1: {"SK_NETPLAY_TEST_ENGINES_AT": "300"}},
             expect={"races_over": 1, "finishes": 3, "engines": 1}),
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
    Scenario("demo-first", "2 players, two lobby races; the host idles into the title screen's demo first, which "
             "only the title screen (gSkipIntro 1) starts", 2, "vs", track="mk:luigi_raceway", races=2,
             series_races=1, cvars={"gSkipIntro": 1}, peer_env={0: {"SK_NETPLAY_TEST_START_DELAY": "1500"}},
             expect={"races_over": 2}),
    Scenario("two-series", "2 players, two lobby races in one room", 2, "vs", track="mk:wario_stadium", races=2,
             series_races=1, expect={"races_over": 2}),
    Scenario("music-volume", "2 players, versus; a player presses L, which sets the music volume setting from inside "
             "the simulation", 2, "vs", track="mk:moo_moo_farm", series_races=1,
             peer_env={1: {"SK_NETPLAY_TEST_PRESS_L_AT": "400"}}, expect={"races_over": 1}),
    Scenario("mixed-settings", "2 players whose games took different ways through the menus (one starts at the title "
             "screen, one skips it), as two computers do", 2, "battle", track="mk:block_fort", series_races=1,
             peer_cvars={0: {"gSkipIntro": 1}}, expect={"races_over": 1}),
    Scenario("sounds", "3 random drivers among traffic, each peer hearing every camera; their sounds must agree "
             "however rollbacks changed them", 3, "vs", track="mk:toads_turnpike", frames=4000,
             peer_env={-1: {"SK_NETPLAY_BOT": "2", "SK_NETPLAY_TEST_SOUND_ROLLBACK_AT": "600"}},
             cvars={"gNetplay.OwnAudio": 0}, expect={"segments": 1, "sound_self_test": 1}),
    Scenario("relay-only", "2 players, versus, with direct connections turned off: all through the relay", 2, "vs",
             track="mk:moo_moo_farm", series_races=1, cvars={"gNetplay.DirectConnect": 0},
             expect={"races_over": 1}, expect_none=("direct",)),
    Scenario("direct-lost", "2 players, versus; the guest's direct connection dies mid-race and both fall back to "
             "the relay", 2, "vs", track="mk:kalimari_desert", series_races=1,
             # The connection dies 5 s after joining, by the clock: run slower, so that the race is still on then.
             peer_env={-1: {"SK_FAST": "3"}, 1: {"SK_NETPLAY_TEST_BLOCK_DIRECT_MS": "5000"}},
             expect={"races_over": 1, "direct": 1, "direct_lost": 1}),
    Scenario("relay-host", "2 players, versus; the host reaches the relay on its own computer, the guest by that "
             "computer's network address, as a second computer would", 2, "vs", track="mk:yoshi_valley",
             series_races=1, peer_env={1: {"SK_NETPLAY_RELAY": "{lan}:{port}"}},
             # The host's addresses are loopback ones as the relay sees them; the guest must get its network address.
             expect={"races_over": 1, "direct": 1, "network_address": 1}),
    Scenario("console-2p", "2 players on a shared console: the menus in lockstep (Game Select, characters, course, "
             "by scripted buttons), a full versus race, the next race, then the menus again; the host's drawing "
             "settings, which change actor state, apply to both", 2, "vs", series_races=1,
             peer_cvars={0: {"gNoCulling": 1, "gDisableLod": 0}}, peer_env={-1: {"SK_NETPLAY_CONSOLE": "1"},
                                       0: {"SK_TEST_CONSOLE_INPUT": CONSOLE_2P_VS},
                                       1: {"SK_TEST_CONSOLE_INPUT": "R_JPAD@290,A@310"}},
             expect={"consoles": 1, "races_over": 1, "finishes": 1, "menus_in_sync": 1}),
    Scenario("console-spectator", "3 players on a shared console pick a 2-player race: player 3 sits it out and "
             "watches player 1", 3, "vs", series_races=1,
             peer_env={-1: {"SK_NETPLAY_CONSOLE": "1"}, 0: {"SK_TEST_CONSOLE_INPUT": CONSOLE_2P_VS},
                       1: {"SK_TEST_CONSOLE_INPUT": "R_JPAD@290,A@310"}},
             expect={"consoles": 1, "races_over": 1, "menus_in_sync": 1}),
    Scenario("console-gp", "2 players on a shared console: a whole Grand Prix from the menus, the podium, then "
             "Game Select again", 2, "gp",
             peer_env={-1: {"SK_NETPLAY_CONSOLE": "1"}, 0: {"SK_TEST_CONSOLE_INPUT": CONSOLE_2P_GP},
                       1: {"SK_TEST_CONSOLE_INPUT": "R_JPAD@260,A@280"}},
             expect={"consoles": 1, "cups": 1, "podiums": 1, "menus_in_sync": 1}, timeout=600),
    Scenario("console-join", "A third player joins a shared console during its race: it waits, then everyone "
             "starts over on Game Select with three controllers", 3, "vs", series_races=1,
             peer_env={-1: {"SK_NETPLAY_CONSOLE": "1", "SK_NETPLAY_TEST_CONSOLE_PLAYERS": "2,3"},
                       0: {"SK_TEST_CONSOLE_INPUT": CONSOLE_2P_VS}, 1: {"SK_TEST_CONSOLE_INPUT": "R_JPAD@290,A@310"},
                       2: {"SK_NETPLAY_TEST_JOIN_AT": "1200", "SK_NETPLAY_RACES": "0",
                           "SK_NETPLAY_TEST_CONSOLE_PLAYERS": "3"}},
             expect={"consoles": 1, "menus_in_sync": 1}),
    Scenario("console-leave", "2 players on a shared console; the guest leaves with Z on Game Select and the host "
             "carries on alone", 2, "vs", races=0,
             peer_env={-1: {"SK_NETPLAY_CONSOLE": "1"}, 0: {"SK_NETPLAY_TEST_CONSOLE_PLAYERS": "1,2,1"},
                       1: {"SK_TEST_CONSOLE_INPUT": "Z@200", "SK_NETPLAY_TEST_CONSOLE_PLAYERS": "9"}},
             expect={"consoles": 1}),
    Scenario("console-quit-after-race", "2 players on a shared console; the guest closes its game on the race's "
             "results and the host starts over alone, from Game Select", 2, "vs", series_races=1, frames=900,
             peer_env={-1: {"SK_NETPLAY_CONSOLE": "1"},
                       0: {"SK_TEST_CONSOLE_INPUT": CONSOLE_2P_VS, "SK_NETPLAY_TEST_CONSOLE_PLAYERS": "1,2,1"},
                       1: {"SK_TEST_CONSOLE_INPUT": "R_JPAD@290,A@310"}},
             expect={"consoles": 1, "segments": 1, "menus_in_sync": 1}),
    Scenario("lag", "2 players, versus at 60 ms +-20 ms and 5% loss, in real time", 2, "vs",
             track="mk:frappe_snowland", frames=1800,
             peer_env={-1: {"SK_FAST": "0", "SK_NETPLAY_DELAY_MS": "60", "SK_NETPLAY_JITTER_MS": "20",
                            "SK_NETPLAY_LOSS": "5"}},
             expect={"laps": 2}),
]


def lan_address() -> str:
    """This computer's address on its network, where a second computer would reach it."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.connect(("192.0.2.1", 9))  # Connecting a UDP socket sends nothing; it only picks the route
        return probe.getsockname()[0]


def write_home(home: Path, o2r: Path, index: int, cvars: dict):
    """Creates a settings folder: windows small and tiled, no intro, no vsync."""
    home.mkdir(parents=True, exist_ok=True)
    width, height = 400, 300
    config = {
        "CVars": {"gSkipIntro": 3, "gVsyncEnabled": 0, "gInterpolationFPS": 30, **cvars},
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
        # On a shared console, a player who joins during a race does not run it.
        present = [log for log in logs if log.exists()]
        if len(present) < 2:
            mismatches.append(f"{suffix}: only {present[0].name} logged it")
            continue
        lines = [log.read_text().splitlines() for log in present]
        # The last frames of a segment can still be predictions on the peer that ended it first.
        compared = max(min(len(peer) for peer in lines) - 10, 0)
        for log, peer in zip(present[1:], lines[1:]):
            differing = sum(1 for a, b in zip(lines[0][:compared], peer[:compared]) if a != b)
            if differing:
                first = next(n for n, (a, b) in enumerate(zip(lines[0], peer)) if a != b)
                mismatches.append(f"{suffix}: p0 vs {log.name[:2]}: {differing} of {compared} frames differ, first "
                                  f"at line {first + 1}")
    return len(host_logs), mismatches


def compare_menu_logs(directory: Path, players: int):
    """A shared console's menus run in lockstep: every peer in a menu session logs the same checksum each frame.
    @returns (sessions compared, list of mismatch descriptions)."""
    mismatches = []
    compared_sessions = 0
    for host_log in sorted(directory.glob("p0.log.console*.seg*.menu")):
        suffix = host_log.name[len("p0.log"):]
        logs = [directory / f"p{i}.log{suffix}" for i in range(players)]
        # Sessions of a console that some peers were not part of (the host alone, before the others joined)
        present = [log for log in logs if log.exists()]
        if len(present) < 2:
            continue
        lines = [log.read_text().splitlines() for log in present]
        # Each peer stops logging when it leaves; compare the frames all of them ran.
        common = min(len(peer) for peer in lines)
        compared_sessions += 1
        for log, peer in zip(present[1:], lines[1:]):
            differing = [n for n in range(common) if peer[n] != lines[0][n]]
            if differing:
                mismatches.append(f"{suffix}: p0 vs {log.name[:2]}: {len(differing)} of {common} menu frames "
                                  f"differ, first at frame {differing[0] + 1}")
    return compared_sessions, mismatches


def compare_sound_logs(directory: Path, players: int):
    """Peers that hear every camera must end up with the same sounds for each frame, whatever their rollbacks.
    @returns (sounds compared, list of mismatch descriptions)."""
    mismatches = []
    total = 0
    for host_log in sorted(directory.glob("p0.log.race*.seg*.sounds")):
        suffix = host_log.name[len("p0.log"):-len(".sounds")]
        logs = [directory / f"p{i}.log{suffix}.sounds" for i in range(players)]
        checksums = [directory / f"p{i}.log{suffix}" for i in range(players)]
        if not all(log.exists() for log in logs + checksums):
            mismatches.append(f"{suffix}: a peer wrote no sound log")
            continue
        # Like the checksums, the last frames can still be predictions on the peer that ended first.
        last_frame = min(len(log.read_text().splitlines()) for log in checksums) - 10
        sounds = [[line for line in log.read_text().splitlines() if int(line.split()[0]) < last_frame]
                  for log in logs]
        total += len(sounds[0])
        for i in range(1, players):
            if sounds[i] != sounds[0]:
                first = next((n for n, (a, b) in enumerate(zip(sounds[0], sounds[i])) if a != b),
                             min(len(sounds[0]), len(sounds[i])))
                mismatches.append(f"{suffix} sounds: p0 has {len(sounds[0])}, p{i} has {len(sounds[i])}, first "
                                  f"difference at entry {first + 1}")
    return total, mismatches


def wait_for_games(games, outputs, deadline, quiet_limit):
    """Waits for every game to exit, and stops them all when time runs out or none logs anything for quiet_limit
    seconds. @returns why they were stopped, or an empty string."""
    reason = ""
    last_sizes = None
    last_change = time.monotonic()
    while any(game.poll() is None for game in games):
        now = time.monotonic()
        sizes = [path.stat().st_size for path in outputs]
        if sizes != last_sizes:
            last_sizes = sizes
            last_change = now
        if now >= deadline:
            reason = "stopped: the scenario's time ran out"
        elif now - last_change >= quiet_limit:
            reason = f"stopped: no game logged anything for {quiet_limit} s"
        if reason:
            for game in games:
                if game.poll() is None:
                    os.killpg(game.pid, signal.SIGKILL)
                    game.wait()
            break
        time.sleep(0.5)
    return reason


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
        write_home(home, args.o2r, i, {**scenario.cvars, **scenario.peer_cvars.get(i, {})})
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
        for name, value in {**scenario.peer_env.get(-1, {}), **scenario.peer_env.get(i, {})}.items():
            env[name] = value.format(port=port, lan=lan_address() if "{lan}" in value else "")
        output_path = directory / f"p{i}.out"
        outputs.append(output_path)
        with open(output_path, "w") as output:
            # After a game crashes, macOS asks whether to reopen its windows at the next start, and the game waits
            # for an answer; this skips that.
            games.append(subprocess.Popen([str(args.exe), "-ApplePersistenceIgnoreState", "YES"], env=env,
                                          stdout=output, stderr=subprocess.STDOUT, start_new_session=True))
        # The host creates the room and writes its code; guests wait for it.
        time.sleep(1.0 if i == 0 else 0.3)

    stopped = wait_for_games(games, outputs, started + scenario.timeout, scenario.quiet_limit)
    relay.kill()
    relay.wait()
    relay_out.close()
    elapsed = time.monotonic() - started

    problems = []
    if stopped:
        problems.append(stopped)
    texts = [path.read_text(errors="replace") for path in outputs]
    for i, (game, text) in enumerate(zip(games, texts)):
        lines = [line for line in text.splitlines() if line.startswith("[Netplay]")]
        if stopped:
            problems.append(f"p{i} last logged: {lines[-1][10:] if lines else '(nothing)'}")
        elif any(pattern in text for pattern in CRASH_PATTERNS) or game.returncode < 0:
            problems.append(f"p{i} crashed (exit {game.returncode})")
        elif game.returncode != 0:
            failure = next((line for line in reversed(lines) if "FAIL" in line or "DESYNC" in line), "")
            problems.append(f"p{i} exit {game.returncode}: {failure[10:]}")
        for line in lines:
            if "DESYNC" in line:
                problems.append(f"p{i}: {line[10:]}")
        for event, minimum in scenario.expect.items():
            count = len(EVENTS[event].findall(text))
            if count < minimum:
                problems.append(f"p{i}: {count} {event}, expected at least {minimum}")
        for event in scenario.expect_none:
            count = len(EVENTS[event].findall(text))
            if count:
                problems.append(f"p{i}: {count} {event}, expected none")

    segments, mismatches = compare_checksum_logs(directory, scenario.players)
    problems += mismatches
    if segments == 0 and scenario.races > 0:
        problems.append("no race segment was logged")
    menu_sessions, mismatches = compare_menu_logs(directory, scenario.players)
    problems += mismatches
    if scenario.env_for(-1).get("SK_NETPLAY_CONSOLE") == "1" and menu_sessions == 0:
        problems.append("no shared console menu session was compared")
    sounds = 0
    if scenario.cvars.get("gNetplay.OwnAudio") == 0:
        sounds, mismatches = compare_sound_logs(directory, scenario.players)
        problems += mismatches
        if sounds == 0:
            problems.append("no sound was logged")

    host = texts[0] if texts else ""
    counts = {event: len(pattern.findall(host)) for event, pattern in EVENTS.items()}
    counts["sounds_compared"] = sounds
    counts["menu_sessions"] = menu_sessions
    for kind in ("played late", "stopped", "restarted"):
        counts["sounds_" + kind.replace(" ", "_")] = sum(
            int(n) for n in re.findall(r"^\[Netplay\] .*, (\d+) " + kind, host, re.MULTILINE))
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
    for event in ("segments", "laps", "finishes", "eliminations", "races_over", "cups", "podiums", "direct",
                  "direct_lost", "sounds_compared", "sounds_played_late", "sounds_stopped", "sounds_restarted"):
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
