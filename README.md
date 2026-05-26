# NFSMW Online

Online/LAN server and client hook for Need for Speed: Most Wanted.

```text
server/
client/online/
docs/
```

## Server

```sh
cd server
python3 server.py server.cfg
```

## Client

```sh
cd client/online
./build.sh
```

The client build writes:

```text
client/online/dist/online.asi
```

Copy `client/online/configs/ONLINE.cfg` next to `speed.exe` as `ONLINE.cfg`, and
load `online.asi` through the game's usual scripts/plugin folder.

## Documentation

- [Configuration](docs/CONFIG.md)
- [Release notes](docs/RELEASE.md)
