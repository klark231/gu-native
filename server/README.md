# MHGURecomp hub server

The lobby / relay server for internet hubs: MHGURecomp players find each other's Local Play hubs through it and
play together without forwarding a port. Like an LDN server for Ryujinx, but for this port only (it lists MHGURecomp
hubs of the same port version, no Ryujinx players).

- A hub created in the game is listed here for players with the same **matchmaking key** (`[Multiplayer] Key` in
  `mhgurecomp.ini`, or the settings menu). Friends who all enter the same key only see each other's hubs; without a
  key a hub is public and seen by everyone else without a key. The game sends a hash of the key, never the key.
- Joining goes through the server: it pipes the player's connection to one the host opens for them, and passes the
  game's UDP packets between the two. Nobody needs a forwarded port. When both players allow direct connections
  (`[Multiplayer] Direct`, on by default), the server also tells each game the other's address so their packets can go
  straight between them (lower ping) when their routers let them; the relay stays as the fallback. A player with
  Direct off never has their IP address shown to anyone.
- Players on the same LAN still find each other directly (LAN search comes first).

One small executable, no database, no configuration file. Uses one port, TCP and UDP (default 39510).

## Build

Go 1.22 or newer (https://go.dev/dl/), nothing else:

```
cd server
go build -o mhgurecomp-hubserver .
go test ./...
```

`GOOS=linux GOARCH=amd64 go build -o mhgurecomp-hubserver .` builds a Linux server on any OS (also `arm64`).

## Run

```
./mhgurecomp-hubserver -addr :39510
```

Open the port for **TCP and UDP** in the firewall (and forward it, if the server is behind a router). Options:

| Flag | Default | |
|---|---|---|
| `-addr` | `:39510` | address and port (TCP and UDP) |
| `-max-conns` | 4000 | connections at once |
| `-max-conns-per-ip` | 24 | connections at once from one IP address |
| `-max-hubs` | 2000 | hubs listed at once |
| `-max-hubs-per-ip` | 4 | hubs from one IP address |
| `-list-max` | 56 | hubs in one search answer (the most one answer can carry; the game takes 24 networks per search and the client lists the rest on the next searches, up to 500 hubs). The answer is always the same for the same hubs: free places first, then the fullest, then the newest. With more hubs than that, the last ones are left out |
| `-bandwidth` | 1048576 | bytes a second per player link and direction (a hub uses far less) |
| `-v` | off | log hubs and joins (never IP addresses) |

With Docker: `docker build -t mhgurecomp-hubserver server` and
`docker run -d --restart unless-stopped -p 39510:39510/tcp -p 39510:39510/udp mhgurecomp-hubserver`.

As a systemd service (Linux), `/etc/systemd/system/mhgurecomp-hubserver.service`:

```
[Unit]
Description=MHGURecomp hub server
After=network-online.target

[Service]
ExecStart=/usr/local/bin/mhgurecomp-hubserver -addr :39510
DynamicUser=yes
Restart=always

[Install]
WantedBy=multi-user.target
```

then `systemctl enable --now mhgurecomp-hubserver`.

## Using it in the game

`[Multiplayer] Server = your.host.name` (or `host:port` for another port) in `mhgurecomp.ini`, or "Hub server" in the
settings menu (Network page). Empty means none: no internet hubs until you enter a server (`Off` is the same).
Everyone who wants to see each other's hubs must use the same server.

## Protocol

server.go's header comment. In short: control frames (u32 size, u8 type, payload) on TCP; requests LIST, HOST,
JOIN, ATTACH carry "MHGS", the server protocol version, the port's link protocol version and the key's hash. After a
join the two connections are spliced and carry the games' own link (runtime/net/ldn.c), which the server does not
read. UDP datagrams "MHGR" + pair id + the sender's secret + payload are passed, without that prefix, to the other end
of the pair.

Direct connections (hole punching): a datagram of exactly that prefix + `MHGQ` + 12 zero bytes asks for the other
end's public UDP address and is not passed on. Once both ends of a pair have asked (each game asks only when its
player has `[Multiplayer] Direct = On`: that is their consent to show their address), the server answers the asker
with `MHGP`, the pair id, the asker's secret, the other end's IPv4 address and port, 2 zero bytes and the pair's
direct key (32 bytes, the query's size); the games mark their direct datagrams with that key. The games then try to
reach each other directly and use the relay whenever that does not work, so a server older than this simply keeps
every game on the relay.

Safety: sizes capped, every NetworkInfo checked before it is listed, LIST / JOIN rate limited per IP address,
connections and hubs per IP address limited, handshakes time out, the relay only forwards between the two ends of a
pair it made (never to an address a client names) and a relayed datagram is always smaller than the one received.
