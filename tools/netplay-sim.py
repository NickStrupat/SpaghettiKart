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

Scenarios run side by side, up to --max-games games at once (each scenario runs one game per player); the ones that
depend on the wall clock run alone. --max-games 1 runs them one at a time.

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
import threading
import time
from concurrent import futures
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
    peer_expect: dict = dataclasses.field(default_factory=dict)  # Minimum count of each event, by peer index
    expect_none: tuple = ()  # Events no peer may log
    saved_cvars: dict = dataclasses.field(default_factory=dict)  # Settings file values at the end, by peer; None: absent
    timeout: int = 300  # Seconds
    quiet_limit: int = 90  # Seconds without a new log line from any game before the scenario counts as stuck
    desync_expected: bool = False  # A peer's state is corrupted on purpose: DESYNC lines are not a failure
    alone: bool = False  # Depends on the wall clock: runs with no other scenario beside it (see --max-games)

    def env_for(self, peer: int) -> dict:
        return {**self.peer_env.get(-1, {}), **self.peer_env.get(peer, {})}


# Event names for Scenario.expect, and the log lines that count as one
EVENTS = {
    "segments": r"Segment \d+ started",
    "laps": r"Race: player \d+ started lap",
    "finishes": r"Race: player \d+ finished",
    "eliminations": r"Race: player \d+ was eliminated",
    "races_over": r"Race: the race is over",
    "cpus_racing": r"Race: (\d+) of \1 CPUs completed a lap",  # Every CPU in a Grand Prix race
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
    "paused": r"Race: player \d+ paused",
    "went_on": r"Race: player \d+ went on",
    "private_room": r"Joined private room",
    "matched": r"Matched into room",
    "matched_away": r"Match found while away",
    "waited_for_away": r"Waiting for \S+ to come back from playing offline",
    "recoveries": r"Recovered from the desync",
    "voice_heard": r"Voice test: hearing note \d+",
    "voice_stopped": r"Voice test: note \d+ stopped",
}
# The game prints each line to stdout, then again through its log; count the plain copy only.
EVENTS = {event: re.compile(r"^\[Netplay\] " + pattern, re.MULTILINE) for event, pattern in EVENTS.items()}

# Scripted buttons for player 1 of a shared console, counted from the console's start: 2P on Game Select, then
# versus (or Mario GP), 150cc, OK; on character select, OK once both picked; then the course (cup) and OK.
CONSOLE_2P_VS = "R_JPAD@60,A@90,D_JPAD@120,A@150,A@180,A@210,A@280,A@350,A@420,A@450,A@480"
CONSOLE_2P_GP = "R_JPAD@60,A@90,A@120,A@150,A@180,A@250,A@320,A@390,A@420"
# The same with 3P chosen on Game Select (a second R_JPAD), for Grand Prix with three players
CONSOLE_3P_GP = "R_JPAD@60,R_JPAD@75,A@90,A@120,A@150,A@180,A@250,A@320,A@390,A@420"
CONSOLE_3P_VS = "R_JPAD@60,R_JPAD@70,A@90,D_JPAD@120,A@150,A@180,A@210,A@280,A@350,A@420,A@450,A@480"
# 3P first, which a room of 2 refuses (Game Select stays put); then back to 2P and on as CONSOLE_2P_VS, 30 frames later
CONSOLE_3P_REFUSED_2P_VS = ("R_JPAD@60,R_JPAD@70,A@90,L_JPAD@105,A@120,D_JPAD@150,A@180,A@210,A@240,A@310,A@380,A@450,"
                            "A@480,A@510")
CONSOLE_4P_VS = "R_JPAD@60,R_JPAD@70,R_JPAD@80,A@90,D_JPAD@120,A@150,A@180,A@210,A@280,A@350,A@420,A@450,A@480"
# 8P on Game Select (seven R_JPADs), then on as CONSOLE_4P_VS
CONSOLE_8P_VS = ("R_JPAD@40,R_JPAD@45,R_JPAD@50,R_JPAD@55,R_JPAD@60,R_JPAD@65,R_JPAD@70,A@90,D_JPAD@120,A@150,A@180,"
                 "A@210,A@280,A@350,A@420,A@450,A@480")

SCENARIOS = [
    Scenario("vs-2p", "2 players, versus, two full races, connected directly; each hears the other's engine", 2, "vs",
             track="mk:luigi_raceway", series_races=2, peer_env={-1: {"SK_NETPLAY_TEST_ENGINES_AT": "300"}},
             expect={"races_over": 2, "finishes": 2, "direct": 1, "engines": 1}, expect_none=("direct_lost",)),
    Scenario("vs-4p", "4 players, versus, one full race; each hears the others' engines", 4, "vs",
             track="mk:mario_raceway", series_races=1, peer_env={-1: {"SK_NETPLAY_TEST_ENGINES_AT": "300"}},
             expect={"races_over": 1, "finishes": 3, "engines": 1}),
    # A race where neither bot makes the top four is raced again, as in the game, so the cup takes 4 races or many
    # more: 600 s leaves room for about 10. The Flower Cup, as console-gp plays the Mushroom Cup.
    Scenario("gp-2p", "2 players and 6 CPUs, the Flower Cup, with the podium", 2, "gp", cup=1,
             expect={"cups": 1, "races_over": 4, "cpus_racing": 4}, timeout=600),
    Scenario("gp-4p", "4 players and 4 CPUs, the Star Cup, with the podium", 4, "gp", cup=2,
             expect={"cups": 1, "races_over": 4, "finishes": 4, "cpus_racing": 4}, timeout=600),
    # 5 to 8 players race in the eight-screen mode (SCREEN_MODE_8P); an odd count leaves a cell for the minimap.
    Scenario("gp-5p", "5 players and 3 CPUs, the Mushroom Cup, with the podium", 5, "gp", cup=0,
             expect={"cups": 1, "races_over": 4, "finishes": 4, "cpus_racing": 4}, timeout=600),
    # Two bots can go over a minute without a hit, and a battle logs nothing between hits, so 2-player battles get
    # longer before they count as stuck.
    Scenario("battle-2p", "2 players, Battle on Block Fort, until one is out", 2, "battle", track="mk:block_fort",
             series_races=1, expect={"eliminations": 1, "races_over": 1}, quiet_limit=180),
    Scenario("battle-4p", "4 players, Battle on Big Donut, until one is left", 4, "battle", track="mk:big_donut",
             series_races=1, expect={"eliminations": 3, "races_over": 1}),
    Scenario("battle-6p", "6 players, Battle on Double Deck, until one is left", 6, "battle", track="mk:double_deck",
             series_races=1, expect={"eliminations": 5, "races_over": 1}),
    Scenario("restart", "2 players, versus; the host restarts the race twice", 2, "vs", track="mk:royal_raceway",
             series_races=2, peer_env={0: {"SK_NETPLAY_TEST_CONTROLS": "restart@400"}}, expect={"segments": 3}),
    Scenario("guest-leaves", "3 players, versus; player 3 leaves mid-race and everyone returns to the room", 3, "vs",
             track="mk:koopa_troopa_beach", peer_env={2: {"SK_NETPLAY_TEST_CONTROLS": "leave@600"}},
             expect={"segments": 1}),
    Scenario("desync-recover", "3 players, versus; player 3's state goes wrong at frame 600, everyone takes over the "
             "host's state and the race goes on to its end", 3, "vs", track="mk:luigi_raceway", series_races=1,
             peer_env={2: {"SK_NETPLAY_DESYNC_AT": "600"}}, desync_expected=True,
             expect={"recoveries": 1, "races_over": 1, "finishes": 2}),
    Scenario("demo-first", "2 players, two lobby races; the host idles into the title screen's demo first, which "
             "only the title screen (gSkipIntro 1) starts", 2, "vs", track="mk:luigi_raceway", races=2,
             series_races=1, cvars={"gSkipIntro": 1}, peer_env={0: {"SK_NETPLAY_TEST_START_DELAY": "1500"}},
             expect={"races_over": 2}),
    Scenario("music-volume", "2 players, versus; a player presses L, which sets the music volume setting from inside "
             "the simulation", 2, "vs", track="mk:moo_moo_farm", series_races=1,
             peer_env={1: {"SK_NETPLAY_TEST_PRESS_L_AT": "400"}}, expect={"races_over": 1}),
    Scenario("mixed-settings", "2 players whose games took different ways through the menus (one starts at the title "
             "screen, one skips it), as two computers do", 2, "battle", track="mk:block_fort", series_races=1,
             peer_cvars={0: {"gSkipIntro": 1}}, expect={"races_over": 1}, quiet_limit=180),
    Scenario("mixed-fps", "2 players, versus, one game drawing 60 pictures a second (frame interpolation) and one the "
             "game's own 30", 2, "vs", track="mk:wario_stadium", series_races=1,
             peer_cvars={1: {"gInterpolationFPS": 60}}, expect={"races_over": 1, "finishes": 1}),
    Scenario("sounds", "3 random drivers among traffic, each peer hearing every camera; their sounds must agree "
             "however rollbacks changed them", 3, "vs", track="mk:toads_turnpike", frames=4000,
             peer_env={-1: {"SK_NETPLAY_BOT": "2", "SK_NETPLAY_TEST_SOUND_ROLLBACK_AT": "600"}},
             cvars={"gNetplay.OwnAudio": 0}, expect={"segments": 1, "sound_self_test": 1}),
    Scenario("voice", "3 players, versus, with voice chat: each hears the other two", 3, "vs",
             track="mk:luigi_raceway", series_races=1,
             peer_env={0: {"SK_NETPLAY_TEST_VOICE": "1"}, 1: {"SK_NETPLAY_TEST_VOICE": "2"},
                       2: {"SK_NETPLAY_TEST_VOICE": "3"}},
             expect={"races_over": 1, "voice_heard": 2}),
    Scenario("voice-relayed", "2 players, versus, with voice chat but direct connections turned off: voice never "
             "goes through the relay, so nobody hears anybody", 2, "vs", track="mk:moo_moo_farm", series_races=1,
             cvars={"gNetplay.DirectConnect": 0},
             peer_env={0: {"SK_NETPLAY_TEST_VOICE": "1"}, 1: {"SK_NETPLAY_TEST_VOICE": "2"}},
             expect={"races_over": 1}, expect_none=("direct", "voice_heard")),
    Scenario("voice-direct-lost", "2 players, versus, with voice chat; the guest's direct connection dies mid-race: "
             "the race goes on through the relay, the voices stop", 2, "vs", track="mk:kalimari_desert",
             series_races=1,
             # The connection dies 5 s after joining, by the clock: run slower, so that the race is still on then.
             peer_env={-1: {"SK_FAST": "3"}, 0: {"SK_NETPLAY_TEST_VOICE": "1"},
                       1: {"SK_NETPLAY_TEST_VOICE": "2", "SK_NETPLAY_TEST_BLOCK_DIRECT_MS": "5000"}},
             expect={"races_over": 1, "direct": 1, "direct_lost": 1, "voice_heard": 1, "voice_stopped": 1},
             alone=True),
    Scenario("relay-host", "2 players, versus; the host reaches the relay on its own computer, the guest by that "
             "computer's network address, as a second computer would", 2, "vs", track="mk:yoshi_valley",
             series_races=1, peer_env={1: {"SK_NETPLAY_RELAY": "{lan}:{port}"}},
             # The host's addresses are loopback ones as the relay sees them; the guest must get its network address.
             expect={"races_over": 1, "direct": 1, "network_address": 1}),
    Scenario("console-2p", "2 players on a shared console: the menus in lockstep (Game Select, characters, course, "
             "by scripted buttons), where 3 players are refused first (the room has 2 places), a full versus race, "
             "the next race, then the menus again; the host's drawing settings, which change actor state, and its "
             "rules apply to both; the guest's debug tools are off",
             2, "vs", series_races=1,
             peer_cvars={0: {"gNoCulling": 0, "gDisableLod": 0, "gDisableRubberbanding": 1},
                         1: {"gDrawObjects": 0, "gEnableDebugMode": 1}},
             peer_env={-1: {"SK_NETPLAY_CONSOLE": "1", "SK_NETPLAY_TEST_SAVE_SETTINGS": "1"},
                       0: {"SK_TEST_CONSOLE_INPUT": CONSOLE_3P_REFUSED_2P_VS},
                       1: {"SK_TEST_CONSOLE_INPUT": "R_JPAD@320,A@340"}},
             # Both save their settings during the room: each file keeps its own player's values, not the room's.
             saved_cvars={0: {"gNoCulling": 0, "gDrawObjects": None, "gEnableDebugMode": None},
                          1: {"gNoCulling": None, "gDisableLod": None, "gDrawObjects": 0, "gEnableDebugMode": 1}},
             expect={"consoles": 1, "races_over": 1, "finishes": 1, "menus_in_sync": 1}),
    Scenario("console-spectator", "3 players on a shared console pick a 2-player race: player 3 sits it out and "
             "watches player 1", 3, "vs", series_races=1,
             peer_env={-1: {"SK_NETPLAY_CONSOLE": "1"}, 0: {"SK_TEST_CONSOLE_INPUT": CONSOLE_2P_VS},
                       1: {"SK_TEST_CONSOLE_INPUT": "R_JPAD@290,A@310"}},
             expect={"consoles": 1, "races_over": 1, "menus_in_sync": 1}),
    Scenario("room-8", "8 players in one room, as many as it holds, race versus from the shared console's menus",
             8, "vs", series_races=1,
             peer_env={-1: {"SK_NETPLAY_CONSOLE": "1"}, 0: {"SK_TEST_CONSOLE_INPUT": CONSOLE_8P_VS},
                       **{p: {"SK_TEST_CONSOLE_INPUT": f"R_JPAD@{288 + 2 * p},A@{306 + 4 * p}"} for p in range(1, 8)}},
             expect={"consoles": 1, "races_over": 1, "finishes": 7, "menus_in_sync": 1}, timeout=420),
    Scenario("local-2p", "2 computers, 3 players: the first has 2 players (controllers 1 and 2) and the second 1; a "
             "3-player versus race from the shared console's menus", 2, "vs", series_races=1,
             peer_env={-1: {"SK_NETPLAY_CONSOLE": "1"},
                       0: {"SK_TEST_LOCAL_PLAYERS": "2", "SK_TEST_CONSOLE_INPUT": CONSOLE_3P_VS, "SK_TEST_CONSOLE_INPUT_2": "R_JPAD@290,A@310"},
                       1: {"SK_TEST_CONSOLE_INPUT": "R_JPAD@292,A@314"}},
             expect={"consoles": 1, "races_over": 1, "finishes": 1, "menus_in_sync": 1, "laps": 9}),
    Scenario("console-gp", "2 players on a shared console: a whole Grand Prix from the menus, the podium, then "
             "Game Select again", 2, "gp",
             peer_env={-1: {"SK_NETPLAY_CONSOLE": "1"}, 0: {"SK_TEST_CONSOLE_INPUT": CONSOLE_2P_GP},
                       1: {"SK_TEST_CONSOLE_INPUT": "R_JPAD@260,A@280"}},
             expect={"consoles": 1, "cups": 1, "podiums": 1, "menus_in_sync": 1}, timeout=600),
    Scenario("console-gp-3p", "3 players on a shared console: Grand Prix chosen on Game Select, the whole cup and "
             "the podium", 3, "gp",
             peer_env={-1: {"SK_NETPLAY_CONSOLE": "1"}, 0: {"SK_TEST_CONSOLE_INPUT": CONSOLE_3P_GP},
                       1: {"SK_TEST_CONSOLE_INPUT": "R_JPAD@260,A@280"},
                       2: {"SK_TEST_CONSOLE_INPUT": "R_JPAD@262,A@284"}},
             expect={"consoles": 1, "cups": 1, "podiums": 1, "menus_in_sync": 1, "cpus_racing": 4}, timeout=600),
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
    Scenario("pause", "2 players, versus; the guest pauses the race for both, then picks CONTINUE GAME in its pause "
             "menu, and the race goes on to its end", 2, "vs", track="mk:moo_moo_farm", series_races=1,
             peer_env={1: {"SK_NETPLAY_TEST_RACE_INPUT": "START@300,A@420"}},
             expect={"paused": 1, "went_on": 1, "races_over": 1}),
    Scenario("console-pause", "2 players on a shared console; the guest pauses and picks COURSE CHANGE, which takes "
             "everyone back to the course select menu, in lockstep", 2, "vs", races=1,
             peer_env={-1: {"SK_NETPLAY_CONSOLE": "1"}, 0: {"SK_TEST_CONSOLE_INPUT": CONSOLE_2P_VS},
                       1: {"SK_TEST_CONSOLE_INPUT": "R_JPAD@290,A@310",
                           "SK_NETPLAY_TEST_RACE_INPUT": "START@300,D_JPAD@360,A@420"}},
             expect={"paused": 1, "consoles": 1, "menus_in_sync": 1}),
    Scenario("private-room", "2 players, versus, in a private room: not listed, the guest joins by its code",
             2, "vs", track="mk:royal_raceway", series_races=1, peer_env={0: {"SK_NETPLAY_PRIVATE": "1"}},
             expect={"races_over": 1, "private_room": 1}),
    Scenario("matchmake", "2 players matchmake: the relay puts them in a new room, hosted by the one who waited "
             "longest, and they race on its shared console", 2, "vs", series_races=1,
             peer_env={-1: {"SK_NETPLAY_CONSOLE": "1", "SK_NETPLAY_MATCHMAKE": "1"},
                       0: {"SK_TEST_CONSOLE_INPUT": CONSOLE_2P_VS},
                       1: {"SK_TEST_CONSOLE_INPUT": "R_JPAD@290,A@310", "SK_NETPLAY_TEST_JOIN_AT": "600"}},
             expect={"matched": 1, "consoles": 1, "races_over": 1, "menus_in_sync": 1}),
    Scenario("matchmake-fill", "A public room on a shared console; 2 players matchmake and the relay fills the room "
             "with them, rather than pairing them in a room of their own", 3, "vs", races=0,
             peer_env={-1: {"SK_NETPLAY_CONSOLE": "1", "SK_NETPLAY_TEST_CONSOLE_PLAYERS": "3"},
                       1: {"SK_NETPLAY_MATCHMAKE": "1", "SK_NETPLAY_TEST_JOIN_AT": "600"},
                       2: {"SK_NETPLAY_MATCHMAKE": "1", "SK_NETPLAY_TEST_JOIN_AT": "600"}},
             expect={"consoles": 1, "menus_in_sync": 1}),
    Scenario("matchmake-away", "2 players matchmake; the second is matched while its title screen's attract demo "
             "runs, as if racing offline: the room waits until it is back in the menus, then starts its console",
             2, "vs", series_races=1, peer_cvars={1: {"gSkipIntro": 1}},
             peer_env={-1: {"SK_NETPLAY_CONSOLE": "1", "SK_NETPLAY_MATCHMAKE": "1"},
                       0: {"SK_TEST_CONSOLE_INPUT": CONSOLE_2P_VS},
                       1: {"SK_TEST_CONSOLE_INPUT": "R_JPAD@290,A@310", "SK_NETPLAY_MATCHMAKE": "demo"}},
             expect={"matched": 1, "consoles": 1, "races_over": 1, "menus_in_sync": 1},
             peer_expect={0: {"waited_for_away": 1}, 1: {"matched_away": 1}}),
    Scenario("lag", "2 players, versus at 60 ms +-20 ms and 5% loss, in real time", 2, "vs",
             track="mk:frappe_snowland", frames=1800,
             peer_env={-1: {"SK_FAST": "0", "SK_NETPLAY_DELAY_MS": "60", "SK_NETPLAY_JITTER_MS": "20",
                            "SK_NETPLAY_LOSS": "5"}},
             expect={"laps": 2}, alone=True),
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


# Every game and relay still running, so that Ctrl-C stops those of every scenario running side by side
running_lock = threading.Lock()
running_processes = set()
used_ports = set()


def start_process(*popen_args, **popen_kwargs) -> subprocess.Popen:
    process = subprocess.Popen(*popen_args, **popen_kwargs)
    with running_lock:
        running_processes.add(process)
    return process


def stop_all_processes():
    with running_lock:
        processes = list(running_processes)
    for process in processes:
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                process.kill()


def pick_relay_port() -> int:
    """A port no other scenario of this run uses, free for both UDP and TCP (the relay's WebSocket)."""
    while True:
        port = random.randint(20000, 40000)
        with running_lock:
            if port in used_ports:
                continue
            used_ports.add(port)
        try:
            for kind in (socket.SOCK_DGRAM, socket.SOCK_STREAM):
                with socket.socket(socket.AF_INET, kind) as probe:
                    probe.bind(("", port))
            return port
        except OSError:
            continue


def run_scenario(scenario: Scenario, args, run_dir: Path):
    directory = run_dir / scenario.name
    directory.mkdir(parents=True)
    port = pick_relay_port()
    relay_args = [str(args.relay), "--port", str(port), "--no-rate-limits"]
    delay, jitter, loss = scenario.lag
    if delay or jitter or loss:
        relay_args += ["--delay-ms", str(delay), "--jitter-ms", str(jitter), "--loss", str(loss)]
    relay_out = open(directory / "relay.out", "w")
    relay = start_process(relay_args, stdout=relay_out, stderr=subprocess.STDOUT, start_new_session=True)
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
        # SK_TEST_SCREENSHOTS given to the simulator: each game saves its pictures in its own folder.
        env.setdefault("SK_TEST_SCREENSHOT_DIR", str(home))
        for name, value in {**scenario.peer_env.get(-1, {}), **scenario.peer_env.get(i, {})}.items():
            env[name] = value.format(port=port, lan=lan_address() if "{lan}" in value else "")
        output_path = directory / f"p{i}.out"
        outputs.append(output_path)
        with open(output_path, "w") as output:
            # After a game crashes, macOS asks whether to reopen its windows at the next start, and the game waits
            # for an answer; this skips that.
            games.append(start_process([str(args.exe), "-ApplePersistenceIgnoreState", "YES"], env=env,
                                       stdout=output, stderr=subprocess.STDOUT, start_new_session=True))
        # The host creates the room and writes its code; guests wait for it.
        time.sleep(1.0 if i == 0 else 0.3)

    stopped = wait_for_games(games, outputs, started + scenario.timeout, scenario.quiet_limit)
    relay.kill()
    relay.wait()
    relay_out.close()
    with running_lock:
        running_processes.difference_update(games + [relay])
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
            if ("DESYNC" in line) and not scenario.desync_expected:
                problems.append(f"p{i}: {line[10:]}")
        for event, minimum in {**scenario.expect, **scenario.peer_expect.get(i, {})}.items():
            count = len(EVENTS[event].findall(text))
            if count < minimum:
                problems.append(f"p{i}: {count} {event}, expected at least {minimum}")
        for event in scenario.expect_none:
            count = len(EVENTS[event].findall(text))
            if count:
                problems.append(f"p{i}: {count} {event}, expected none")
        if scenario.saved_cvars.get(i):
            saved = json.loads((directory / f"home{i}" / "spaghettify.cfg.json").read_text()).get("CVars", {})
            for name, expected in scenario.saved_cvars[i].items():
                if saved.get(name) != expected:
                    problems.append(f"p{i}: settings file has {name} = {saved.get(name)}, expected {expected}")

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
    parser.add_argument("--max-games", type=int, default=max(1, min(8, (os.cpu_count() or 2) - 2)),
                        help="games running at once, across the scenarios run side by side (default: the cores "
                             "less 2, at most 8); a scenario with more players than this runs alone")
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
    started = time.monotonic()
    results = run_side_by_side(selected, args, run_dir)

    failed = [r for r in results if not r["passed"]]
    if failed:
        print(f"\nFailed: {', '.join(r['name'] for r in failed)}")
    minutes = (time.monotonic() - started) / 60
    print(f"\n{len(results) - len(failed)} of {len(results)} scenarios passed, in {minutes:.0f} minutes.")
    return 1 if failed else 0


def run_side_by_side(selected, args, run_dir: Path):
    """Runs the scenarios, as many at once as --max-games allows, and prints each result as it ends.

    The longest (by time limit) start first, so that the short ones fill the gaps around them rather than a long one
    running alone at the end. A scenario marked alone, or with more players than --max-games, waits for the others to
    end and runs by itself."""
    pending = sorted(selected, key=lambda scenario: -scenario.timeout)
    running = {}  # future -> scenario
    results = []
    with futures.ThreadPoolExecutor(max_workers=max(1, args.max_games)) as pool:
        try:
            while pending or running:
                games = sum(scenario.players for scenario in running.values())
                alone_running = any(scenario.alone for scenario in running.values())
                for scenario in list(pending):
                    runs_alone = scenario.alone or (scenario.players > args.max_games)
                    if alone_running or (runs_alone and running) or (games + scenario.players > args.max_games
                                                                      and running):
                        continue
                    pending.remove(scenario)
                    running[pool.submit(run_scenario, scenario, args, run_dir)] = scenario
                    games += scenario.players
                    alone_running = runs_alone
                    if runs_alone:
                        break
                done, _ = futures.wait(running, return_when=futures.FIRST_COMPLETED)
                for future in done:
                    running.pop(future)
                    result = future.result()
                    results.append(result)
                    verdict = "PASS" if result["passed"] else "FAIL"
                    print(f"{result['name']:24} {verdict}  {result['elapsed']:5.0f} s  "
                          f"{summarize_events(result['counts'])}", flush=True)
                    for problem in result["problems"]:
                        print(f"{'':26}- {problem}", flush=True)
        except KeyboardInterrupt:
            # The worker threads wait for their games, so stop the games before the pool waits for the threads.
            stop_all_processes()
            raise
    return results


if __name__ == "__main__":
    sys.exit(main())
