#!/usr/bin/env python3
"""Protocol tests for the netplay relay. Starts the relay binary on a free port and talks to it over UDP.

Usage: test_relay.py <path to netplay-relay>
"""
import socket
import struct
import subprocess
import sys
import time

MAGIC = 0x4B53
VERSION = 2
CREATE, JOIN, RELAY, KEEPALIVE, LEAVE = 1, 2, 3, 4, 5
JOINED, ERROR, RELAYED, ROOM_STATE, ROOM_CLOSED = 0x81, 0x82, 0x83, 0x84, 0x85
NO_SUCH_ROOM, ROOM_FULL, BUILD_MISMATCH, CONTENT_MISMATCH, UNKNOWN_CLIENT = 1, 2, 3, 4, 7
HOST_LEFT = 1
SHARE_ADDRESSES = 1
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
        _, self.token, code, self.slot = struct.unpack("<IQ6sB", body)
        self.code = code

    def create(self, request=1):
        self.send(header(CREATE) + struct.pack("<I", request) + BUILD + struct.pack("<Q", CONTENT))
        self.joined(self.receive(JOINED))

    def join(self, code, request=1, build=BUILD, content=CONTENT):
        self.send(header(JOIN) + struct.pack("<I6s", request, code) + build + struct.pack("<Q", content))

    def error(self):
        return struct.unpack("<IB", self.receive(ERROR))[1]

    def relay(self, destination, payload):
        self.send(header(RELAY) + struct.pack("<QB", self.token, destination) + payload)

    def room_state(self, echo=0, share=False, local=(0, 0)):
        """@returns (own slot, occupied mask, [(key, public address, local address)] per occupied slot)."""
        flags = SHARE_ADDRESSES if share else 0
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


def main():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    relay = subprocess.Popen([sys.argv[1], "--port", str(port)], stdout=subprocess.PIPE, text=True)
    assert "listening" in relay.stdout.readline(), "relay started"
    try:
        host = Peer(port)
        host.create()
        assert host.slot == 0 and len(host.code) == 6, "create gives slot 0 and a code"
        token = host.token
        host.create()
        assert host.token == token, "a repeated create gets the same reply"

        guest = Peer(port)
        guest.join(b"ZZZZZZ")
        assert guest.error() == NO_SUCH_ROOM
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

        others = [Peer(port) for _ in range(3)]
        for i, other in enumerate(others):
            other.join(host.code, request=10 + i)
        others[0].joined(others[0].receive(JOINED))
        others[1].joined(others[1].receive(JOINED))
        assert (others[0].slot, others[1].slot) == (2, 3)
        assert others[2].error() == ROOM_FULL

        others[0].send(header(LEAVE) + struct.pack("<Q", others[0].token))
        time.sleep(0.1)
        assert host.room_state(echo=8)[:2] == (0, 0b1011), "a guest leaving frees its slot"
        others[0].relay(0, b"x")
        assert others[0].error() == UNKNOWN_CLIENT

        host.send(header(LEAVE) + struct.pack("<Q", host.token))
        assert guest.receive(ROOM_CLOSED) == bytes([HOST_LEFT]), "the room closes when the host leaves"
        guest.relay(0, b"x")
        assert guest.error() == UNKNOWN_CLIENT
        print("relay protocol tests: PASS")
    finally:
        relay.terminate()
        relay.wait()
        print(relay.stdout.read(), end="")


if __name__ == "__main__":
    main()
