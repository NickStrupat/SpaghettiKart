# Netplay relay server

Players meet through this relay. The host creates a room and gets a 6-character code. The other players join with that code, and the relay then forwards packets between the members of the room. It is a small, single-threaded UDP server and knows nothing about the game.

The wire format is described in `RelayProtocol.h`, which the game includes too.

## Run it

```sh
cmake -S tools/netplay-relay -B build-relay
cmake --build build-relay
./build-relay/netplay-relay --port 7777
```

Or with Docker:

```sh
docker build -t spaghettikart-relay tools/netplay-relay
docker run -d --name relay -p 7777:7777/udp --restart unless-stopped spaghettikart-relay
```

Then, in the game, open **Enhancements > Netplay > Open Netplay Lobby** and enter the relay's `host:port` under **Relay server**.

Open UDP port 7777 in the server's firewall. Each player needs about 10 KB/s in each direction while racing, so a small VPS can carry many rooms. Most races don't go through the relay at all; see below.

The game and the relay must speak the same protocol version: after updating the game, rebuild the relay too. A game that gets no answer says so.

## Direct connections

Once players are in a room, their games try to reach each other directly (UDP hole punching), which is faster than going through the relay:

1. The relay tells each member where the others are: the address it sees them at, and the address each reports on its own network (for players behind the same router).
2. Each game sends small "punch" packets to those addresses from the same socket it talks to the relay with, so most home routers let the replies in.
3. Race traffic to a player goes straight to it once one of its punches gets through, and back through the relay if the direct way goes quiet for 1.5 s.

Lobby messages always go through the relay. Routers that give every destination a different public port (symmetric NAT, common on mobile networks) block direct connections; those players stay on the relay.

Direct connections share each player's IP address with the others in the room. A player can turn them off with **Connect directly to other players** in the netplay window; the relay then keeps that player's addresses to itself.

## Options

| Option | Effect |
|---|---|
| `--port N` | UDP port to listen on (default 7777) |
| `--delay-ms N` | Delays every packet the relay sends by N ms |
| `--jitter-ms N` | Adds a random 0–N ms on top of the delay; this reorders packets |
| `--loss P` | Drops P% of the packets the relay sends |
| `--no-rate-limits` | Turns off the per-address limits below, for tests that run several fast-forwarded games on one machine |

`--delay-ms`, `--jitter-ms` and `--loss` simulate a bad network, for testing rollback on one machine.

## Behaviour

- Joining needs the same game build and the same game files and mods as the host. The relay compares both and refuses a mismatch with a clear error.
- A room closes when its host leaves.
- A client the relay has not heard from in 15 s is dropped.
- Rate limits apply per source IP: packets, room creations (10 per minute), and join attempts (20 per minute; too slow to guess codes).
- Payloads over 1200 bytes are dropped.
- Traffic is not encrypted. The relay sees inputs and lobby messages, nothing more.
- Each member gets a random key that the relay shows only to its room, so a member can tell the others' direct packets from strangers'.

## Test

```sh
python3 tools/netplay-relay/test_relay.py build-relay/netplay-relay
```

`tools/netplay-sim.py` plays whole races, cups and battles through a relay with bot-driven games, fast-forwarded, and
checks that the games stayed in sync.
