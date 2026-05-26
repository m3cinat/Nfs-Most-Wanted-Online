# Configuration

This project uses two main config files:

- `client/online/configs/ONLINE.cfg` - placed next to `speed.exe` as `ONLINE.cfg`.
- `server/server.cfg` - used when starting the Python server.

## Client: ONLINE.cfg

Use `server_host` to point the game to your server. Per-service hosts are
optional; if omitted, they use `server_host`.

For same-machine testing:

```ini
server_host = 127.0.0.1
```

For remote hosting, set it to the server IP or DNS name:

```ini
server_host = 203.0.113.10
```

Default ports:

- `bootstrap_port = 20921`
- `lobby_port = 9900`
- `race_port = 2000`
- `control_port = 20923`
- `control_alias_port = 13505`

Optional per-service host keys:

- `bootstrap_host`
- `lobby_host`
- `race_host`
- `control_host`
- `control_alias_host`
- `lan_host`

If a per-service host is omitted, it falls back to `server_host`. If
`server_host` is also omitted, the built-in default host is used.

Ports do not inherit from a main port. If a port is omitted, its built-in
default is used.

Server-advertised endpoints can override configured endpoints after the client
connects, when the server sends endpoint fields for that service.

## Client Runtime Options

Endpoint and LAN redirect options:

```ini
online_override_host = on
lan_override_host = on
lan_provider_seed = on
```

`online_override_host` redirects the stock online/bootstrap connection to
`bootstrap_host`/`bootstrap_port`, falling back to `server_host`.

`lan_override_host` redirects the LAN lobby connection to the configured LAN
endpoint. For NFSMW, keep the LAN lobby port on TCP `9900`.

`lan_provider_seed` mirrors LAN discovery traffic to the configured server so
the client can find a remote lobby without relying only on local broadcast.

LAN server-list injection options:

```ini
lan_discovery_host_id = NFSMWNA
lan_discovery_name = MWONLINE
lan_discovery_port = 0
lan_internal_hooks = on
lan_internal_fake = on
lan_internal_selected_host_hook = on
lan_internal_fake_name = MWONLINE
lan_internal_fake_short = 9900
lan_internal_fake_id = 1
```

These options control the LAN server entry shown inside the game. Leave them on
their defaults unless the LAN server entry does not appear.

Debug logging:

```ini
debug = on
```

Set `debug = off` for normal use if you do not need client logs.

## Server: server.cfg

Public endpoints are what the server advertises to clients:

```ini
BOOTSTRAP_ENDPOINT=127.0.0.1:20921
LOBBY_ENDPOINT=127.0.0.1:30920
CONTROL_ENDPOINT=127.0.0.1:20923
CONTROL_ALIAS_ENDPOINT=127.0.0.1:13505
RACE_ENDPOINT=127.0.0.1:2000
```

For remote hosting, change the endpoint hosts to your public IP or DNS name:

```ini
BOOTSTRAP_ENDPOINT=203.0.113.10:20921
LOBBY_ENDPOINT=203.0.113.10:30920
CONTROL_ENDPOINT=203.0.113.10:20923
CONTROL_ALIAS_ENDPOINT=203.0.113.10:13505
RACE_ENDPOINT=203.0.113.10:2000
```

Listen endpoints are local sockets opened by the server:

```ini
BOOTSTRAP_LISTEN=0.0.0.0:20921
LOBBY_LISTEN=0.0.0.0:9900
LOBBY_EXTRA_LISTEN_PORTS=30920
CONTROL_LISTEN=0.0.0.0:20923
CONTROL_ALIAS_LISTEN=0.0.0.0:13505
RACE_LISTEN=0.0.0.0:2000
```

Usually keep listen hosts on `0.0.0.0` and only change public endpoints. Bind a
specific listen host only when you need one network interface.

The client `race_port`, server `RACE_ENDPOINT` port, and server `RACE_LISTEN`
port should normally match. The client can consume the advertised race endpoint,
but the advertised port still has to be a port where the server is listening.

The game also uses local UDP `3658` for peer traffic:

```ini
UDP_GAME_PORT=3658
```

This is not the relay listen port. Keep UDP `3658` free on each client machine,
especially when testing multiple clients locally.

## Capacity and Abuse Limits

Basic connection limits:

```ini
SERVER_MAX_PLAYERS=10
SERVER_MAX_CONNECTIONS=64
SERVER_CONN_RATE_LIMIT=20
SERVER_CONN_RATE_WINDOW=10
SERVER_CONN_RATE_BLOCK=5
SERVER_TCP_TIMEOUT=60
SERVER_MAX_BUFFER_BYTES=131072
```

`SERVER_MAX_PLAYERS` caps simultaneous connected players.

`SERVER_CONN_RATE_LIMIT` caps new TCP lobby/bootstrap connections per source IP
inside `SERVER_CONN_RATE_WINDOW` seconds. When exceeded, new connections from
that IP are dropped for `SERVER_CONN_RATE_BLOCK` seconds. Set
`SERVER_CONN_RATE_LIMIT=0` to disable this throttle.

`SERVER_MAX_CONNECTIONS` caps active lobby/bootstrap sockets before login, and
`SERVER_MAX_BUFFER_BYTES` drops clients that keep incomplete lobby frames or
lines buffered above the limit.

Control/social limits:

```ini
CONTROL_REQUIRE_LOBBY_SESSION=1
CONTROL_TRUST_CLIENT_PERSONA=0
CONTROL_MAX_CONNECTIONS=32
CONTROL_PREAUTH_TIMEOUT=20.0
CONTROL_IDLE_TIMEOUT=120.0
CONTROL_HTTP_MAX_BODY=8192
CONTROL_MAX_FRAME_BYTES=65535
```

Keep `CONTROL_REQUIRE_LOBBY_SESSION=1` and `CONTROL_TRUST_CLIENT_PERSONA=0` for
Internet-facing servers.

UDP relay state limits:

```ini
UDP_RELAY_MAX_CLIENTS=128
UDP_RELAY_MAX_PENDING_ROOMS=128
UDP_RELAY_PENDING_ROOM_TTL=60.0
```

## Authentication

Account validation is controlled by `AUTH_VERIFY`:

```ini
AUTH_VERIFY=0
AUTH_ALLOW_CREATE=1
AUTH_ACCOUNTS_FILE=data/auth_accounts.json
```

Use `AUTH_VERIFY=0` for no-auth local/LAN testing. Use `AUTH_VERIFY=1` when you
want the server to validate accounts from `AUTH_ACCOUNTS_FILE`.

Use `server/data/auth_accounts.example.json` as the account file format
reference.

Persona rules:

```ini
PERSONA_UNIQUE=1
PERSONA_BLACKLIST_FILE=data/persona_blacklist.txt
```

## Logging

Useful debug options:

```ini
DEBUG_MODE=1
LOG_CONSOLE_LEVEL=INFO
LOG_FILE=
LOBBY_FRAME_TRACE=1
UDP_RELAY_VERBOSE=1
UDP_DEBUG=on
```

For normal public hosting, keep verbose tracing off:

```ini
DEBUG_MODE=0
LOG_CONSOLE_LEVEL=WARNING
LOG_FILE=logs/server.log
LOG_FILE_LEVEL=INFO
LOBBY_FRAME_TRACE=0
UDP_RELAY_VERBOSE=0
UDP_DEBUG=off
```

To disable all logging:

```ini
LOG_CONSOLE_LEVEL=OFF
LOG_FILE=
```

## Data Files

Runtime data is stored under `server/data/`:

- `auth_accounts.json`
- `social_relations.json`
- `admin_bans.json`
- `rankings.dat`
- `stats.dat`
- `game_reports.dat`
- `persona_blacklist.txt`

The repository ships empty/default data files. Do not publish live server data
from a public server.
