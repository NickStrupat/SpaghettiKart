#!/usr/bin/env python3
"""Protocol tests for the netplay relay. Starts the relay binary on a free port and talks to it over UDP, and over a
WebSocket as a browser would.

Usage: test_relay.py <path to netplay-relay>
"""
import base64
import hashlib
import os
import socket
import struct
import subprocess
import sys
import time

MAGIC = 0x4B53
VERSION = 5
MAX_SLOTS = 8  # RelayProtocol::kMaxSlots
CREATE, JOIN, RELAY, KEEPALIVE, LEAVE, LIST_ROOMS, MATCHMAKE, MATCH_CANCEL = 1, 2, 3, 4, 5, 6, 7, 8
JOINED, ERROR, RELAYED, ROOM_STATE, ROOM_CLOSED, ROOM_LIST, MATCH_WAITING = 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87
NO_SUCH_ROOM, ROOM_FULL, BUILD_MISMATCH, CONTENT_MISMATCH, RATE_LIMITED, UNKNOWN_CLIENT = 1, 2, 3, 4, 5, 7
HOST_LEFT = 1
SHARE_ADDRESSES, IN_RACE = 1, 2
CREATE_PRIVATE = 1
ROOM_RACING, ROOM_PRIVATE, ROOM_MATCHMADE = 1, 2, 4
CODE_LENGTH = 7
BUILD = b"test-build".ljust(32, b"\0")
CONTENT = 0x1234


def header(kind):
    return struct.pack("<HBB", MAGIC, VERSION, kind)


class Peer:
    def __init__(self, port):
        self.address = ("127.0.0.1", port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(1.0)
        self.token = None
        self.slot = None
        self.code = None
        self.flags = None

    def send(self, data):
        self.sock.sendto(data, self.address)

    def receive(self, kind):
        """Returns the body of the next message of this kind, skipping others (such as RoomState broadcasts)."""
        deadline = time.time() + 1.0
        while time.time() < deadline:
            data, _ = self.sock.recvfrom(2048)
            magic, version, got = struct.unpack_from("<HBB", data)
            assert (magic, version) == (MAGIC, VERSION)
            if got == kind:
                return data[4:]
        raise AssertionError(f"no message of type {kind:#x}")

    def nothing_received(self):
        self.sock.settimeout(0.2)
        try:
            data, _ = self.sock.recvfrom(2048)
            return struct.unpack_from("<B", data, 3)[0] == ROOM_STATE  # Only membership updates are expected
        except socket.timeout:
            return True
        finally:
            self.sock.settimeout(1.0)

    def joined(self, body):
        """@returns the request the Joined answers."""
        request, self.token, self.code, self.slot, self.flags = struct.unpack("<IQ7sBB", body)
        return request

    def create(self, request=1, name=b"Host", build=BUILD, private=False):
        flags = CREATE_PRIVATE if private else 0
        self.send(header(CREATE) + struct.pack("<I", request) + build + struct.pack("<Q16sB", CONTENT, name, flags))
        self.joined(self.receive(JOINED))

    def matchmake(self, request=1, name=b"Player", build=BUILD):
        self.send(header(MATCHMAKE) + struct.pack("<I", request) + build + struct.pack("<Q16s", CONTENT, name))

    def waiting(self):
        """@returns (request, players waiting) from the next MatchWaiting."""
        return struct.unpack("<IH", self.receive(MATCH_WAITING))

    def cancel_match(self, request=1):
        self.send(header(MATCH_CANCEL) + struct.pack("<I", request))

    def list_rooms(self, request=1, build=BUILD, content=CONTENT):
        """@returns [(code, host name, members, flags)] for each listed room. Sets other_builds: the rooms left out for
        being of another build, and other_content: those of this build with other game files."""
        self.send(header(LIST_ROOMS) + struct.pack("<I", request) + build + struct.pack("<Q", content))
        body = self.receive(ROOM_LIST)
        got_request, count = struct.unpack_from("<IB", body)
        assert got_request == request, "the list answers its request"
        rooms = []
        for i in range(count):
            code, name, members, flags = struct.unpack_from("<7s16sBB", body, 5 + i * 25)
            rooms.append((code, name.rstrip(b"\0"), members, flags))
        self.other_builds, self.other_content = struct.unpack_from("<BB", body, 5 + count * 25)
        return rooms

    def join(self, code, request=1, build=BUILD, content=CONTENT):
        self.send(header(JOIN) + struct.pack("<I7s", request, code) + build + struct.pack("<Q", content))

    def error(self):
        return struct.unpack("<IB", self.receive(ERROR))[1]

    def relay(self, destination, payload):
        self.send(header(RELAY) + struct.pack("<QB", self.token, destination) + payload)

    def room_state(self, echo=0, share=False, local=(0, 0), racing=False):
        """@returns (own slot, occupied mask, [(key, public address, local address)] per occupied slot)."""
        flags = (SHARE_ADDRESSES if share else 0) | (IN_RACE if racing else 0)
        self.send(header(KEEPALIVE) + struct.pack("<QIBIH", self.token, echo, flags, *local))
        while True:
            body = self.receive(ROOM_STATE)
            got_echo, slot, mask = struct.unpack_from("<IBB", body)
            if got_echo != echo:
                continue
            members = []
            for offset in range(6, len(body), 20):
                key, public_ip, public_port, local_ip, local_port = struct.unpack_from("<QIHIH", body, offset)
                members.append((key, (public_ip, public_port), (local_ip, local_port)))
            assert len(members) == bin(mask).count("1"), "one entry per member"
            return slot, mask, members


class WebSocketPeer(Peer):
    """A browser's connection: the same messages, each in one binary WebSocket message over TCP."""

    def __init__(self, port, extra_headers="", receive_buffer=None):
        """@param receive_buffer Size of the socket's receive buffer, to make a browser that falls behind sooner."""
        super().__init__(port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if receive_buffer is not None:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        self.sock.settimeout(1.0)
        self.sock.connect(("127.0.0.1", port))
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\n"
                           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
                           f"{extra_headers}\r\n").encode())
        answer = b""
        while b"\r\n\r\n" not in answer:
            answer += self.sock.recv(1024)
        expected = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        assert answer.startswith(b"HTTP/1.1 101"), "the relay accepts the WebSocket"
        assert b"Sec-WebSocket-Accept: " + expected in answer, "with the accept key RFC 6455 asks for"
        self.pending = answer.split(b"\r\n\r\n", 1)[1]

    def send_frame(self, opcode, data):
        mask = os.urandom(4)
        length = bytes([0x80 | len(data)]) if len(data) < 126 else bytes([0x80 | 126]) + struct.pack(">H", len(data))
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        self.sock.sendall(bytes([0x80 | opcode]) + length + mask + masked)

    def send(self, data):
        self.send_frame(0x2, data)

    def read_exact(self, size):
        while len(self.pending) < size:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("closed")
            self.pending += chunk
        data, self.pending = self.pending[:size], self.pending[size:]
        return data

    def read_frame(self):
        """@returns (opcode, payload) of the next frame from the relay, which sends them unmasked."""
        first, second = self.read_exact(2)
        assert second & 0x80 == 0, "server frames are not masked"
        length = second & 0x7F
        if length == 126:
            length = struct.unpack(">H", self.read_exact(2))[0]
        return first & 0x0F, self.read_exact(length)

    def receive(self, kind):
        deadline = time.time() + 1.0
        while time.time() < deadline:
            opcode, data = self.read_frame()
            if opcode != 0x2:
                continue
            magic, version, got = struct.unpack_from("<HBB", data)
            assert (magic, version) == (MAGIC, VERSION)
            if got == kind:
                return data[4:]
        raise AssertionError(f"no message of type {kind:#x}")


def test_websockets(port):
    page = socket.create_connection(("127.0.0.1", port), timeout=1.0)
    page.sendall(b"GET / HTTP/1.1\r\nHost: relay\r\n\r\n")
    assert page.recv(1024).startswith(b"HTTP/1.1 400"), "a plain HTTP request is turned away"
    page.close()

    browser = WebSocketPeer(port)
    browser.create(name=b"Browser")
    assert browser.slot == 0, "a browser can host"
    desktop = Peer(port)
    assert desktop.list_rooms(request=20)[0][:2] == (browser.code, b"Browser"), "and its room is listed over UDP"
    desktop.join(browser.code, request=21)
    desktop.joined(desktop.receive(JOINED))
    desktop.relay(0, b"to the browser")
    assert browser.receive(RELAYED) == b"\x01to the browser", "UDP reaches a WebSocket member"
    browser.relay(1, b"to the desktop")
    assert desktop.receive(RELAYED) == b"\x00to the desktop", "and the other way"
    browser.room_state(echo=3, share=True, local=(0x0A000003, 5000))
    members = desktop.room_state(echo=4)[2]
    assert members[0][1:] == ((0, 0), (0, 0)), "a browser has no address to share, even when it asks"

    browser.send_frame(0x9, b"ping")
    while True:
        opcode, payload = browser.read_frame()
        if opcode == 0xA:
            break
    assert payload == b"ping", "pings are answered"

    guest = WebSocketPeer(port)
    guest.join(browser.code, request=22)
    guest.joined(guest.receive(JOINED))
    assert guest.slot == 2
    guest.sock.close()
    time.sleep(0.2)
    assert desktop.room_state(echo=5)[:2] == (1, 0b011), "a closed WebSocket frees its slot at once"
    browser.sock.close()
    assert desktop.receive(ROOM_CLOSED) == bytes([HOST_LEFT]), "and a browser host's room closes with it"

    # The relay runs with --trusted-proxy 127.0.0.1: these come from "the proxy".
    proxied = WebSocketPeer(port, "X-Forwarded-For: 198.51.100.1, 203.0.113.7\r\n")
    proxied.create(request=30, name=b"Proxied")
    print("relay WebSocket tests: PASS")


def unused_code(*taken):
    """@returns a valid code that none of these rooms has."""
    code = b"U" * CODE_LENGTH
    while code in taken:
        code = code[:-1] + b"DLR"[b"UDL".index(code[-1:])].to_bytes(1, "little")
    return code


def test_private_rooms(port):
    build = b"private-build".ljust(32, b"\0")
    asker = Peer(port)
    asker.list_rooms(request=39, build=b"probe".ljust(32, b"\0"))
    others_before = asker.other_builds
    host = Peer(port)
    host.create(request=40, name=b"Secret", build=build, private=True)
    assert host.flags == ROOM_PRIVATE, "a private room says so in Joined"
    public = Peer(port)
    public.create(request=41, name=b"Open", build=build)
    assert public.flags == 0, "a room created without the flag is public"
    assert [room[1] for room in asker.list_rooms(request=42, build=build)] == [b"Open"], "private rooms are not listed"
    asker.list_rooms(request=44, build=b"probe".ljust(32, b"\0"))
    assert asker.other_builds == others_before + 1, "nor counted among the rooms of other builds"
    guest = Peer(port)
    guest.join(host.code, request=43, build=build)
    guest.joined(guest.receive(JOINED))
    assert (guest.slot, guest.flags) == (1, ROOM_PRIVATE), "a private room is joined by its code"
    for peer in (host, public):
        peer.send(header(LEAVE) + struct.pack("<Q", peer.token))
    print("relay private room tests: PASS")


def test_matchmaking(port):
    build = b"match-build".ljust(32, b"\0")
    first = Peer(port)
    first.matchmake(request=50, name=b"First", build=build)
    assert first.waiting() == (50, 1), "a player alone waits, and is told how many are looking"
    first.matchmake(request=50, name=b"First", build=build)
    assert first.waiting() == (50, 1), "a repeat keeps the same place in the queue"
    other_build = Peer(port)
    other_build.matchmake(request=51, build=b"match-other".ljust(32, b"\0"))
    assert other_build.waiting() == (51, 1), "players of another build are not matched"

    second = Peer(port)
    second.matchmake(request=52, name=b"Second", build=build)
    assert second.joined(second.receive(JOINED)) == 52
    assert first.joined(first.receive(JOINED)) == 50, "the player waiting is told at once"
    assert (first.slot, second.slot) == (0, 1), "the player who waited longest hosts"
    assert first.code == second.code and first.flags == ROOM_MATCHMADE == second.flags, "both are in a matchmade room"
    token = first.token
    first.matchmake(request=50, name=b"First", build=build)
    first.joined(first.receive(JOINED))
    assert first.token == token, "a repeat after the match gets the same Joined"
    rooms = Peer(port).list_rooms(request=53, build=build)
    assert rooms == [(first.code, b"First", 2, 0)], "a matchmade room is public, named after its host"

    # Matchmaking fills the public room with the most players first, never a private or racing one.
    private = Peer(port)
    private.create(request=54, build=build, private=True)
    racing = Peer(port)
    racing.create(request=55, build=build)
    racing.room_state(echo=1, racing=True)
    third = Peer(port)
    third.matchmake(request=56, build=build)
    third.joined(third.receive(JOINED))
    assert (third.code, third.slot) == (first.code, 2), "a matchmaker joins the fullest open public room"
    lonely = Peer(port)
    lonely.create(request=57, build=build)
    fourth = Peer(port)
    fourth.matchmake(request=58, build=build)
    fourth.joined(fourth.receive(JOINED))
    assert (fourth.code, fourth.slot) == (first.code, 3), "and fills it up before an emptier one"
    for slot in range(4, MAX_SLOTS):
        filler = Peer(port)
        filler.matchmake(request=100 + slot, build=build)
        filler.joined(filler.receive(JOINED))
        assert (filler.code, filler.slot) == (first.code, slot), "every slot of the room is filled"
    time.sleep(3)  # Every test client shares one address: let its matchmaking allowance refill (2 a second)
    fifth = Peer(port)
    fifth.matchmake(request=59, build=build)
    fifth.joined(fifth.receive(JOINED))
    assert (fifth.code, fifth.slot) == (lonely.code, 1), "then the next open room, never the private or racing one"
    for peer in (first, private, racing, lonely):
        peer.send(header(LEAVE) + struct.pack("<Q", peer.token))
    time.sleep(0.1)

    waiting = Peer(port)
    waiting.matchmake(request=60, build=build)
    assert waiting.waiting() == (60, 1)
    waiting.cancel_match(request=60)
    late = Peer(port)
    late.matchmake(request=61, build=build)
    assert late.waiting() == (61, 1), "a cancelled player leaves the queue"
    matched = Peer(port)
    matched.matchmake(request=62, build=build)
    matched.joined(matched.receive(JOINED))
    late.joined(late.receive(JOINED))
    matched.cancel_match(request=62)
    assert late.receive(ROOM_STATE), "cancelling after a match leaves the room"
    assert late.room_state(echo=2)[:2] == (0, 0b01)
    print("relay matchmaking tests: PASS")


def test_failed_join_limit(port):
    host = Peer(port)
    host.create(request=70)
    guesser = Peer(port)
    wrong = unused_code(host.code)
    for request in range(71, 81):
        guesser.join(wrong, request=request)
        assert guesser.error() == NO_SUCH_ROOM
    guesser.join(wrong, request=81)
    assert guesser.error() == RATE_LIMITED, "after 10 joins of rooms that do not exist, joins are refused"
    guesser.join(host.code, request=82)
    assert guesser.error() == RATE_LIMITED, "even with the right code, so that guessing stays slow"
    print("relay failed join limit test: PASS")


def test_moved_clients(port):
    """A client whose address changed, then left, is forgotten under the address it joined from too: a repeat of its
    first request from there is a new request, not an answer about a client that is gone (which stopped the relay)."""
    host = Peer(port)
    host.create(request=90)
    first_token = host.token
    guest = Peer(port)
    guest.join(host.code, request=91)
    guest.joined(guest.receive(JOINED))
    # The same tokens from other sockets: the relay takes both clients to have moved there (NAT rebinding).
    moved_host, moved_guest = Peer(port), Peer(port)
    moved_host.token, moved_guest.token = host.token, guest.token
    moved_guest.room_state(echo=1)
    moved_host.room_state(echo=2)
    moved_host.send(header(LEAVE) + struct.pack("<Q", host.token))
    assert moved_guest.receive(ROOM_CLOSED) == bytes([HOST_LEFT])
    host.create(request=90)
    assert host.slot == 0 and host.token != first_token, "a repeat from the old address of a host that left is new"
    guest.join(host.code, request=91)
    guest.joined(guest.receive(JOINED))
    assert guest.slot == 1, "and of a guest its room closed under"
    print("relay moved clients test: PASS")


def test_stuck_browser_host(port):
    """A browser that hosts and stops reading is closed once its queue is full, with its room, without the relay
    using the room after it is gone (an AddressSanitizer build of the relay reports it if it does)."""
    browser = WebSocketPeer(port, receive_buffer=4096)
    browser.create(request=95)
    guest = Peer(port)
    guest.join(browser.code, request=96)
    guest.joined(guest.receive(JOINED))
    payload = b"x" * 1100
    closed = False
    for _ in range(200):
        for _ in range(200):
            guest.relay(0xFF, payload)
        guest.sock.settimeout(0.01)
        try:
            while not closed:
                data, _ = guest.sock.recvfrom(2048)
                closed = data[3] == ROOM_CLOSED
        except socket.timeout:
            pass
        finally:
            guest.sock.settimeout(1.0)
        if closed:
            break
    assert closed, "the room closes once its host's browser falls too far behind"
    Peer(port).create(request=97)
    print("relay stuck browser host test: PASS")


def start_relay(*arguments):
    """@returns (process, port) of a relay running on a free port."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    relay = subprocess.Popen([sys.argv[1], "--port", str(port), *arguments], stdout=subprocess.PIPE, text=True)
    assert "listening" in relay.stdout.readline(), "relay started"
    return relay, port


def main():
    # The failed join limit is tested on a relay of its own: the other tests join from the same address.
    relay, port = start_relay()
    try:
        test_failed_join_limit(port)
    finally:
        relay.terminate()
        relay.wait()

    # These fill a browser's queue, faster than the rate limits allow.
    relay, port = start_relay("--no-rate-limits")
    try:
        test_moved_clients(port)
        test_stuck_browser_host(port)
    finally:
        relay.terminate()
        relay.wait()
        log = relay.stdout.read()
    assert "closed: host too slow to read" in log, "the stuck browser's room closes for that reason"

    relay, port = start_relay("--trusted-proxy", "127.0.0.1")
    try:
        host = Peer(port)
        host.create()
        assert host.slot == 0 and len(host.code) == CODE_LENGTH, "create gives slot 0 and a code"
        assert set(host.code) <= set(b"UDLR"), "codes are stick directions"
        token = host.token
        host.create()
        assert host.token == token, "a repeated create gets the same reply"

        browser = Peer(port)
        Peer(port).create(name=b"Other build", build=b"other".ljust(32, b"\0"))
        assert browser.list_rooms() == [(host.code, b"Host", 1, 0)], "only rooms of the same build are listed"
        assert browser.other_builds == 1, "the rooms of other builds are counted"
        assert browser.other_content == 0, "a room of another build is not counted as other content too"
        assert browser.list_rooms(request=2, content=CONTENT + 1) == [], "and of the same content"
        assert browser.other_builds == 1, "a room of other content but the same build is not another build's"
        assert browser.other_content == 1, "and is counted as other content"
        host.room_state(echo=9, racing=True)
        assert browser.list_rooms(request=3)[0][3] == ROOM_RACING, "the host's keep-alive marks the room racing"
        host.room_state(echo=10)

        guest = Peer(port)
        guest.join(unused_code(host.code))
        assert guest.error() == NO_SUCH_ROOM
        guest.join(b"ZZZZZZZ")
        assert guest.error() == NO_SUCH_ROOM, "a code with other letters names no room"
        guest.join(host.code.lower(), request=2, build=b"other".ljust(32, b"\0"))
        assert guest.error() == BUILD_MISMATCH, "codes are case-insensitive; build is checked"
        guest.join(host.code, request=3, content=CONTENT + 1)
        assert guest.error() == CONTENT_MISMATCH
        guest.join(host.code, request=4)
        guest.joined(guest.receive(JOINED))
        assert guest.slot == 1
        slot, mask, members = host.room_state(echo=7)
        assert (slot, mask) == (0, 0b11), "keepalive echoes and reports members"
        assert members[0][0] != members[1][0], "each member has its own key"
        assert members[1][1:] == ((0, 0), (0, 0)), "addresses stay hidden until shared"
        guest.room_state(echo=5, share=True, local=(0x0A000002, 4000))
        members = host.room_state(echo=6)[2]
        loopback = (127 << 24) | 1
        assert members[1][1] == (loopback, guest.sock.getsockname()[1]), "the public address is where the relay sees it"
        assert members[1][2] == (0x0A000002, 4000), "the local address is the one reported"
        assert members[0][1:] == ((0, 0), (0, 0)), "a member that does not share stays hidden"

        guest.relay(0, b"hello")
        assert host.receive(RELAYED) == b"\x01hello", "relayed payload carries the source slot"
        host.relay(0xFF, b"all")
        assert guest.receive(RELAYED) == b"\x00all"
        host.relay(0, b"self")
        assert host.nothing_received(), "packets are never sent back to their sender"

        others = [Peer(port) for _ in range(MAX_SLOTS - 1)]
        for i, other in enumerate(others):
            other.join(host.code, request=10 + i)
        for other in others[:-1]:
            other.joined(other.receive(JOINED))
        assert [other.slot for other in others[:-1]] == list(range(2, MAX_SLOTS))
        assert others[-1].error() == ROOM_FULL
        assert browser.list_rooms(request=4) == [], "full rooms are not listed"
        full_state = host.room_state(echo=7)
        assert full_state[:2] == (0, (1 << MAX_SLOTS) - 1) and len(full_state[2]) == MAX_SLOTS, \
            "the room state lists every member of a full room"

        others[0].send(header(LEAVE) + struct.pack("<Q", others[0].token))
        time.sleep(0.1)
        assert host.room_state(echo=8)[:2] == (0, ((1 << MAX_SLOTS) - 1) & ~0b100), "a guest leaving frees its slot"
        others[0].relay(0, b"x")
        assert others[0].error() == UNKNOWN_CLIENT

        host.send(header(LEAVE) + struct.pack("<Q", host.token))
        assert guest.receive(ROOM_CLOSED) == bytes([HOST_LEFT]), "the room closes when the host leaves"
        guest.relay(0, b"x")
        assert guest.error() == UNKNOWN_CLIENT
        print("relay protocol tests: PASS")
        test_private_rooms(port)
        test_matchmaking(port)
        test_websockets(port)
    finally:
        relay.terminate()
        relay.wait()
        log = relay.stdout.read()
        print(log, end="")
    assert "created by WebSocket" in log and "from 203.0.113.7" in log, \
        "a WebSocket from the trusted proxy counts as the last address in X-Forwarded-For"
    print("relay proxy test: PASS")


if __name__ == "__main__":
    main()
