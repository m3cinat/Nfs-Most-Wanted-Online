# Release

## Package Contents

A release should include:

- `server/`
- `client/online/dist/online.asi`
- `client/online/configs/ONLINE.cfg`
- `README.md`
- `docs/CONFIG.md`
- `docs/RELEASE.md`

The client source and build script can be included for source releases. For a
binary-only client package, the required client files are:

```text
online.asi
ONLINE.cfg
```

## Build

Build the client plugin from `client/online/`:

```sh
./build.sh
```

The build output is:

```text
client/online/dist/online.asi
```

Check the server files compile before publishing:

```sh
python3 -m py_compile server/*.py
```

## Server Setup

Start the server from the `server/` directory:

```sh
python3 server.py server.cfg
```

For local testing, the default endpoints use `127.0.0.1`.

For remote hosting, edit `server/server.cfg` and set the public endpoint hosts
to your server IP or DNS name:

```ini
BOOTSTRAP_ENDPOINT=203.0.113.10:20921
LOBBY_ENDPOINT=203.0.113.10:30920
CONTROL_ENDPOINT=203.0.113.10:20923
CONTROL_ALIAS_ENDPOINT=203.0.113.10:13505
RACE_ENDPOINT=203.0.113.10:2000
```

Keep listen endpoints on `0.0.0.0` unless you need to bind a specific network
interface.

## Client Setup

Copy these files next to `speed.exe`:

```text
online.asi
ONLINE.cfg
```

Edit `ONLINE.cfg`:

```ini
server_host = 203.0.113.10
bootstrap_port = 20921
lobby_port = 9900
control_port = 20923
control_alias_port = 13505
race_port = 2000
```

Use `127.0.0.1` for same-machine testing.

## Ports

Open these ports on the server/firewall:

- TCP `20921` - bootstrap
- TCP `9900` - MW LAN lobby
- TCP `30920` - online lobby endpoint
- TCP `20923` - control/social
- TCP `13505` - control alias
- UDP `2000` - race relay

Each client machine must also keep UDP `3658` free for the game process.

## Public Server Checklist

Before publishing a server package or running a public server:

- Set public endpoint hosts in `server/server.cfg`.
- Keep `*_LISTEN` hosts on `0.0.0.0` unless binding one interface.
- Keep `RACE_ENDPOINT` and `RACE_LISTEN` on the same UDP port.
- Set `AUTH_VERIFY=1` if account validation is required.
- Keep `CONTROL_REQUIRE_LOBBY_SESSION=1`.
- Keep `CONTROL_TRUST_CLIENT_PERSONA=0`.
- Disable verbose debug logs unless actively debugging.
- Do not publish live `server/data/` files from a public server.

Recommended public logging:

```ini
DEBUG_MODE=0
LOG_CONSOLE_LEVEL=WARNING
LOG_FILE=logs/server.log
LOG_FILE_LEVEL=INFO
LOBBY_FRAME_TRACE=0
UDP_RELAY_VERBOSE=0
UDP_DEBUG=off
```

## Repository Checks

Useful checks before tagging a release:

```sh
python3 -m py_compile server/*.py
cd client/online
./build.sh
```

The repository should not contain local IPs, personal data, captures, logs, or
live account/stat files.
