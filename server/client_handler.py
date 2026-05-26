"""
client_handler.py - compact MW lobby handler.

The previous file had grown into a patch stack.  This version keeps the public
entry points that the server/admin code uses, but models the MW 9900 lobby flow
from the decoded stock captures:

    login/select -> host create -> usea/ujoi/gjoi staged join -> ready -> gsta
"""

from __future__ import annotations

import ipaddress
import logging
import socket
import struct
import threading
import time
from dataclasses import dataclass
from hashlib import md5
from typing import Iterable, Optional

import persona_policy
from protocol import encode_message, parse_message
from room_manager import GAME_STATE_ACTIVE, GAME_STATE_OPEN, Game
from user_manager import STAT_GAME, STAT_LOBBY, STAT_ROOM, User

log = logging.getLogger("client")

SHORT_FRAME_TAGS = {b"newsbadc", b"userbadc"}


@dataclass(frozen=True)
class MWFrame:
    command: str
    payload: bytes
    total_len: int
    reserved_be32: int = 0
    raw: bytes = b""
    short_tag: bool = False

    @property
    def body_text(self) -> str:
        return self.payload.rstrip(b"\x00").decode("utf-8", errors="replace")


def is_printable_cmd4(buf: bytes) -> bool:
    return len(buf) == 4 and all(32 <= b <= 126 for b in buf)


def parse_mw_frame(buf: bytes, offset: int = 0) -> MWFrame | None:
    """Parse one MW 9900 lobby frame from buf[offset:]."""
    if offset < 0 or len(buf) - offset < 12:
        return None

    head8 = buf[offset : offset + 8]
    total = struct.unpack(">I", buf[offset + 8 : offset + 12])[0]
    if total < 12 or total > 0x4000 or len(buf) - offset < total:
        return None

    raw = bytes(buf[offset : offset + total])
    if head8 in SHORT_FRAME_TAGS:
        return MWFrame(
            command=head8.decode("latin1", errors="replace"),
            payload=b"",
            total_len=total,
            raw=raw,
            short_tag=True,
        )

    cmd_raw = buf[offset : offset + 4]
    token_reply = not is_printable_cmd4(cmd_raw)
    command = (
        cmd_raw.decode("latin1", errors="replace")
        if not token_reply
        else f"token:{cmd_raw.hex()}"
    )
    reserved = (
        struct.unpack(">I", cmd_raw)[0]
        if token_reply
        else struct.unpack(">I", buf[offset + 4 : offset + 8])[0]
    )
    return MWFrame(
        command=command,
        payload=bytes(buf[offset + 12 : offset + total]),
        total_len=total,
        reserved_be32=reserved,
        raw=raw,
    )


def find_mw_frame_offset(buf: bytes, start: int = 0) -> int | None:
    if start < 0:
        start = 0
    end = max(0, len(buf) - 11)
    for off in range(start, end + 1):
        if parse_mw_frame(buf, off) is not None:
            return off
    return None


def make_20922_tab_message(cmd4: str, fields: list[str]) -> bytes:
    raw = cmd4.encode("latin1", errors="ignore")
    if not is_printable_cmd4(raw):
        raise ValueError(f"invalid 20922 cmd: {cmd4!r}")
    body = ("\t".join(fields).encode("utf-8") + b"\x00") if fields else b"\x00"
    return raw + b"\x00\x00\x00\x00" + struct.pack(">I", 12 + len(body)) + body


def make_token_tab_reply(token_be32: int, fields: list[str]) -> bytes:
    body = ("\t".join(fields).encode("utf-8") + b"\x00") if fields else b"\x00"
    return (
        struct.pack(">I", token_be32 & 0xFFFFFFFF)
        + b"\x00\x00\x00\x00"
        + struct.pack(">I", 12 + len(body))
        + body
    )


def make_short_frame(tag8: str) -> bytes:
    raw = tag8.encode("latin1", errors="ignore")
    if len(raw) != 8:
        raise ValueError(f"invalid short lobby frame tag: {tag8!r}")
    return raw + struct.pack(">I", 12)


def make_20922_binary_message(cmd4: str, payload: bytes, reserved_be32: int = 0) -> bytes:
    raw = cmd4.encode("latin1", errors="ignore")
    if not is_printable_cmd4(raw):
        raise ValueError(f"invalid 20922 cmd: {cmd4!r}")
    return raw + struct.pack(">I", reserved_be32 & 0xFFFFFFFF) + struct.pack(">I", 12 + len(payload)) + payload


def make_20922_signed_binary_message(
    cmd4: str,
    payload_prefix: bytes,
    total_payload_len: int,
    reserved_be32: int = 0,
) -> bytes:
    if total_payload_len < 8:
        raise ValueError(f"invalid signed payload length: {total_payload_len}")
    body_cap = total_payload_len - 8
    if len(payload_prefix) > body_cap:
        raise ValueError(f"signed payload prefix too large: {len(payload_prefix)} > {body_cap}")
    payload_wo_sig = payload_prefix + (b"\x00" * (body_cap - len(payload_prefix)))
    frame_wo_sig = make_20922_binary_message(cmd4, payload_wo_sig + (b"\x00" * 8), reserved_be32=reserved_be32)
    return frame_wo_sig[:-8] + md5(frame_wo_sig[:-8]).digest()[:8]


def parse_20922_kv(body: bytes) -> dict:
    txt = body.decode("utf-8", errors="replace").rstrip("\x00")
    out = {}
    txt = txt.replace("\r", "").replace("\t", "\n")
    for line in (line.strip() for line in txt.split("\n")):
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        out[key.strip()] = value.strip()
    return out


def format_lobby_time(ts: float | None = None) -> str:
    tm = time.localtime(time.time() if ts is None else ts)
    return f"{tm.tm_year}.{tm.tm_mon}.{tm.tm_mday} {tm.tm_hour:02d}:{tm.tm_min:02d}:{tm.tm_sec:02d}"


READY_OPFLAG = 134217728
READY_USER_FLAG = 0x2000000
RACE_SYSFLAGS = 524288
DEFAULT_GAME_PARAMS = "TRACK%3d4000%0aDIR%3d0%0aLAPS%3d3%0a"
LOBBY_FLAG_CHARS = "@ABCDEFGHIJKLMNOPQRSTUVWXYZ0123-"
LOBBY_USERSET_RANK_BIT = 1 << LOBBY_FLAG_CHARS.index("I")
LOBBY_USERSET_MODE_BITS = (1 << LOBBY_FLAG_CHARS.index("J")) | (1 << LOBBY_FLAG_CHARS.index("M"))
DIR_SESS = "1773180069"
DIR_MASK = "ed5faa76adec3f22520b6c90ec35acd4"
LOBBY_SERVER_SKEY = "$b54ca8de40238572024704cc4de73590"
AUTH_IMST_RESERVED = int.from_bytes(b"imst", "big")
AUTH_LOGN_RESERVED = int.from_bytes(b"logn", "big")
AUTH_LOCK_RESERVED = int.from_bytes(b"lock", "big")
AUTH_PASS_RESERVED = int.from_bytes(b"pass", "big")
AUTH_IKEY_RESERVED = int.from_bytes(b"ikey", "big")
AUTH_TOSA_RESERVED = int.from_bytes(b"tosa", "big")
AUTH_DBER_RESERVED = int.from_bytes(b"dber", "big")
AUTH_BLAK_RESERVED = int.from_bytes(b"blak", "big")
AUTH_SHAR_RESERVED = int.from_bytes(b"shar", "big")
AUTH_MISS_RESERVED = int.from_bytes(b"miss", "big")
AUTH_FILT_RESERVED = int.from_bytes(b"filt", "big")
AUTH_TIME_RESERVED = int.from_bytes(b"time", "big")
AUTH_OVER_RESERVED = int.from_bytes(b"over", "big")
AUTH_DUPL_RESERVED = int.from_bytes(b"dupl", "big")
PERSONA_DUPL_RESERVED = int.from_bytes(b"dupl", "big")
PERSONA_INVP_RESERVED = int.from_bytes(b"invp", "big")
PERSONA_NSPC_RESERVED = int.from_bytes(b"nspc", "big")
PERSONA_MAUT_RESERVED = int.from_bytes(b"maut", "big")
PERSONA_PSET_RESERVED = int.from_bytes(b"pset", "big")
TRACE_CMDS = {
    "addr", "skey", "news", "newsbadc", "epnt", "auth", "*ath", "pers", "cper", "PERS", "*pat", "acct", "user",
    "sele", "usea", "ucre", "UCRE", "uadm", "UADM", "ujoi", "gsea", "gcre", "GCRE",
    "gjoi", "GJOI", "gset", "GSET", "auxi", "AUXI", "mesg", "MESG", "onln", "gsta",
    "GSTA", "+who", "+ust", "+usm", "+usr", "+gam", "+mgm", "+msg", "+sst", "+ses",
}


def _cfg_bool(cfg: object, key: str, default: bool = False) -> bool:
    getter = getattr(cfg, "get", None)
    value = getter(key, default) if callable(getter) else default
    if isinstance(value, str):
        return value.strip().lower() not in ("", "0", "false", "no", "off")
    return bool(value)


def _cfg_float(cfg: object, key: str, default: float) -> float:
    getter = getattr(cfg, "get", None)
    value = getter(key, default) if callable(getter) else default
    try:
        return float(value)
    except (TypeError, ValueError):
        return float(default)


def _fields_text(fields: Iterable[str]) -> str:
    return "\t".join(str(field) for field in fields if str(field) != "")


def _safe_int(value: object, default: int = 0) -> int:
    try:
        return int(str(value).strip(), 10)
    except (TypeError, ValueError):
        return int(default)


def _parse_lobby_int(value: object, default: int = 0) -> int:
    text = str(value or "").strip()
    if not text:
        return int(default)
    sign = 1
    if text[0] == "-":
        sign = -1
        text = text[1:]
    elif text[0] == "+":
        text = text[1:]
    base = 10
    if text.startswith("$"):
        base = 16
        text = text[1:]
    try:
        return sign * int(text, base)
    except ValueError:
        return int(default)


def _u32(value: object) -> int:
    return int(value or 0) & 0xFFFFFFFF


def _decode_lobby_flag_set(value: object, default: int = 0) -> int:
    text = str(value or "").strip()
    if not text:
        return int(default)
    flags = 0
    matched = False
    for char in text:
        idx = LOBBY_FLAG_CHARS.find(char)
        if idx < 0:
            break
        flags |= 1 << idx
        matched = True
    return flags if matched else int(default)


def _encode_lobby_flag_set(flags: int) -> str:
    value = _u32(flags)
    return "".join(char for idx, char in enumerate(LOBBY_FLAG_CHARS) if value & (1 << idx))


def _normalize_lobby_userset_cf(value: object) -> str:
    text = str(value or "").strip() or "JKM-"
    flags = _decode_lobby_flag_set(text, 0)
    if not flags:
        return text
    return _encode_lobby_flag_set(flags) or "JKM-"


def _lobby_userset_ignored_search_bits(kv: dict[str, str]) -> int:
    mask = _u32(_parse_lobby_int(kv.get("CUSTMASK"), 0))
    if not (mask & LOBBY_USERSET_RANK_BIT):
        return 0
    if mask & LOBBY_USERSET_MODE_BITS:
        return 0
    return LOBBY_USERSET_RANK_BIT


def _kv_text(fields: dict[str, object]) -> list[str]:
    return [f"{key}={value}" for key, value in fields.items()]


def _strip_nul_text(data: bytes) -> str:
    return data.rstrip(b"\x00").decode("utf-8", errors="replace")


def _is_loopback_host(value: object) -> bool:
    text = str(value or "").strip()
    if not text:
        return False
    if text.lower() == "localhost":
        return True
    try:
        return ipaddress.ip_address(text).is_loopback
    except ValueError:
        return False


def _normalize_peer_laddr(addr: object, laddr: object) -> str:
    public_addr = str(addr or "").strip()
    local_addr = str(laddr or "").strip()
    if public_addr and local_addr and _is_loopback_host(local_addr) and not _is_loopback_host(public_addr):
        return public_addr
    return local_addr or public_addr


def _normalize_aux_text(text: str) -> str:
    text = str(text or "").replace("\r", "")
    text = text.strip()
    if len(text) >= 2 and text[0] == text[-1] == '"':
        text = text[1:-1].strip()
    text = text.replace("%3D", "%3d").replace("%2C", "%2C").replace("%0A", "%0a")
    text = text.replace(",", "%2C")
    if "\n" in text:
        parts = [part.strip() for part in text.split("\n") if part.strip()]
        text = "%0a".join(parts)
    if text and not text.endswith("%0a"):
        text += "%0a"
    return text


def _aux_fields_to_text(fields: dict[str, object]) -> str:
    parts: list[str] = []
    preferred = ("SCF", "CE", "LT", "V", "TIME", "PAUSE", "HURRY")
    for key in preferred:
        if key in fields:
            value = str(fields[key])
            value = value.replace(",", "%2C")
            parts.append(f"{key}%3d{value}")
    for key, value in fields.items():
        if key in preferred:
            continue
        value = str(value).replace(",", "%2C")
        parts.append(f"{key}%3d{value}")
    return ("%0a".join(parts) + "%0a") if parts else ""


def _remove_aux_keys(text: str, *keys: str) -> str:
    wanted = {key.upper() for key in keys}
    chunks = [chunk for chunk in str(text or "").split("%0a") if chunk]
    kept: list[str] = []
    for chunk in chunks:
        key = chunk.split("%3d", 1)[0].split("=", 1)[0].upper()
        if key not in wanted:
            kept.append(chunk)
    return ("%0a".join(kept) + "%0a") if kept else ""


def _stock_when(ts: float | None = None) -> str:
    tm = time.localtime(time.time() if ts is None else ts)
    return f"{tm.tm_year}.{tm.tm_mon}.{tm.tm_mday}-{tm.tm_hour:02d}:{tm.tm_min:02d}:{tm.tm_sec:02d}"


class ClientHandler:
    _lobby_handlers: set["ClientHandler"] = set()
    _lobby_handlers_lock = threading.Lock()

    def __init__(self, server: "GameServer", user: User):
        self.srv = server
        self.user = user
        self._disconnect_reason = "loop_exit"
        self._recv_buf = bytearray()
        self._registered = False
        self._probe_client_addr = getattr(user, "ip", "127.0.0.1")
        self._probe_client_port = str(getattr(user, "port", 0) or 0)
        self._probe_display_name = getattr(user, "name", "") or f"Player{getattr(user, 'uid', 0)}"
        self._probe_persona = getattr(user, "pers", "") or self._probe_display_name
        self._auth_mail = ""
        self._auth_personas: list[str] = []
        self._probe_aux_text = getattr(user, "aux", "") or ""
        self._probe_seen_auth = False
        self._probe_seen_sele = False
        self._probe_last_ref = ""
        self._probe_deferred_addr_frame = b""
        self._probe_deferred_skey_frame = b""
        self._lobby_race_bootstrap_done = False
        self._lobby_post_gjoi_auxi_game_id = 0
        self._lobby_post_gjoi_promoted = False
        self._lobby_post_gjoi_staged = False
        self._lobby_pending_join_gset_game_id = 0
        self._lobby_pending_join_gset_uid = 0
        self._lobby_pending_userset_id = 0
        self._lobby_pending_userset_name = ""
        self._lobby_pending_userset_cf = ""
        self._lobby_pending_userset_desc = ""
        self._lobby_pending_userset_params = ""
        self._lobby_pending_userset_size = 0
        self._lobby_pending_invite_game_id = 0
        self._lobby_pending_invite_from = ""
        self._lobby_pending_invite_name = ""
        self._lobby_last_uadm_at = 0.0
        self._lobby_last_gsea_kv: dict[str, str] | None = None
        self._lobby_active_room_reentry_game_id = 0
        with ClientHandler._lobby_handlers_lock:
            ClientHandler._lobby_handlers.add(self)

    # ------------------------------------------------------------------ #
    # Registry, sending and frame helpers                                 #
    # ------------------------------------------------------------------ #

    @classmethod
    def _snapshot_lobby_handlers(cls) -> list["ClientHandler"]:
        with cls._lobby_handlers_lock:
            return list(cls._lobby_handlers)

    @classmethod
    def _parse_any_bootstrap_frame(cls, buf: bytes):
        frame = parse_mw_frame(buf)
        if frame is None:
            return None
        return frame.command, frame.payload, frame.total_len

    @classmethod
    def _find_bootstrap_frame_offset(cls, buf: bytes, start: int = 0):
        return find_mw_frame_offset(buf, start)

    def _make_20922_tab_message(self, cmd4: str, fields: list[str]) -> bytes:
        return make_20922_tab_message(cmd4, fields)

    def _make_token_tab_reply(self, token_be32: int, fields: list[str]) -> bytes:
        return make_token_tab_reply(token_be32, fields)

    def _make_20922_binary_message(self, cmd4: str, payload: bytes, reserved_be32: int = 0) -> bytes:
        return make_20922_binary_message(cmd4, payload, reserved_be32)

    def _make_20922_signed_binary_message(
        self,
        cmd4: str,
        payload_prefix: bytes,
        total_payload_len: int,
        reserved_be32: int = 0,
    ) -> bytes:
        return make_20922_signed_binary_message(cmd4, payload_prefix, total_payload_len, reserved_be32)

    def _send_bootstrap_bytes(self, data: bytes, label: str = "") -> None:
        if not data:
            return
        self.user.send_bytes(data)
        self._trace_lobby_bytes("send", data, label=label)
        if label:
            log.debug("[uid=%d] sent %s len=%d", self.user.uid, label, len(data))

    def _send_later_bytes(
        self,
        delay_s: float,
        data: bytes,
        label: str = "",
        should_send=None,
    ) -> threading.Timer:
        def _send() -> None:
            try:
                if should_send is not None and not should_send():
                    return
                self._send_bootstrap_bytes(data, label=label)
            except Exception as exc:
                log.debug("[uid=%d] delayed send failed %s: %s", self.user.uid, label, exc)

        timer = threading.Timer(max(0.0, float(delay_s or 0.0)), _send)
        timer.daemon = True
        timer.start()
        return timer

    def _broadcast_lobby_bytes(self, data: bytes, *, exclude_uid: int = 0, label: str = "") -> int:
        count = 0
        for handler in self._snapshot_lobby_handlers():
            if exclude_uid and int(handler.user.uid) == int(exclude_uid):
                continue
            if not bool(getattr(handler.user, "connected", False)):
                continue
            handler._send_bootstrap_bytes(data, label=label)
            count += 1
        return count

    # ------------------------------------------------------------------ #
    # Run loop                                                            #
    # ------------------------------------------------------------------ #

    def run(self) -> None:
        conn = self.user.conn
        try:
            timeout = _cfg_float(getattr(self.srv, "cfg", {}), "SERVER_TCP_TIMEOUT", 60.0)
            try:
                conn.settimeout(timeout)
            except Exception:
                pass
            while bool(getattr(self.srv, "is_running", True)) and bool(getattr(self.user, "connected", True)):
                try:
                    data = conn.recv(8192)
                except socket.timeout:
                    if self._lobby_keep_active_race_tcp_on_timeout():
                        self.user.touch()
                        continue
                    break
                except OSError:
                    break
                if not data:
                    break
                self.user.touch()
                self._recv_buf.extend(data)
                consumed = self._consume_bootstrap_frames(bytes(self._recv_buf))
                if consumed:
                    del self._recv_buf[:consumed]
                else:
                    line_consumed = self._consume_text_lines()
                    if line_consumed:
                        del self._recv_buf[:line_consumed]
                max_buf = _safe_int(getattr(self.srv, "cfg", {}).get("SERVER_MAX_BUFFER_BYTES", 131072), 131072)
                if len(self._recv_buf) > max_buf:
                    log.warning("[uid=%d] closing oversized lobby buffer len=%d", self.user.uid, len(self._recv_buf))
                    break
        finally:
            self._cleanup_on_close()

    def _lobby_keep_active_race_tcp_on_timeout(self) -> bool:
        game = self.srv.games.get(int(getattr(self.user, "game", 0) or 0)) if getattr(self.user, "game", 0) else None
        if game is None or getattr(game, "state", "") != GAME_STATE_ACTIVE:
            return False
        if int(getattr(self.user, "uid", 0) or 0) not in self._lobby_game_participant_uid_set(game):
            return False
        log.debug("[uid=%d] keeping idle TCP open during active race game=%s", self.user.uid, getattr(game, "id", 0))
        return True

    def _consume_text_lines(self) -> int:
        buf = bytes(self._recv_buf)
        newline = buf.find(b"\n")
        if newline < 0:
            return 0
        line = buf[: newline + 1].decode("utf-8", errors="replace").strip()
        if not line:
            return newline + 1
        try:
            cmd, fields = parse_message(line)
        except Exception:
            return newline + 1
        self._handle_text_command(cmd, fields)
        return newline + 1

    def _consume_bootstrap_frames(self, buf: bytes) -> int:
        consumed = 0
        while consumed < len(buf):
            frame = parse_mw_frame(buf, consumed)
            if frame is None:
                next_off = find_mw_frame_offset(buf, consumed + 1)
                if next_off is None:
                    break
                consumed = next_off
                continue
            self._handle_frame(frame.command, frame.payload, frame.reserved_be32)
            consumed += frame.total_len
        return consumed

    def _handle_frame(self, cmd: str, payload: bytes, reserved_be32: int = 0) -> None:
        if cmd in ("@dir", "?dir"):
            self._send_bootstrap_bytes(self._make_dir_reply(), label="dir")
            return
        if cmd in ("@tic", "?tic"):
            return
        if cmd in ("newsbadc", "userbadc"):
            return
        self._handle_plain_prelogin_frame(cmd, payload, reserved_be32=reserved_be32)

    def _handle_text_command(self, cmd: str, fields: dict) -> None:
        cmd = (cmd or "").upper()
        if cmd == "PING":
            self.user.send(encode_message("PONG"))
        elif cmd == "SERVER-STAT":
            self._cmd_server_stat(fields)
        else:
            self.user.send(encode_message(cmd or "OK", RESULT="OK"))

    # ------------------------------------------------------------------ #
    # Identity/auth                                                       #
    # ------------------------------------------------------------------ #

    def _ensure_registered_user(self) -> bool:
        users = getattr(self.srv, "users", None)
        if users is None:
            self._registered = True
            return True
        get = getattr(users, "get", None)
        if callable(get) and get(int(self.user.uid)) is not None:
            self._registered = True
            return True
        add = getattr(users, "add", None)
        if callable(add):
            if getattr(self.srv, "is_user_banned", lambda user: False)(self.user):
                self._disconnect_reason = "admin_ban"
                return False
            ok = bool(add(self.user))
            self._registered = ok
            return ok
        self._registered = True
        return True

    def _server_accepts_new_user(self) -> bool:
        users = getattr(self.srv, "users", None)
        if users is None:
            return True
        get = getattr(users, "get", None)
        if callable(get) and get(int(self.user.uid)) is not None:
            return True
        try:
            max_users = int(getattr(users, "max_users", 0) or 0)
        except (TypeError, ValueError):
            max_users = 0
        if max_users <= 0:
            return True
        all_users = getattr(users, "all_users", None)
        current = len(all_users()) if callable(all_users) else 0
        return current < max_users

    def _display_and_persona_from_kv(self, kv: dict[str, str]) -> tuple[str, str]:
        name = kv.get("NAME", "").strip() or kv.get("MAIL", "").split("@", 1)[0].strip()
        has_wire_name = bool(name)
        if not name:
            name = self._probe_display_name or getattr(self.user, "name", "") or f"Player{self.user.uid}"
        persona = kv.get("PERS", "").strip() or kv.get("PERSONA", "").strip()
        if not persona:
            persona = name if has_wire_name else (self._probe_persona or name)
        self._probe_display_name = name
        self._probe_persona = persona
        self.user.name = name
        self.user.pers = persona
        return name, persona

    def _auth_reply_fields(self) -> list[str]:
        name = self._probe_display_name or self.user.name
        persona = self._probe_persona or self.user.pers or name
        addr = self._probe_client_addr or getattr(self.user, "ip", "127.0.0.1")
        personas = self._lobby_auth_personas_value(persona)
        return [
            f"NAME={name}",
            f"GTAG={name}",
            f"PERSONAS={personas}",
            "XUID=",
            "TOS=1",
            "SHARE=1",
            f"ADDR={addr}",
        ]

    def _lobby_auth_frame(self) -> bytes:
        name = (self._probe_display_name or self.user.name or f"Player{self.user.uid}").strip()
        persona = (self._probe_persona or self.user.pers or name).strip()
        self._probe_display_name = name
        self.user.name = name
        self._probe_persona = persona
        self.user.pers = persona
        return self._make_20922_tab_message("auth", self._auth_reply_fields())

    def _lobby_persona_ack_fields(self) -> list[str]:
        name = self._probe_display_name or self.user.name
        persona = self._probe_persona or self.user.pers or name
        return [f"NAME={name}", f"PERS={persona}", "GTAG=", "XUID="]

    def _lobby_tos_value(self, key: str, default: int) -> int:
        try:
            return int(getattr(self.srv, "cfg", {}).get(key, default) or default)
        except (TypeError, ValueError):
            return int(default)

    def _lobby_pers_frame(self, persona: str, display_name: str) -> bytes:
        persona = str(persona or "").strip()
        display_name = str(display_name or persona or self.user.name or "").strip()
        self._probe_persona = persona
        self.user.pers = persona
        self._probe_display_name = display_name
        self.user.name = display_name
        addr = self._probe_client_addr or getattr(self.user, "ip", "127.0.0.1")
        laddr = _normalize_peer_laddr(addr, getattr(self.user, "laddr", "") or addr)
        return self._make_20922_tab_message(
            "pers",
            [
                f"NAME={display_name}",
                f"PERS={persona}",
                "LOC=enUS",
                "MA=",
                f"A={addr}",
                f"LA={laddr}",
            ],
        )

    def _lobby_user_frame(self) -> bytes:
        persona = self._lobby_persona()
        try:
            stat_csv = self.srv.stats.profile_stat_csv(persona)
        except Exception:
            stat_csv = "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
        payload = (
            f"LMSTAT={stat_csv}\n"
            f"STAT={stat_csv}\n"
            "LGAME=\n"
        ).encode("utf-8") + b"\x00"
        return self._make_20922_signed_binary_message("user", payload, len(payload) + 8)

    def _lobby_player_stat_csv(self, persona: str) -> str:
        try:
            return self.srv.stats.player_stat_csv(persona)
        except Exception:
            return ",".join(["270f", "0", "0", "0", "64", "65", "65"] * 5)

    def _lobby_player_stat_csv_for_user(self, user: User) -> str:
        return self._lobby_player_stat_csv(self._lobby_persona_for(user))

    def _lobby_auxi_frame(self) -> bytes:
        return self._make_20922_signed_binary_message("auxi", b"\x00", 9)

    def _lobby_online_who_snapshot(self, *, include_self: bool = True) -> bytes:
        frames: list[bytes] = []
        current_uid = int(getattr(self.user, "uid", 0) or 0)
        for user in sorted(getattr(self.srv.users, "all_users", lambda: [])(), key=lambda item: int(getattr(item, "uid", 0) or 0)):
            if not getattr(user, "connected", True):
                continue
            uid = int(getattr(user, "uid", 0) or 0)
            if not include_self and uid == current_uid:
                continue
            if not str(getattr(user, "pers", "") or getattr(user, "name", "") or "").strip():
                continue
            frames.append(
                self._make_20922_tab_message(
                    "+who",
                    self._lobby_who_fields_for(
                        user,
                        aux_text=self._lobby_aux_for(user),
                        game_active=bool(getattr(user, "game", 0) or 0),
                    ),
                )
            )
        return b"".join(frames)

    def _lobby_broadcast_online_who(self, subject: User, *, delay_s: float = 0.02, exclude_uid: int | None = None) -> None:
        if not getattr(subject, "connected", True):
            return
        for handler in self._snapshot_lobby_handlers():
            if not getattr(handler.user, "connected", True):
                continue
            if exclude_uid is not None and int(handler.user.uid) == int(exclude_uid):
                continue
            frame = handler._make_20922_tab_message(
                "+who",
                handler._lobby_who_fields_for(
                    subject,
                    aux_text=handler._lobby_aux_for(subject),
                    game_active=bool(getattr(subject, "game", 0) or 0),
                ),
            )
            handler._send_later_bytes(delay_s, frame, label="online-who")

    @staticmethod
    def _format_probe_ref(ts: float | None = None) -> str:
        tm = time.localtime(time.time() if ts is None else ts)
        return f"{tm.tm_year}.{tm.tm_mon}.{tm.tm_mday} {tm.tm_hour:02d}:{tm.tm_min:02d}:{tm.tm_sec:02d}"

    def _lobby_server_addr(self) -> str:
        host = getattr(self.srv, "lobby_tcp_host", lambda conn=None: "")(getattr(self.user, "conn", None))
        return str(host or "127.0.0.1")

    def _lobby_game_endpoint_for_user(self, user: User | None):
        target = user or self.user
        try:
            return self._race_endpoint_for(target)
        except Exception:
            return str(getattr(target, "ip", "") or self._lobby_server_addr()), 3658

    def _lobby_online_bootstrap_fields(self) -> list[str]:
        conn = getattr(self.user, "conn", None)
        lobby_host = str(getattr(self.srv, "lobby_tcp_host", lambda _conn=None: "127.0.0.1")(conn) or "").strip()
        try:
            lobby_port = int(getattr(self.srv, "lobby_tcp_port", lambda: 9900)() or 0)
        except Exception:
            lobby_port = 9900
        control_host = str(getattr(self.srv, "control_host", lambda _conn=None: lobby_host)(conn) or "").strip()
        try:
            control_port = int(getattr(self.srv, "control_port", lambda: 20923)() or 0)
        except Exception:
            control_port = 20923
        control_alias_host = str(getattr(self.srv, "control_alias_host", lambda _conn=None: "")(conn) or "").strip()
        try:
            control_alias_port = int(getattr(self.srv, "control_alias_port", lambda: 0)() or 0)
        except Exception:
            control_alias_port = 0
        udp_host, udp_port = self._lobby_game_endpoint_for_user(self.user)
        udp_host = str(udp_host or "").strip()
        udp_port = int(udp_port or 0)
        fields: list[str] = []
        if lobby_host:
            fields.append(f"LOBBYHOST={lobby_host}")
        if lobby_port > 0:
            fields.append(f"LOBBYTCP={lobby_port}")
        if control_host:
            fields.append(f"CONTROLHOST={control_host}")
        if control_port > 0:
            fields.append(f"CONTROLPORT={control_port}")
        if control_alias_host:
            fields.append(f"CONTROLALIASHOST={control_alias_host}")
        if control_alias_port > 0:
            fields.append(f"CONTROLALIASPORT={control_alias_port}")
        if udp_host:
            fields.extend([f"UDPHOST={udp_host}", f"RLYHOST={udp_host}"])
        if udp_port > 0:
            fields.extend([f"UDPPORT={udp_port}", f"RLYPORT={udp_port}"])
        return fields

    def _lobby_endpoint_advertisement_frame(self) -> bytes:
        payload = ("\n".join(self._lobby_online_bootstrap_fields()) + "\n").encode("ascii", errors="ignore") + b"\x00"
        return self._make_20922_binary_message("epnt", payload)

    def _lobby_news_with_endpoint_advertisement(self) -> bytes:
        return self._lobby_endpoint_advertisement_frame() + self._lobby_news_burst()

    def _lobby_news_autopush_enabled(self) -> bool:
        try:
            return int(getattr(self.srv, "cfg", {}).get("LOBBY_NEWS_AUTOPUSH_AUTH", 0) or 0) != 0
        except Exception:
            return False

    def _lobby_auth_autopush_enabled(self) -> bool:
        try:
            return int(getattr(self.srv, "cfg", {}).get("AUTH_AUTOPUSH", 0) or 0) != 0
        except Exception:
            return False

    def _lobby_news_push_after_auth_enabled(self) -> bool:
        try:
            return int(getattr(self.srv, "cfg", {}).get("LOBBY_NEWS_PUSH_AFTER_AUTH", 0) or 0) != 0
        except Exception:
            return False

    def _lobby_news_push_delay(self) -> float:
        try:
            return max(0.0, float(getattr(self.srv, "cfg", {}).get("LOBBY_NEWS_PUSH_DELAY", 0.75) or 0.75))
        except Exception:
            return 0.75

    def _lobby_prelogin_burst_after_news_enabled(self) -> bool:
        mode = str(getattr(self.srv, "cfg", {}).get("LOBBY_NEWS_MODE", "captured") or "captured").strip().lower()
        if mode in {"legacy", "png", "~png"}:
            return False
        try:
            return int(getattr(self.srv, "cfg", {}).get("LOBBY_PRELOGIN_BURST_AFTER_NEWS", 1) or 1) != 0
        except Exception:
            return True

    def _schedule_lobby_news_auth_followups(self) -> None:
        if not self._lobby_news_autopush_enabled():
            return
        self._send_later_bytes(
            0.035,
            self._lobby_sele_frame(),
            label="news-autosele",
            should_send=lambda: not self._probe_seen_sele,
        )
        if self._lobby_auth_autopush_enabled():
            self._send_later_bytes(
                0.12,
                self._lobby_auth_frame(),
                label="news-autoauth",
                should_send=lambda: not self._probe_seen_auth,
            )

    def _schedule_lobby_news_push(self, label: str = "news-push") -> None:
        self._send_later_bytes(
            self._lobby_news_push_delay(),
            self._lobby_news_with_endpoint_advertisement(),
            label=label,
        )

    @staticmethod
    def _lobby_persona_reject_reserved(reason: str) -> int:
        key = persona_policy.canonical_reason(reason)
        return {
            "dupl": PERSONA_DUPL_RESERVED,
            "invp": PERSONA_INVP_RESERVED,
            "nspc": PERSONA_NSPC_RESERVED,
            "maut": PERSONA_MAUT_RESERVED,
            "pset": PERSONA_PSET_RESERVED,
        }.get(key, PERSONA_INVP_RESERVED)

    @staticmethod
    def _lobby_persona_is_valid(persona: str) -> bool:
        text = str(persona or "").strip()
        if not text or len(text.encode("utf-8", errors="ignore")) > 32:
            return False
        return not any(ch in text for ch in "\x00\r\n\t")

    def _lobby_persona_allowed_for_auth_account(self, persona: str) -> bool:
        personas = [str(item or "").strip().lower() for item in self._auth_personas if str(item or "").strip()]
        if not personas:
            return True
        return str(persona or "").strip().lower() in personas

    def _lobby_persona_conflict(self, persona: str):
        wanted = str(persona or "").strip().lower()
        if not wanted:
            return None
        current_uid = int(getattr(self.user, "uid", 0) or 0)
        for other in getattr(self.srv.users, "all_users", lambda: [])():
            other_uid = int(getattr(other, "uid", 0) or 0)
            if other_uid == current_uid or not getattr(other, "connected", True):
                continue
            other_persona = str(getattr(other, "pers", "") or getattr(other, "name", "") or "").strip().lower()
            if other_persona and other_persona == wanted:
                return other
        return None

    def _lobby_persona_slots_full(self, persona: str) -> bool:
        try:
            limit = int(getattr(self.srv, "cfg", {}).get("PERSONA_MAX_PERSONAS", 0) or 0)
        except (TypeError, ValueError):
            limit = 0
        if limit <= 0:
            return False
        existing = {
            str(item or "").strip().lower()
            for item in self._auth_personas
            if str(item or "").strip()
        }
        if str(persona or "").strip().lower() in existing:
            return False
        return len(existing) >= limit

    def _lobby_persona_reject_frame(self, cmd4: str, reason: str) -> bytes:
        cmd = "cper" if str(cmd4 or "").lower() == "cper" else "pers"
        return self._make_20922_signed_binary_message(
            cmd,
            b"\x00",
            9,
            reserved_be32=self._lobby_persona_reject_reserved(reason),
        )

    def _lobby_reject_persona(self, persona: str, stage: str, reason: str, *, conflict=None) -> None:
        stage_l = str(stage or "").lower()
        if stage_l in ("cper", "pers"):
            self._send_bootstrap_bytes(self._lobby_persona_reject_frame(stage_l, reason), label="persona-reject")
        else:
            self._send_bootstrap_bytes(make_short_frame("userbadc"), label="persona-reject")
        log.warning(
            "[uid=%d] persona rejected stage=%s reason=%s persona=%s conflict_uid=%d",
            self.user.uid,
            stage,
            reason,
            str(persona or "-")[:64],
            int(getattr(conflict, "uid", 0) or 0) if conflict is not None else 0,
        )

    def _lobby_claim_persona_or_reject(self, persona: str, stage: str) -> bool:
        stage_l = str(stage or "").lower()
        if stage_l in ("cper", "pers"):
            forced_reason = ""
            try:
                forced_reason = self.srv.pop_forced_persona_reject(stage_l, persona)
            except Exception:
                forced_reason = ""
            if forced_reason:
                self._lobby_reject_persona(persona, stage_l, forced_reason)
                return False
            if not self._lobby_persona_is_valid(persona):
                self._lobby_reject_persona(persona, stage_l, "invp")
                return False
            blacklist_reason = ""
            try:
                blacklist_reason = self.srv.persona_blacklist_reject_reason(persona, stage_l)
            except Exception:
                blacklist_reason = ""
            if blacklist_reason:
                self._lobby_reject_persona(persona, stage_l, blacklist_reason)
                return False
            if stage_l == "cper" and self._lobby_persona_slots_full(persona):
                self._lobby_reject_persona(persona, stage_l, "nspc")
                return False
            auth_verify = getattr(self.srv, "auth_verify_enabled", None)
            if stage_l == "pers" and callable(auth_verify) and auth_verify() and not self._probe_seen_auth:
                self._lobby_reject_persona(persona, stage_l, "maut")
                return False
            if stage_l == "pers" and not self._lobby_persona_allowed_for_auth_account(persona):
                self._lobby_reject_persona(persona, stage_l, "invp")
                return False
        conflict = self._lobby_persona_conflict(persona)
        if conflict is not None:
            self._lobby_reject_persona(persona, stage_l, "dupl" if stage_l == "cper" else "invp", conflict=conflict)
            return False
        return True

    def _lobby_reattach_detached_identity(self) -> bool:
        wanted = {
            str(self._probe_persona or self.user.pers or "").strip().lower(),
            str(self._probe_display_name or self.user.name or "").strip().lower(),
        }
        wanted.discard("")
        if not wanted:
            return False
        current_uid = int(getattr(self.user, "uid", 0) or 0)
        peer_ip = str(self._probe_client_addr or getattr(self.user, "ip", "") or "").strip()
        for old_user in getattr(self.srv.users, "all_users", lambda: [])():
            old_uid = int(getattr(old_user, "uid", 0) or 0)
            if old_uid == current_uid or getattr(old_user, "connected", True):
                continue
            if float(getattr(old_user, "race_detached_at", 0.0) or 0.0) <= 0.0:
                continue
            old_names = {
                str(getattr(old_user, "pers", "") or "").strip().lower(),
                str(getattr(old_user, "name", "") or "").strip().lower(),
            }
            old_names.discard("")
            if not wanted.intersection(old_names):
                continue
            old_ip = str(getattr(old_user, "ip", "") or "").strip()
            if peer_ip and old_ip and peer_ip != old_ip:
                continue
            game_id = int(getattr(old_user, "game", 0) or 0)
            game = self.srv.games.get(game_id) if game_id and hasattr(self.srv, "games") else None
            if game is not None:
                game.participants = [
                    current_uid if int(uid) == old_uid else int(uid)
                    for uid in list(getattr(game, "participants", []) or [])
                ]
                if int(getattr(game, "host_uid", 0) or 0) == old_uid:
                    game.host_uid = current_uid
                if old_uid in getattr(game, "ready_participants", set()):
                    game.ready_participants.discard(old_uid)
                    game.ready_participants.add(current_uid)
                self.user.game = game_id
                self.user.stat = getattr(old_user, "stat", STAT_ROOM) or STAT_ROOM
            self.user.room = int(getattr(old_user, "room", 0) or 0)
            self.user.aux = str(getattr(old_user, "aux", "") or getattr(self.user, "aux", "") or "")
            old_user.game = 0
            old_user.room = 0
            old_user.race_detached_at = 0.0
            remove = getattr(self.srv.users, "remove", None)
            if callable(remove):
                remove(old_uid)
            log.info(
                "[uid=%d] reattached detached identity old_uid=%d game=%d persona=%s",
                current_uid,
                old_uid,
                game_id,
                self._lobby_persona() or "-",
            )
            return True
        return False

    @staticmethod
    def _lobby_account_text(account: dict, *keys: str) -> str:
        for key in keys:
            value = account.get(key)
            if value is None:
                continue
            text = str(value or "").strip()
            if text:
                return text
        return ""

    @staticmethod
    def _lobby_account_list(account: dict, *keys: str) -> list[str]:
        values: list[str] = []
        for key in keys:
            raw = account.get(key)
            if raw is None:
                continue
            items = raw if isinstance(raw, list) else str(raw).replace(";", ",").split(",")
            for item in items:
                text = str(item or "").strip()
                if text:
                    values.append(text)
        return values

    def _lobby_apply_auth_account(self, account: dict, fallback_name: str, fallback_persona: str) -> None:
        if not account:
            return
        personas = self._lobby_account_list(account, "personas", "persona", "pers")
        display = (
            self._lobby_account_text(account, "display_name", "display", "name", "username", "user")
            or fallback_name
            or self.user.name
        )
        persona = personas[0] if personas else (fallback_persona or display)
        mail = self._lobby_account_text(account, "email", "mail", "__key")
        self._auth_mail = mail
        self._auth_personas = personas
        self._probe_display_name = display
        self.user.name = display
        self._probe_persona = persona
        self.user.pers = persona

    def _lobby_auth_personas_value(self, persona: str) -> str:
        values: list[str] = []
        for item in [persona, *self._auth_personas, *str(getattr(self.srv, "cfg", {}).get("AUTH_EXTRA_PERSONAS", "") or "").split(",")]:
            text = str(item or "").strip()
            if text and text.lower() not in {value.lower() for value in values}:
                values.append(text)
        return ",".join(values) if values else persona

    @staticmethod
    def _lobby_auth_reject_reserved(reason: str) -> int:
        key = str(reason or "").strip().lower()
        if key.startswith("auth") and len(key) == 8:
            key = key[4:]
        key = key.replace("-", "_").replace(" ", "_").split(":", 1)[0]
        return {
            "imst": AUTH_IMST_RESERVED,
            "invalid_auth": AUTH_IMST_RESERVED,
            "unknown_account": AUTH_IMST_RESERVED,
            "missing_identifier": AUTH_IMST_RESERVED,
            "logn": AUTH_LOGN_RESERVED,
            "account_in_use": AUTH_LOGN_RESERVED,
            "already_logged_in": AUTH_LOGN_RESERVED,
            "already_online": AUTH_LOGN_RESERVED,
            "lock": AUTH_LOCK_RESERVED,
            "locked": AUTH_LOCK_RESERVED,
            "account_locked": AUTH_LOCK_RESERVED,
            "account_disabled": AUTH_LOCK_RESERVED,
            "disabled": AUTH_LOCK_RESERVED,
            "rate_limited": AUTH_LOCK_RESERVED,
            "pass": AUTH_PASS_RESERVED,
            "bad_password": AUTH_PASS_RESERVED,
            "missing_password": AUTH_PASS_RESERVED,
            "password_error": AUTH_PASS_RESERVED,
            "ikey": AUTH_IKEY_RESERVED,
            "invalid_key": AUTH_IKEY_RESERVED,
            "bad_key": AUTH_IKEY_RESERVED,
            "invalid_cdkey": AUTH_IKEY_RESERVED,
            "invalid_cd_key": AUTH_IKEY_RESERVED,
            "tosa": AUTH_TOSA_RESERVED,
            "tos_not_accepted": AUTH_TOSA_RESERVED,
            "terms_not_accepted": AUTH_TOSA_RESERVED,
            "dber": AUTH_DBER_RESERVED,
            "database_error": AUTH_DBER_RESERVED,
            "backend_error": AUTH_DBER_RESERVED,
            "server_full": AUTH_DBER_RESERVED,
            "save_failed": AUTH_DBER_RESERVED,
            "no_accounts": AUTH_DBER_RESERVED,
            "blak": AUTH_BLAK_RESERVED,
            "banned": AUTH_BLAK_RESERVED,
            "admin_ban": AUTH_BLAK_RESERVED,
            "blacklisted": AUTH_BLAK_RESERVED,
            "blacklist": AUTH_BLAK_RESERVED,
            "blocked": AUTH_BLAK_RESERVED,
            "shar": AUTH_SHAR_RESERVED,
            "share_not_accepted": AUTH_SHAR_RESERVED,
            "share_required": AUTH_SHAR_RESERVED,
            "miss": AUTH_MISS_RESERVED,
            "missing_fields": AUTH_MISS_RESERVED,
            "missing_required_fields": AUTH_MISS_RESERVED,
            "filt": AUTH_FILT_RESERVED,
            "filtered": AUTH_FILT_RESERVED,
            "filter_failed": AUTH_FILT_RESERVED,
            "profane": AUTH_FILT_RESERVED,
            "time": AUTH_TIME_RESERVED,
            "auth_timeout": AUTH_TIME_RESERVED,
            "timeout": AUTH_TIME_RESERVED,
            "backend_timeout": AUTH_TIME_RESERVED,
            "over": AUTH_OVER_RESERVED,
            "invalid_state": AUTH_OVER_RESERVED,
            "backend_over": AUTH_OVER_RESERVED,
        }.get(key, AUTH_IMST_RESERVED)

    @staticmethod
    def _lobby_account_create_reject_reserved(reason: str) -> int:
        if str(reason or "").strip() == "missing_password":
            return AUTH_PASS_RESERVED
        if str(reason or "").strip() == "account_exists":
            return AUTH_DUPL_RESERVED
        return AUTH_IMST_RESERVED

    def _lobby_auth_reject_frame(self, reserved_be32: int) -> bytes:
        return self._make_20922_signed_binary_message("auth", b"\x00", 9, reserved_be32=reserved_be32)

    def _lobby_account_create_frame(self, reason: str = "created", *, ok: bool = True) -> bytes:
        reserved = 0 if ok else self._lobby_account_create_reject_reserved(reason)
        return self._make_20922_signed_binary_message("acct", b"\x00", 9, reserved_be32=reserved)

    def _lobby_auth_reject_repeat(self) -> int:
        try:
            repeat = int(getattr(self.srv, "cfg", {}).get("AUTH_REJECT_REPEAT", 4) or 4)
        except (TypeError, ValueError):
            repeat = 4
        return max(1, min(8, repeat))

    def _lobby_auth_reject_interval(self) -> float:
        try:
            interval = float(getattr(self.srv, "cfg", {}).get("AUTH_REJECT_INTERVAL", 0.25) or 0.25)
        except (TypeError, ValueError):
            interval = 0.25
        return max(0.0, min(2.0, interval))

    def _lobby_auth_reject_close_delay(self, repeat: int, interval: float) -> float:
        try:
            delay = float(getattr(self.srv, "cfg", {}).get("AUTH_REJECT_CLOSE_DELAY", 1.10) or 1.10)
        except (TypeError, ValueError):
            delay = 1.10
        minimum = (max(1, int(repeat)) - 1) * max(0.0, float(interval)) + 0.20
        return max(minimum, min(10.0, delay))

    def _lobby_close_after_auth_reject(self, delay: float) -> None:
        def _close() -> None:
            self.user.connected = False
            try:
                self.user.conn.shutdown(socket.SHUT_RDWR)
            except Exception:
                pass

        timer = threading.Timer(max(0.0, delay), _close)
        timer.daemon = True
        timer.start()

    def _lobby_reject_auth(self, reason: str, identifier: str, *, reserved_be32: int | None = None) -> None:
        reserved = self._lobby_auth_reject_reserved(reason) if reserved_be32 is None else int(reserved_be32)
        frame = self._lobby_auth_reject_frame(reserved)
        repeat = self._lobby_auth_reject_repeat()
        interval = self._lobby_auth_reject_interval()
        self._send_bootstrap_bytes(frame, label="auth-reject")
        for idx in range(1, repeat):
            self._send_later_bytes(interval * idx, frame, label=f"auth-reject-{idx + 1}")
        safe_identifier = (identifier or "-").replace("\n", " ").replace("\r", " ")
        log.warning("[uid=%d] auth rejected reason=%s id=%s", self.user.uid, reason, safe_identifier[:96])
        self._disconnect_reason = f"auth_failed:{reason}"
        self._lobby_close_after_auth_reject(self._lobby_auth_reject_close_delay(repeat, interval))

    def _lobby_account_conflict(self, account_name: str):
        wanted = str(account_name or "").strip().lower()
        if not wanted:
            return None
        current_uid = int(getattr(self.user, "uid", 0) or 0)
        for other in getattr(self.srv.users, "all_users", lambda: [])():
            other_uid = int(getattr(other, "uid", 0) or 0)
            if other_uid == current_uid or not getattr(other, "connected", True):
                continue
            other_name = str(getattr(other, "name", "") or "").strip().lower()
            if other_name and other_name == wanted:
                return other
        return None

    def _lobby_accept_auth(self, kv: dict[str, str], fallback_name: str, fallback_persona: str) -> bool:
        if getattr(self.srv, "is_user_banned", lambda user: False)(self.user):
            self._lobby_reject_auth("admin_ban", fallback_name or self.user.name, reserved_be32=AUTH_BLAK_RESERVED)
            return False
        authenticate = getattr(self.srv, "authenticate_login", None)
        if callable(authenticate):
            auth_kv = dict(kv or {})
            recent = getattr(self.srv, "recent_lobby_dir_challenge", None)
            if callable(recent):
                sess, mask = recent(self.user.ip)
                if sess:
                    auth_kv.setdefault("SESS", sess)
                if mask:
                    auth_kv.setdefault("MASK", mask)
                    auth_kv.setdefault("CHALLENGE", mask)
            ok, reason, account, identifier = authenticate(auth_kv)
        else:
            ok, reason, account, identifier = True, "disabled", {}, fallback_name
        if not ok:
            self._lobby_reject_auth(reason, identifier)
            return False
        if account:
            self._lobby_apply_auth_account(account, fallback_name, fallback_persona)
        account_name = self._probe_display_name or self.user.name or fallback_name
        conflict = self._lobby_account_conflict(account_name)
        if conflict is not None:
            auth_verify = getattr(self.srv, "auth_verify_enabled", None)
            if callable(auth_verify) and not auth_verify():
                fallback_unique = f"Player{getattr(self.user, 'uid', 0)}"
                self._probe_persona = fallback_unique
                self.user.pers = fallback_unique
                account_name = fallback_unique
            elif not callable(auth_verify):
                fallback_unique = f"Player{getattr(self.user, 'uid', 0)}"
                self._probe_persona = fallback_unique
                self.user.pers = fallback_unique
                account_name = fallback_unique
            else:
                self._lobby_reject_auth("account_in_use", account_name, reserved_be32=AUTH_LOGN_RESERVED)
                return False
        conflict = self._lobby_account_conflict(account_name)
        if conflict is not None:
            self._lobby_reject_auth("account_in_use", account_name, reserved_be32=AUTH_LOGN_RESERVED)
            return False
        ranking = getattr(self.srv, "ranking", None)
        if ranking is not None and hasattr(ranking, "get_or_create"):
            ranking.get_or_create(self.user.uid, self.user.name)
        stats = getattr(self.srv, "stats", None)
        if stats is not None and hasattr(stats, "get_player_stats"):
            stats.get_player_stats(self._probe_persona or self.user.pers or fallback_persona, create=True)
        return True

    # ------------------------------------------------------------------ #
    # Names, ids and game lookup                                          #
    # ------------------------------------------------------------------ #

    def _lobby_display_name(self) -> str:
        return self._lobby_display_name_for(self.user)

    def _lobby_persona(self) -> str:
        return self._lobby_persona_for(self.user)

    def _lobby_display_name_for(self, user: Optional[User]) -> str:
        if user is None:
            return ""
        return str(getattr(user, "pers", "") or getattr(user, "name", "") or f"Player{getattr(user, 'uid', 0)}")

    def _lobby_persona_for(self, user: Optional[User]) -> str:
        return self._lobby_display_name_for(user)

    def _lobby_game_protocol_id(self, game) -> int:
        if game is None:
            return 0
        if not hasattr(game, "_lobby_protocol_id"):
            setattr(game, "_lobby_protocol_id", int(getattr(game, "id", 0) or 0) + 1)
        return int(getattr(game, "_lobby_protocol_id", 0) or 0)

    def _lobby_room_ident(self, game) -> int:
        if game is None:
            return 0
        return int(getattr(game, "_lobby_userset_id", 0) or getattr(game, "id", 0) or 0)

    def _lobby_next_game_ident(self) -> int:
        ident = int(getattr(Game, "_counter", 0) or 0)
        if ident > 0:
            return ident
        games = self.srv.games.list_games() if hasattr(self.srv.games, "list_games") else []
        return max([int(getattr(game, "id", 0) or 0) for game in games] or [0]) + 1

    def _lobby_game_participant_uids(self, game) -> list[int]:
        if game is None:
            return []
        ordered: list[int] = []
        host_uid = int(getattr(game, "host_uid", 0) or 0)
        if host_uid and self.srv.users.get(host_uid) is not None:
            ordered.append(host_uid)
        for uid in list(getattr(game, "participants", []) or []):
            uid = int(uid)
            if self.srv.users.get(uid) is None:
                continue
            if uid not in ordered:
                ordered.append(uid)
        return ordered

    def _lobby_game_participant_uid_set(self, game) -> set[int]:
        return set(self._lobby_game_participant_uids(game))

    def _lobby_game_wire_ids(self, game) -> dict[int, int]:
        mapping = getattr(game, "_lobby_wire_ids", None)
        if not isinstance(mapping, dict):
            mapping = {}
            setattr(game, "_lobby_wire_ids", mapping)
        next_id = max([int(value) for value in mapping.values()] or [0]) + 1
        for uid in self._lobby_game_participant_uids(game):
            uid = int(uid)
            if uid not in mapping:
                mapping[uid] = next_id
                next_id += 1
        return mapping

    def _lobby_existing_game_wire_id_for(self, game, user: User) -> int:
        mapping = getattr(game, "_lobby_wire_ids", None)
        if not isinstance(mapping, dict):
            return 0
        return int(mapping.get(int(getattr(user, "uid", 0) or 0), 0) or 0)

    def _lobby_game_wire_id_for(self, game, user: User) -> int:
        uid = int(getattr(user, "uid", 0) or 0)
        mapping = self._lobby_game_wire_ids(game)
        if uid not in mapping:
            mapping[uid] = max([int(value) for value in mapping.values()] or [0]) + 1
        return int(mapping[uid])

    def _lobby_game_snapshot_participant_uids(self, game) -> list[int]:
        participants = self._lobby_game_participant_uids(game)
        if not participants:
            return []
        wire_ids = self._lobby_game_wire_ids(game)
        return sorted(participants, key=lambda uid: (int(wire_ids.get(int(uid), 0) or 0), participants.index(uid)))


    def _lobby_wire_id_for(self, user: User) -> int:
        return self._lobby_global_wire_id_for(user)

    def _lobby_global_wire_id_for(self, user: User) -> int:
        users = sorted(
            [int(getattr(u, "uid", 0) or 0) for u in getattr(self.srv.users, "all_users", lambda: [])()],
        )
        uid = int(getattr(user, "uid", 0) or 0)
        return users.index(uid) + 1 if uid in users else uid

    def _lobby_join_slot_reanchor_frames(self, game, joiner: User) -> bytes:
        return b""

    def _lobby_handler_for_uid(self, uid: int) -> Optional["ClientHandler"]:
        for handler in self._snapshot_lobby_handlers():
            if int(getattr(handler.user, "uid", 0) or 0) == int(uid):
                return handler
        return None

    def _lobby_game_handlers(self, game_id: int) -> list["ClientHandler"]:
        game = self.srv.games.get(int(game_id or 0)) if game_id else None
        allowed = self._lobby_game_participant_uid_set(game)
        return [handler for handler in self._snapshot_lobby_handlers() if int(handler.user.uid) in allowed]

    def _find_game_by_name_or_ident(self, name: str = "", ident: int = 0, *, open_only: bool = False):
        self._lobby_prune_stale_games()
        name_norm = str(name or "").strip().lower()
        for game in self.srv.games.list_games():
            if open_only and getattr(game, "state", GAME_STATE_OPEN) != GAME_STATE_OPEN:
                continue
            if ident and int(getattr(game, "id", 0) or 0) not in (ident, ident - 1):
                if self._lobby_game_protocol_id(game) != ident:
                    continue
            if not name_norm:
                return game
            host = self.srv.users.get(int(getattr(game, "host_uid", 0) or 0))
            names = {
                str(getattr(game, "custom", "") or "").strip().lower(),
                self._lobby_display_name_for(host).strip().lower(),
                str(getattr(game, "_lobby_userset_name", "") or "").strip().lower(),
            }
            if name_norm in names:
                return game
        return None

    # ------------------------------------------------------------------ #
    # Stock-ish fields                                                    #
    # ------------------------------------------------------------------ #

    def _lobby_sst_presence_fields(self, *, gcr: int | None = None, gcm: int | None = None, gip: int | None = None) -> str:
        active_games = self.srv.games.active_games() if hasattr(self.srv.games, "active_games") else []
        active_count = len(active_games)
        active_users = self.srv.games.active_participant_count() if hasattr(self.srv.games, "active_participant_count") else 0
        if gip is None:
            gip = 1 if active_count else 0
        if gcr is None:
            gcr = sum(1 for game in self.srv.games.list_games() if getattr(game, "state", GAME_STATE_OPEN) == GAME_STATE_OPEN)
        if gcm is None:
            gcm = 1 if gcr else 0
        fields = [
            f"UIL={len(getattr(self.srv.users, 'all_users', lambda: [])())}",
            "UIR=0",
            f"UIG={active_users}",
            f"GIP={int(gip)}",
            f"GCR={int(gcr)}",
            f"GCM={int(gcm)}",
        ]
        return _fields_text(fields)

    def _lobby_sst_frame(self, *, gcr: int | None = None, gcm: int | None = None, gip: int | None = None) -> bytes:
        return self._make_20922_tab_message("+sst", self._lobby_sst_presence_fields(gcr=gcr, gcm=gcm, gip=gip).split("\t"))

    def _lobby_aux_for(self, user: User) -> str:
        return str(getattr(user, "aux", "") or "")

    def _lobby_aux_for_presence(self, user: User) -> str:
        aux = self._lobby_aux_for(user)
        game = self.srv.games.get(int(getattr(user, "game", 0) or 0)) if getattr(user, "game", 0) else None
        if not aux and game is not None:
            if int(getattr(user, "uid", 0) or 0) == int(getattr(game, "host_uid", 0) or 0):
                aux = "SCF%3d0%0aCE%3d3%2C2%0aV%3d20%0a"
            else:
                aux = "SCF%3d0%0aLT%3d-1%0aV%3d20%0a"
        if game is not None and self._lobby_countdown_reset_active(game):
            return _remove_aux_keys(aux, "CE")
        return aux

    def _lobby_usr_fields(
        self,
        sync: int = 1,
        game_id: int | None = None,
        user: Optional[User] = None,
        flags: str = "",
        wire_id: int = 0,
    ) -> list[str]:
        user = user or self.user
        display_game = 0
        effective_game_id = int(getattr(user, "game", 0) or 0) if game_id is None else int(game_id or 0)
        game = self.srv.games.get(effective_game_id) if effective_game_id else None
        if game is not None:
            display_game = (
                self._lobby_game_protocol_id(game)
                if getattr(game, "state", "") == GAME_STATE_ACTIVE
                else self._lobby_room_ident(game)
            )
        fields = [
            f"I={int(wire_id) if wire_id else self._lobby_wire_id_for(user)}",
            f"N={self._lobby_display_name_for(user)}",
            f"P={self._lobby_persona_for(user)}",
            f"GAME={display_game}",
            f"SYNC={int(sync)}",
        ]
        if flags:
            fields.append(f"FLAGS={flags}")
        return fields

    def _lobby_next_usr_sync_for(self, user: User) -> int:
        current = int(getattr(user, "_lobby_usr_sync", 3) or 3) + 1
        setattr(user, "_lobby_usr_sync", current)
        return current

    def _lobby_room_usr_fields_for_user(self, user: User, game_id: int = 0) -> list[str]:
        return self._lobby_usr_fields(sync=2, game_id=game_id, user=user)

    def _lobby_who_fields_for(
        self,
        user: User,
        *,
        aux_text: str = "",
        game_active: bool = False,
        game_id: int = 0,
        display_game_id: int = 0,
        force_display_game_id: bool = False,
        flags: str = "U",
        wire_id: int = 0,
    ) -> list[str]:
        game = self.srv.games.get(int(game_id or getattr(user, "game", 0) or 0)) if (game_id or getattr(user, "game", 0)) else None
        if force_display_game_id:
            g_value = int(display_game_id)
        elif game_active and game is not None:
            g_value = self._lobby_game_protocol_id(game)
        else:
            g_value = 0
        virtual_addr = self._virtual_addr_for_user(game, user) if game is not None else ""
        addr = virtual_addr or str(getattr(user, "ip", "") or getattr(user, "addr", "") or self._probe_client_addr or "127.0.0.1")
        laddr = virtual_addr or _normalize_peer_laddr(addr, getattr(user, "laddr", "") or addr)
        userset = ""
        if game is not None:
            userset = str(getattr(game, "_lobby_userset_name", "") or "")
        elif user is self.user:
            userset = str(getattr(user, "_lobby_userset_name", "") or getattr(self, "_lobby_pending_userset_name", "") or "")
        aux = _normalize_aux_text(aux_text or self._lobby_aux_for_presence(user))
        fields = [
            f"I={int(wire_id) if wire_id else self._lobby_wire_id_for(user)}",
            f"N={self._lobby_display_name_for(user)}",
            f"M={self._lobby_persona_for(user)}",
            f"F={flags}",
            f"A={addr}",
            "P=425",
            "S=",
            f"G={g_value}",
            "AT=",
            "CL=511",
            "LV=0",
            "MD=0",
            f"LA={laddr}",
            "HW=0",
            "RP=0",
            "MA=",
            "LO=enUS",
            f"X={aux}",
            f"US={userset}",
            "C=",
        ]
        return fields

    def _lobby_who_fields(self, *, aux_text: str = "", game_active: bool = False) -> list[str]:
        return self._lobby_who_fields_for(self.user, aux_text=aux_text, game_active=game_active)

    def _lobby_usm_fields_for_user(
        self,
        user: User,
        *,
        game_id: int = 0,
        display_game_id: int = 0,
        active: bool | None = None,
        flags: str = "",
    ) -> list[str]:
        game = self.srv.games.get(int(game_id or getattr(user, "game", 0) or 0)) if (game_id or getattr(user, "game", 0)) else None
        if display_game_id:
            g_value = display_game_id
        elif game is not None and (active is True or int(getattr(user, "game", 0) or 0) == int(getattr(game, "id", 0) or 0)):
            g_value = self._lobby_game_protocol_id(game)
        else:
            g_value = 0
        if active is False:
            g_value = 0
        fields = [
            f"I={self._lobby_wire_id_for(user)}",
            f"N={self._lobby_display_name_for(user)}",
            f"F={flags}",
            f"G={g_value}",
            f"X={_normalize_aux_text(self._lobby_aux_for_presence(user))}",
            "S=0",
        ]
        return fields

    def _lobby_game_ready(self, game, uid: int) -> bool:
        return int(uid) in {int(x) for x in getattr(game, "ready_participants", set()) or set()}

    def _lobby_game_has_ready_marker(self, game) -> bool:
        return bool(getattr(game, "ready_participants", set()) or set())

    def _lobby_game_params(self, game) -> str:
        return _normalize_aux_text(str(getattr(game, "_lobby_params", "") or ""))

    def _lobby_game_ready_snapshot_fields(
        self,
        game,
        *,
        viewer_uid: int = 0,
        tunnel_addrs: bool = False,
        snapshot_params: str | None = None,
        include_relay: bool | None = None,
        sysflags_extra: int = 0,
        force_room_view: bool = False,
    ) -> list[str]:
        if game is None:
            return ["COUNT=0"]
        participants = self._lobby_game_snapshot_participant_uids(game)
        active = getattr(game, "state", GAME_STATE_OPEN) == GAME_STATE_ACTIVE and not force_room_view
        display_game = self._lobby_game_protocol_id(game) if active else self._lobby_room_ident(game)
        sysflags = (RACE_SYSFLAGS if active else 0) | int(sysflags_extra or 0)
        fields = [
            f"IDENT={display_game}",
            f"WHEN={str(getattr(game, '_lobby_when', '') or _stock_when(getattr(game, 'created_at', None)))}",
            f"NAME={str(getattr(game, 'custom', '') or self._host_name_for_game(game))}",
            f"HOST={self._host_name_for_game(game)}",
            f"ROOM={int(getattr(game, 'room_id', 0) or 0)}",
            f"MAXSIZE={int(getattr(game, 'limit', 8) or 8)}",
            f"MINSIZE={int(getattr(game, 'minsize', 2) or 2)}",
            f"COUNT={len(participants)}",
            "PRIV=0",
            "CUSTFLAGS=0",
            f"SYSFLAGS={sysflags}",
            f"EVID={int(getattr(game, 'evid', 0) or 0) if active else 0}",
            f"EVGID={int(getattr(game, 'evgid', 0) or 0)}",
            "NUMPART=1",
        ]
        addr_fields = self._participant_addr_fields(game, active=active or tunnel_addrs)
        addr_by_uid = {uid: vals for uid, vals in addr_fields.items()}
        for idx, uid in enumerate(participants):
            user = self.srv.users.get(uid)
            name = self._lobby_display_name_for(user)
            opflag = READY_OPFLAG if (
                _cfg_bool(getattr(self.srv, "cfg", {}), "LOBBY_ROOM_OPFLAGS_FROM_READY", False)
                and self._lobby_game_ready(game, uid)
                and active
            ) else 0
            addr, laddr, port = addr_by_uid.get(int(uid), ("127.0.0.1", "127.0.0.1", 3658))
            fields.extend(
                [
                    f"OPPO{idx}={name}",
                    f"OPPART{idx}=0",
                    f"OPFLAG{idx}={opflag}",
                    f"PRES{idx}=0",
                    f"OPID{idx}={self._lobby_wire_id_for(user) if user is not None else idx + 1}",
                    f"ADDR{idx}={addr}",
                    f"LADDR{idx}={laddr}",
                    f"MADDR{idx}=",
                ]
            )
        fields.append(f"PARTSIZE0={int(getattr(game, 'limit', 8) or 8)}")
        params = snapshot_params if snapshot_params is not None else (self._lobby_game_params(game) if active else "")
        fields.append(f"PARAMS={params}")
        fields.append("PARTPARAMS0=")
        for idx, _uid in enumerate(participants):
            fields.append(f"OPPARAM{idx}=")
        if include_relay:
            host, port = self._race_endpoint_for(self.user)
            fields.extend([f"RLYHOST={host}", f"RLYPORT={port or 3658}"])
        return fields

    def _lobby_gam_fields(self, game, *, params: str | None = None) -> list[str]:
        if game is None:
            return ["COUNT=0"]
        display_game = (
            self._lobby_game_protocol_id(game)
            if getattr(game, "state", "") == GAME_STATE_ACTIVE
            else self._lobby_room_ident(game)
        )
        return [
            f"IDENT={display_game}",
            f"GAME={display_game}",
            f"NAME={str(getattr(game, 'custom', '') or self._host_name_for_game(game))}",
            f"HOST={self._host_name_for_game(game)}",
            f"COUNT={len(self._lobby_game_participant_uids(game))}",
            f"SYSFLAGS={RACE_SYSFLAGS if getattr(game, 'state', '') == GAME_STATE_ACTIVE else 0}",
            f"PARAMS={params if params is not None else self._lobby_game_params(game)}",
        ]

    def _lobby_onln_fields_for_user(
        self,
        user: User,
        game=None,
        *,
        viewer_uid: int = 0,
        wire_id: int = 0,
        display_game_override: int | None = None,
        userset_override: str | None = None,
    ) -> list[str]:
        display_game = 0
        userset = ""
        if game is not None:
            display_game = (
                self._lobby_game_protocol_id(game)
                if getattr(game, "state", "") == GAME_STATE_ACTIVE
                else self._lobby_room_ident(game)
            )
            userset = str(getattr(game, "_lobby_userset_name", "") or "")
        if display_game_override is not None:
            display_game = int(display_game_override)
        if userset_override is not None:
            userset = str(userset_override or "")
        virtual_addr = self._virtual_addr_for_user(game, user) if game is not None else ""
        addr = virtual_addr or str(getattr(user, "ip", "") or getattr(user, "addr", "") or self._probe_client_addr or "127.0.0.1")
        laddr = virtual_addr or _normalize_peer_laddr(addr, getattr(user, "laddr", "") or addr)
        return [
            f"I={int(wire_id) if wire_id else self._lobby_wire_id_for(user)}",
            f"N={self._lobby_display_name_for(user)}",
            f"M={self._lobby_persona_for(user)}",
            "F=",
            f"A={addr}",
            "P=425",
            "S=",
            f"G={display_game}",
            "AT=",
            "CL=511",
            "LV=0",
            "MD=0",
            f"LA={laddr}",
            "HW=0",
            "RP=0",
            "MA=",
            "LO=enUS",
            f"X={_normalize_aux_text(self._lobby_aux_for_presence(user))}",
            f"US={userset}",
            f"PERS={self._lobby_persona_for(user)}",
            f"NAME={self._lobby_display_name_for(user)}",
        ]

    def _lobby_onln_room_view_fields_for_user(self, user: User, game, *, viewer_uid: int = 0) -> list[str]:
        return self._lobby_onln_fields_for_user(
            user,
            game,
            viewer_uid=viewer_uid,
            display_game_override=0,
            userset_override=str(getattr(game, "_lobby_userset_name", "") or ""),
        )

    def _lobby_msg_fields(self, text: str, *, sender: str = "Server", attr: str = "", flag: str = "") -> list[str]:
        msg_flag = str(flag or attr or "").strip()
        if msg_flag:
            return [f"F={msg_flag}", f"T={text}", "U=", f"N={sender}"]
        return [f"FROM={sender}", f"TEXT={text}"]

    def _lobby_alt_token_ack(self, token_be32: int, tag4: bytes = b"asta") -> bytes:
        return struct.pack(">I", token_be32 & 0xFFFFFFFF) + tag4[:4].ljust(4, b"\x00") + struct.pack(">I", 12)

    def _host_name_for_game(self, game) -> str:
        host = self.srv.users.get(int(getattr(game, "host_uid", 0) or 0))
        return self._lobby_display_name_for(host) or str(getattr(game, "custom", "") or "")

    # ------------------------------------------------------------------ #
    # Usersets, list/select/create                                       #
    # ------------------------------------------------------------------ #

    def _lobby_ensure_game_userset_for(self, user: User, game):
        if game is None:
            return None
        if int(getattr(game, "_lobby_userset_id", 0) or 0) <= 0:
            setattr(game, "_lobby_userset_id", int(getattr(game, "id", 0) or 0))
        if not str(getattr(game, "_lobby_userset_name", "") or ""):
            pending_name = str(getattr(self, "_lobby_pending_userset_name", "") or getattr(user, "_lobby_userset_name", "") or "")
            setattr(game, "_lobby_userset_name", pending_name or self._lobby_display_name_for(user))
        if not str(getattr(game, "_lobby_userset_cf", "") or ""):
            pending_cf = str(getattr(self, "_lobby_pending_userset_cf", "") or "")
            if pending_cf:
                setattr(game, "_lobby_userset_cf", _normalize_lobby_userset_cf(pending_cf))
        if not str(getattr(game, "_lobby_userset_sf", "") or ""):
            pending_sf = str(getattr(self, "_lobby_pending_userset_sf", "") or "")
            if pending_sf:
                setattr(game, "_lobby_userset_sf", pending_sf)
        if not str(getattr(game, "_lobby_userset_desc", "") or ""):
            pending_desc = str(getattr(self, "_lobby_pending_userset_desc", "") or "")
            if pending_desc:
                setattr(game, "_lobby_userset_desc", pending_desc)
        if not str(getattr(game, "_lobby_userset_params", "") or ""):
            pending_params = str(getattr(self, "_lobby_pending_userset_params", "") or "")
            if pending_params:
                setattr(game, "_lobby_userset_params", pending_params)
        pending_size = int(getattr(self, "_lobby_pending_userset_size", 0) or 0)
        if pending_size and not int(getattr(game, "_lobby_userset_size", 0) or 0):
            setattr(game, "_lobby_userset_size", pending_size)
        return int(getattr(game, "_lobby_userset_id", 0) or 0)

    def _lobby_userset_fields(self, game) -> list[str]:
        host = self.srv.users.get(int(getattr(game, "host_uid", 0) or 0))
        ident = self._lobby_ensure_game_userset_for(host or self.user, game)
        participants = self._lobby_game_snapshot_participant_uids(game)
        cf = _normalize_lobby_userset_cf(getattr(game, "_lobby_userset_cf", "JKM-"))
        sf = str(getattr(game, "_lobby_userset_sf", "KV") or "KV")
        set_name = str(getattr(game, "_lobby_userset_name", "") or self._host_name_for_game(game))
        desc = str(getattr(game, "_lobby_userset_desc", "") or "")
        params = str(getattr(game, "_lobby_userset_params", "") or self._lobby_game_params(game))
        size = int(getattr(game, "_lobby_userset_size", 0) or getattr(game, "limit", 4) or 4)
        return [
            f"I={ident}",
            "T=0",
            f"SF={sf}",
            f"CF={cf}",
            f"O={self._host_name_for_game(game)}",
            f"S={size}",
            f"N={set_name}",
            f"D={desc}",
            f"P={params}",
            f"C={len(participants)}",
            f"IDENT={ident}",
            f"NAME={set_name}",
        ]

    def _lobby_broadcast_userset_update(self, game, fields: list[str], *, exclude_uid: int = 0) -> None:
        if game is None:
            return
        for uid in self._lobby_game_participant_uids(game):
            if exclude_uid and int(uid) == int(exclude_uid):
                continue
            handler = self._lobby_handler_for_uid(uid)
            if handler is None:
                continue
            handler._send_bootstrap_bytes(
                handler._make_20922_tab_message("+ust", fields),
                label="userset-update-peer",
            )

    def _lobby_remember_pending_userset(self, kv: dict[str, str]) -> list[str]:
        name = kv.get("NAME", "").strip() or self._lobby_display_name()
        ident = int(getattr(self, "_lobby_pending_userset_id", 0) or 0) or self._lobby_next_game_ident()
        self._lobby_pending_userset_id = ident
        self._lobby_pending_userset_name = name
        self._lobby_pending_userset_cf = _normalize_lobby_userset_cf(kv.get("CUSTFLAGS", kv.get("CF", "JKM-")))
        self._lobby_pending_userset_sf = kv.get("SYSFLAGS", kv.get("SF", "KV")) or "KV"
        self._lobby_pending_userset_desc = kv.get("DESC", "")
        self._lobby_pending_userset_params = _normalize_aux_text(kv.get("PARAMS", "") or DEFAULT_GAME_PARAMS)
        self._lobby_pending_userset_size = max(1, _safe_int(kv.get("SIZE", kv.get("MAXSIZE", 4)), 4))
        setattr(self.user, "_lobby_userset_name", name)
        return self._lobby_pending_userset_fields()

    def _lobby_pending_userset_fields(self) -> list[str]:
        ident = int(getattr(self, "_lobby_pending_userset_id", 0) or 0) or self._lobby_next_game_ident()
        name = str(getattr(self, "_lobby_pending_userset_name", "") or getattr(self.user, "_lobby_userset_name", "") or self._lobby_display_name())
        cf = _normalize_lobby_userset_cf(getattr(self, "_lobby_pending_userset_cf", "") or "JKM-")
        sf = str(getattr(self, "_lobby_pending_userset_sf", "") or "KV")
        desc = str(getattr(self, "_lobby_pending_userset_desc", "") or "")
        params = str(getattr(self, "_lobby_pending_userset_params", "") or DEFAULT_GAME_PARAMS)
        size = int(getattr(self, "_lobby_pending_userset_size", 0) or 4)
        return [
            f"I={ident}",
            "T=0",
            f"SF={sf}",
            f"CF={cf}",
            f"O={self._lobby_display_name()}",
            f"S={size}",
            f"N={name}",
            f"D={desc}",
            f"P={params}",
            "C=1",
            f"IDENT={ident}",
            f"NAME={name}",
        ]

    def _lobby_apply_pending_userset_to_game(self, game) -> None:
        if game is None:
            return
        setattr(game, "_lobby_userset_id", int(getattr(game, "id", 0) or 0))
        if getattr(self, "_lobby_pending_userset_name", ""):
            setattr(game, "_lobby_userset_name", self._lobby_pending_userset_name)
        elif getattr(self.user, "_lobby_userset_name", ""):
            setattr(game, "_lobby_userset_name", getattr(self.user, "_lobby_userset_name"))
        if getattr(self, "_lobby_pending_userset_cf", ""):
            setattr(game, "_lobby_userset_cf", _normalize_lobby_userset_cf(self._lobby_pending_userset_cf))
        if getattr(self, "_lobby_pending_userset_sf", ""):
            setattr(game, "_lobby_userset_sf", self._lobby_pending_userset_sf)
        if getattr(self, "_lobby_pending_userset_desc", ""):
            setattr(game, "_lobby_userset_desc", self._lobby_pending_userset_desc)
        if getattr(self, "_lobby_pending_userset_params", ""):
            setattr(game, "_lobby_userset_params", self._lobby_pending_userset_params)
        if int(getattr(self, "_lobby_pending_userset_size", 0) or 0):
            setattr(game, "_lobby_userset_size", int(self._lobby_pending_userset_size))

    def _lobby_user_is_live(self, uid: int) -> bool:
        user = self.srv.users.get(int(uid)) if hasattr(self.srv, "users") else None
        return bool(user is not None and getattr(user, "connected", True))

    def _lobby_prune_stale_games(self) -> None:
        for game in list(self.srv.games.list_games()):
            if getattr(game, "state", GAME_STATE_OPEN) == GAME_STATE_ACTIVE:
                continue
            host_uid = int(getattr(game, "host_uid", 0) or 0)
            if host_uid and not self._lobby_user_is_live(host_uid):
                self.srv.games.destroy(int(game.id), reason=f"stale_host:{host_uid}")
                continue
            for uid in list(getattr(game, "participants", []) or []):
                if not self._lobby_user_is_live(int(uid)):
                    game.remove_player(int(uid))
            if not list(getattr(game, "participants", []) or []):
                self.srv.games.destroy(int(game.id), reason="stale_empty")

    def _visible_open_games(self, viewer: Optional[User] = None) -> list:
        self._lobby_prune_stale_games()
        games = []
        for game in self.srv.games.list_games():
            if getattr(game, "state", GAME_STATE_OPEN) != GAME_STATE_OPEN:
                continue
            host_uid = int(getattr(game, "host_uid", 0) or 0)
            if host_uid and not self._lobby_user_is_live(host_uid):
                continue
            games.append(game)
        return games

    def _lobby_game_userset_flags(self, game) -> int:
        return _u32(_decode_lobby_flag_set(getattr(game, "_lobby_userset_cf", "JKM-"), 0))

    def _lobby_game_userset_sysflags(self, game) -> int:
        return _u32(_decode_lobby_flag_set(getattr(game, "_lobby_userset_sf", "KV"), 0))

    def _lobby_game_custom_flags(self, game) -> int:
        if hasattr(game, "_lobby_game_custflags"):
            return _u32(getattr(game, "_lobby_game_custflags", 0))
        try:
            return _u32(int(getattr(game, "flags", 0) or 0))
        except (TypeError, ValueError):
            return 0

    def _lobby_game_sysflags(self, game) -> int:
        base = int(getattr(game, "_lobby_game_sysflags", 0) or 0)
        if getattr(game, "state", GAME_STATE_OPEN) == GAME_STATE_ACTIVE:
            base |= RACE_SYSFLAGS
        return _u32(base)

    def _lobby_search_name_matches(self, game, wanted: str) -> bool:
        wanted = str(wanted or "").strip().lower()
        if not wanted:
            return True
        candidates = [
            str(getattr(game, "_lobby_userset_name", "") or ""),
            self._host_name_for_game(game),
            str(getattr(game, "custom", "") or ""),
            str(self._lobby_room_ident(game)),
            str(self._lobby_game_protocol_id(game)),
        ]
        for candidate in candidates:
            haystack = str(candidate or "").strip().lower()
            if haystack and wanted in haystack:
                return True
        return False

    def _lobby_search_mask_matches(
        self,
        flags: int,
        kv: dict[str, str],
        flag_key: str,
        mask_key: str,
        *,
        ignored_bits: int = 0,
    ) -> bool:
        wanted = _u32(_parse_lobby_int(kv.get(flag_key), 0))
        mask = _u32(_parse_lobby_int(kv.get(mask_key), 0))
        if ignored_bits:
            mask &= ~_u32(ignored_bits)
            wanted &= mask
            flags = _u32(flags) & ~_u32(ignored_bits)
        return (_u32(flags) & mask) == wanted

    def _lobby_search_game_matches(self, game, kv: dict[str, str], *, userset: bool) -> bool:
        if not self._lobby_search_name_matches(game, kv.get("NAME", "")):
            return False
        if userset:
            custom_flags = self._lobby_game_userset_flags(game)
            sysflags = self._lobby_game_userset_sysflags(game)
        else:
            custom_flags = self._lobby_game_custom_flags(game)
            sysflags = self._lobby_game_sysflags(game)
        ignored_custom_bits = _lobby_userset_ignored_search_bits(kv) if userset else 0
        if not self._lobby_search_mask_matches(
            custom_flags,
            kv,
            "CUSTFLAGS",
            "CUSTMASK",
            ignored_bits=ignored_custom_bits,
        ):
            return False
        if not self._lobby_search_mask_matches(sysflags, kv, "SYSFLAGS", "SYSMASK"):
            return False
        return True

    def _lobby_search_games(self, games: list, kv: dict[str, str], *, userset: bool) -> list:
        start = max(0, _safe_int(kv.get("START", 0), 0))
        count = _safe_int(kv.get("COUNT", 50), 50)
        count = max(0, min(count, 100))
        if count <= 0:
            return []
        out = []
        for index, game in enumerate(games):
            if index < start:
                continue
            if not self._lobby_search_game_matches(game, kv, userset=userset):
                continue
            out.append(game)
            if len(out) >= count:
                break
        return out

    def _lobby_active_room_reentry_game(self, kv: dict[str, str] | None = None):
        game = self.srv.games.get(int(getattr(self.user, "game", 0) or 0)) if getattr(self.user, "game", 0) else None
        if game is None or getattr(game, "state", GAME_STATE_OPEN) != GAME_STATE_ACTIVE:
            return None
        if int(getattr(self.user, "uid", 0) or 0) not in self._lobby_game_participant_uid_set(game):
            return None
        kv = kv or {}
        wanted = (kv.get("NAME", "") or "").strip().lower()
        if not wanted:
            return None
        candidates = {
            str(getattr(game, "_lobby_userset_name", "") or "").strip().lower(),
            self._host_name_for_game(game).strip().lower(),
            str(getattr(game, "custom", "") or "").strip().lower(),
            str(self._lobby_room_ident(game)).strip().lower(),
        }
        candidates.discard("")
        return game if wanted in candidates else None

    def _lobby_active_room_reentry_frames(
        self,
        game,
        *,
        include_userset: bool = True,
        include_members: bool = True,
        include_status: bool = True,
    ) -> bytes:
        if game is None:
            return b""
        self.user.stat = STAT_ROOM
        self._lobby_active_room_reentry_game_id = int(getattr(game, "id", 0) or 0)
        reentered = getattr(game, "_lobby_room_reentry_uids", None)
        if not isinstance(reentered, set):
            reentered = set()
            setattr(game, "_lobby_room_reentry_uids", reentered)
        reentered.add(int(getattr(self.user, "uid", 0) or 0))
        room_ident = self._lobby_room_ident(game)
        parts = [
            self._make_20922_tab_message(
                "+who",
                self._lobby_who_fields_for(
                    self.user,
                    aux_text=self._lobby_aux_for_presence(self.user),
                    game_active=False,
                    game_id=int(game.id),
                ),
            )
        ]
        if include_userset:
            parts.append(self._make_20922_tab_message("+ust", self._lobby_userset_fields(game)))
        if include_members:
            for uid in self._lobby_game_participant_uids(game):
                user = self.srv.users.get(int(uid))
                if user is not None:
                    parts.append(
                        self._make_20922_tab_message(
                            "+usm",
                            self._lobby_usm_fields_for_user(user, game_id=int(game.id), active=False),
                        )
                    )
        else:
            parts.append(
                self._make_20922_tab_message(
                    "+usm",
                    self._lobby_usm_fields_for_user(self.user, game_id=int(game.id), active=False),
                )
            )
        if include_status:
            parts.append(self._make_20922_tab_message("+mgm", [f"IDENT={room_ident}"]))
            parts.append(self._lobby_sst_frame())
        return b"".join(parts)

    def _lobby_should_force_room_view(self, game) -> bool:
        if game is None:
            return False
        game_id = int(getattr(game, "id", 0) or 0)
        if int(getattr(self, "_lobby_active_room_reentry_game_id", 0) or 0) == game_id:
            return True
        reentered = getattr(game, "_lobby_room_reentry_uids", set()) or set()
        return bool(reentered)

    def _lobby_sele_frame(self, kv: dict[str, str] | None = None) -> bytes:
        kv = kv or {}
        fields = [
            "MYGAME=1",
            "ROOMS=1",
            "USERSETS=1",
            "USERS=1",
            "GAMES=1",
            f"USERSET0={kv.get('USERSET0', '')}",
            f"INGAME={kv.get('INGAME', '0') or '0'}",
        ]
        return self._make_20922_tab_message("sele", fields)

    def _lobby_ingame_sele_frame(self) -> bytes:
        return self._make_20922_tab_message(
            "sele",
            [
                "GAMES=0",
                "MYGAME=0",
                "ROOMS=0",
                "USERS=0",
                "USERSETS=0",
                "MESGS=1",
                "MESGTYPES=GPY",
                "ASYNC=0",
                "STATS=0",
                "SLOTS=3",
                "INGAME=1",
            ],
        )

    def _lobby_lobby_snapshot_for(self, user: User, search_kv: dict | None = None) -> bytes:
        parts = []
        game = self.srv.games.get(int(getattr(user, "game", 0) or 0)) if getattr(user, "game", 0) else None
        if game is not None:
            parts.append(self._make_20922_tab_message("+mgm", self._lobby_game_ready_snapshot_fields(game, viewer_uid=int(user.uid))))
        parts.append(self._lobby_sst_frame())
        return b"".join(parts)

    # ------------------------------------------------------------------ #
    # Command dispatch                                                    #
    # ------------------------------------------------------------------ #

    def _handle_plain_prelogin_frame(self, cmd: str, payload: bytes, reserved_be32: int = 0):
        kv = parse_20922_kv(payload)
        self._trace_lobby_frame("recv", cmd, kv, len(payload) + 12, reserved_be32=reserved_be32)
        tail = b""
        nul = payload.find(b"\x00")
        if nul >= 0 and nul + 1 < len(payload):
            tail = payload[nul + 1 :]
        log.debug("[uid=%d] frame cmd=%s token=%08x kv=%s", self.user.uid, cmd, reserved_be32 & 0xFFFFFFFF, sorted(kv))

        if cmd == "addr":
            self._probe_client_addr = kv.get("ADDR", self._probe_client_addr)
            self._probe_client_port = kv.get("PORT", self._probe_client_port)
            self.user.laddr = self._probe_client_addr
            frame = self._make_20922_signed_binary_message("addr", b"\x00", 9)
            if self._lobby_prelogin_burst_after_news_enabled():
                self._probe_deferred_addr_frame = frame
                return
            self._send_bootstrap_bytes(frame)
            return
        if cmd == "skey":
            frame = self._make_20922_signed_binary_message("skey", b"\x00", 9)
            if self._lobby_prelogin_burst_after_news_enabled():
                self._probe_deferred_skey_frame = frame
                return
            self._send_bootstrap_bytes(frame)
            return
        if cmd == "news":
            burst = b""
            if self._lobby_prelogin_burst_after_news_enabled():
                burst = self._probe_deferred_addr_frame + self._probe_deferred_skey_frame
                self._probe_deferred_addr_frame = b""
                self._probe_deferred_skey_frame = b""
            self._send_bootstrap_bytes(burst + self._lobby_news_with_endpoint_advertisement())
            return
        if cmd == "~png":
            self._schedule_lobby_news_auth_followups()
            return

        if cmd == "*ath":
            self._probe_seen_auth = True
            name, persona = self._display_and_persona_from_kv(kv)
            if not self._server_accepts_new_user():
                self._lobby_reject_auth("server_full", name, reserved_be32=AUTH_DBER_RESERVED)
                return
            if not self._ensure_registered_user():
                reason = "admin_ban" if self._disconnect_reason == "admin_ban" else "server_full"
                reserved = AUTH_BLAK_RESERVED if reason == "admin_ban" else AUTH_DBER_RESERVED
                self._lobby_reject_auth(reason, name, reserved_be32=reserved)
                return
            remember = getattr(self.srv, "remember_control_profile", None)
            if callable(remember):
                remember(name=name, persona=persona, client_addr=self._probe_client_addr)
            self._send_bootstrap_bytes(
                self._make_token_tab_reply(
                    reserved_be32,
                    [
                        f"NAME={name}",
                        f"GTAG={name}",
                        f"PERSONAS={self._lobby_auth_personas_value(persona)}",
                        "XUID=",
                        f"TOS={self._lobby_tos_value('ATH_TOS', 1)}",
                        "SHARE=1",
                    ],
                )
            )
            return

        if cmd == "auth":
            self._probe_seen_auth = True
            name, persona = self._display_and_persona_from_kv(kv)
            if not self._server_accepts_new_user():
                self._lobby_reject_auth("server_full", name, reserved_be32=AUTH_DBER_RESERVED)
                return
            if not self._lobby_accept_auth(kv, name, persona):
                return
            name = self._probe_display_name or name
            persona = self._probe_persona or persona
            if not self._ensure_registered_user():
                reason = "admin_ban" if self._disconnect_reason == "admin_ban" else "server_full"
                reserved = AUTH_BLAK_RESERVED if reason == "admin_ban" else AUTH_DBER_RESERVED
                self._lobby_reject_auth(reason, name, reserved_be32=reserved)
                return
            remember = getattr(self.srv, "remember_control_profile", None)
            if callable(remember):
                remember(name=name, persona=persona, client_addr=self._probe_client_addr)
            self._send_bootstrap_bytes(self._lobby_auth_frame())
            if self._lobby_news_push_after_auth_enabled():
                self._schedule_lobby_news_push(label="news-push-auth")
            return

        if cmd == "*pat":
            requested = kv.get("PERS", "").strip() or self._probe_persona or self._probe_display_name
            display_name = self._probe_display_name or self.user.name or requested
            if not self._lobby_claim_persona_or_reject(requested, cmd):
                return
            self._probe_persona = requested
            self.user.pers = requested
            self._ensure_registered_user()
            self._lobby_reattach_detached_identity()
            self._send_bootstrap_bytes(
                self._make_token_tab_reply(
                    reserved_be32,
                    [
                        f"PERS={requested}",
                        f"NAME={display_name}",
                        "GTAG=",
                        "XUID=",
                    ],
                )
            )
            return

        if cmd == "PERS":
            requested = kv.get("PERS", "").strip() or self._probe_persona or self._probe_display_name
            display_name = self._probe_display_name or self.user.name or requested
            if not self._lobby_claim_persona_or_reject(requested, cmd):
                return
            self._probe_persona = requested
            self.user.pers = requested
            self._ensure_registered_user()
            self._lobby_reattach_detached_identity()
            self._send_bootstrap_bytes(
                self._make_token_tab_reply(
                    reserved_be32,
                    [
                        f"NAME={display_name}",
                        f"PERS={requested}",
                    ],
                )
                + self._make_20922_tab_message("+usr", self._lobby_usr_fields(sync=1, game_id=0))
            )
            return

        if cmd in ("pers", "cper"):
            requested = kv.get("PERS", "").strip() or self._probe_persona or self._probe_display_name
            display_name = self._probe_display_name or self.user.name or requested
            if not self._lobby_claim_persona_or_reject(requested, cmd):
                return
            self._probe_persona = requested
            self.user.pers = requested
            self._ensure_registered_user()
            self._lobby_reattach_detached_identity()
            remember = getattr(self.srv, "remember_control_profile", None)
            if callable(remember):
                remember(name=display_name, persona=requested, client_addr=self._probe_client_addr)
            self._send_bootstrap_bytes(
                self._lobby_pers_frame(requested, display_name)
                + self._make_20922_tab_message("+usr", self._lobby_usr_fields(sync=1, game_id=0))
            )
            return
        if cmd == "acct":
            create_account = getattr(self.srv, "create_account", None)
            if callable(create_account):
                ok, reason, account, identifier = create_account(kv)
                if ok and account:
                    self._lobby_apply_auth_account(account, identifier, identifier)
                    remember = getattr(self.srv, "remember_control_profile", None)
                    if callable(remember):
                        remember(
                            name=self._probe_display_name or identifier,
                            persona=self._probe_persona or self._probe_display_name or identifier,
                            client_addr=self._probe_client_addr,
                        )
                self._send_bootstrap_bytes(self._lobby_account_create_frame(reason, ok=ok))
                log.info("[uid=%d] account create result=%s id=%s", self.user.uid, reason, (identifier or "-")[:96])
            else:
                self._display_and_persona_from_kv(kv)
                self._send_bootstrap_bytes(self._make_20922_tab_message("acct", ["RESULT=OK"] + self._auth_reply_fields()))
            return

        if cmd == "user":
            remember = getattr(self.srv, "remember_control_profile", None)
            if callable(remember):
                remember(
                    name=self._probe_display_name or self.user.name,
                    persona=self._probe_persona or self.user.pers,
                    client_addr=self._probe_client_addr,
                )
            self._ensure_registered_user()
            self._send_bootstrap_bytes(
                self._lobby_user_frame()
                + self._lobby_online_who_snapshot(include_self=True)
                + self._lobby_sst_frame()
                + self._lobby_auxi_frame()
            )
            self._lobby_broadcast_online_who(self.user, delay_s=0.03, exclude_uid=int(self.user.uid))
            return

        self._ensure_registered_user()

        handlers = {
            "sele": self._cmd_sele,
            "usea": self._cmd_usea,
            "ucre": self._cmd_ucre,
            "UCRE": self._cmd_ucre,
            "uadm": self._cmd_uadm,
            "UADM": self._cmd_uadm,
            "ujoi": self._cmd_ujoi,
            "gsea": self._cmd_gsea,
            "gcre": self._cmd_gcre,
            "GCRE": self._cmd_gcre,
            "gjoi": self._cmd_gjoi,
            "GJOI": self._cmd_gjoi,
            "gset": self._cmd_gset,
            "GSET": self._cmd_gset,
            "auxi": self._cmd_auxi,
            "AUXI": self._cmd_auxi,
            "mesg": self._cmd_mesg,
            "MESG": self._cmd_mesg,
            "onln": self._cmd_onln,
            "gsta": self._cmd_gsta,
            "GSTA": self._cmd_gsta,
            "RANK": self._cmd_rank,
            "glea": self._cmd_leave,
            "gdel": self._cmd_leave,
            "TERM": self._cmd_leave,
            "KICK": self._cmd_kick,
            "*con": self._cmd_con,
            "@cnt": self._cmd_heartbeat,
            "@alv": self._cmd_heartbeat,
        }
        handler = handlers.get(cmd)
        if handler is not None:
            handler(cmd, kv, payload, reserved_be32, tail)
            return
        self._send_bootstrap_bytes(self._make_20922_tab_message(cmd[:4] if len(cmd) >= 4 else "null", ["RESULT=OK"]))

    def _trace_lobby_bytes(self, direction: str, data: bytes, *, label: str = "") -> None:
        if not _cfg_bool(getattr(self.srv, "cfg", {}), "LOBBY_FRAME_TRACE", False):
            return
        offset = 0
        while offset < len(data):
            frame = parse_mw_frame(data, offset)
            if frame is None:
                break
            self._trace_lobby_frame(
                direction,
                frame.command,
                parse_20922_kv(frame.payload),
                frame.total_len,
                reserved_be32=frame.reserved_be32,
                label=label,
            )
            offset += frame.total_len

    def _trace_lobby_frame(
        self,
        direction: str,
        cmd: str,
        kv: dict[str, str],
        total_len: int,
        *,
        reserved_be32: int = 0,
        label: str = "",
    ) -> None:
        if not _cfg_bool(getattr(self.srv, "cfg", {}), "LOBBY_FRAME_TRACE", False):
            return
        if cmd not in TRACE_CMDS:
            return
        keys = (
            "IDENT", "I", "NAME", "N", "HOST", "START", "COUNT", "CUSTFLAGS", "CUSTMASK", "SYSFLAGS", "SYSMASK", "GAME", "G", "US",
            "OPPO0", "OPPO1", "OPPO2", "OPID0", "OPID1", "OPID2", "ADDR", "PORT", "A", "LA",
            "ADDR0", "LADDR0", "ADDR1", "LADDR1", "ADDR2", "LADDR2",
            "PARAMS", "ATTR", "FLAGS", "F", "TEXT", "T", "X",
        )
        fields = " ".join(f"{key}={kv[key]}" for key in keys if key in kv)
        log.info(
            "[uid=%d] 20922 trace %s%s cmd=%s len=%d token=%08x %s",
            int(getattr(self.user, "uid", 0) or 0),
            direction,
            f" label={label}" if label else "",
            cmd,
            total_len,
            reserved_be32 & 0xFFFFFFFF,
            fields or "-",
        )

    def _cmd_con(self, cmd, kv, payload, reserved_be32, tail):
        self._send_bootstrap_bytes(self._make_20922_tab_message("*con", []))

    def _cmd_heartbeat(self, cmd, kv, payload, reserved_be32, tail):
        if reserved_be32:
            self._send_bootstrap_bytes(self._make_token_tab_reply(reserved_be32, []))

    def _cmd_sele(self, cmd, kv, payload, reserved_be32, tail):
        self._probe_seen_sele = True
        if kv.get("INGAME", "").strip() == "1" and self._lobby_race_bootstrap_done:
            self._send_bootstrap_bytes(self._lobby_ingame_sele_frame())
            return
        burst = b"".join(
            [
                self._make_20922_tab_message("+who", self._lobby_who_fields_for(self.user, game_active=False)),
                self._lobby_sele_frame(kv),
                self._lobby_sst_frame(),
            ]
        )
        self._send_bootstrap_bytes(burst)

    def _cmd_usea(self, cmd, kv, payload, reserved_be32, tail):
        games = self._visible_open_games(self.user)
        reentry_game = self._lobby_active_room_reentry_game(kv)
        if reentry_game is not None and all(int(getattr(game, "id", 0) or 0) != int(reentry_game.id) for game in games):
            games.append(reentry_game)
        games = self._lobby_search_games(games, kv, userset=True)
        if reentry_game is not None and all(int(getattr(game, "id", 0) or 0) != int(reentry_game.id) for game in games):
            reentry_game = None
        parts = []
        if reentry_game is not None:
            parts.append(self._lobby_active_room_reentry_frames(reentry_game))
        parts.append(self._make_20922_tab_message("usea", [f"COUNT={len(games)}"]))
        for game in games:
            parts.append(self._make_20922_tab_message("+uss", self._lobby_userset_fields(game)))
        self._send_bootstrap_bytes(b"".join(parts))

    def _cmd_ucre(self, cmd, kv, payload, reserved_be32, tail):
        name = kv.get("NAME", "").strip() or self._lobby_display_name()
        self.user.room = 1
        setattr(self.user, "_lobby_userset_name", name)
        game = self.srv.games.get(int(getattr(self.user, "game", 0) or 0)) if getattr(self.user, "game", 0) else None
        if game is not None:
            setattr(game, "_lobby_userset_name", name)
            setattr(game, "_lobby_userset_id", int(getattr(game, "id", 0) or 0))
            if "PARAMS" in kv:
                setattr(game, "_lobby_userset_params", _normalize_aux_text(kv.get("PARAMS", "")))
            if "CUSTFLAGS" in kv:
                setattr(game, "_lobby_userset_cf", _normalize_lobby_userset_cf(kv.get("CUSTFLAGS")))
            if "SYSFLAGS" in kv:
                setattr(game, "_lobby_userset_sf", kv.get("SYSFLAGS") or "KV")
            if "SF" in kv:
                setattr(game, "_lobby_userset_sf", kv.get("SF") or "KV")
            if "SIZE" in kv:
                setattr(game, "_lobby_userset_size", max(1, _safe_int(kv.get("SIZE"), 4)))
            fields = self._lobby_userset_fields(game)
            self._lobby_broadcast_userset_update(game, fields, exclude_uid=int(self.user.uid))
        else:
            fields = self._lobby_remember_pending_userset(kv)
        if cmd == "UCRE" or reserved_be32:
            self._send_bootstrap_bytes(self._make_20922_tab_message("+ust", fields))
        else:
            parts = [self._make_20922_tab_message("ucre", fields)]
            if game is None:
                parts.extend(
                    [
                        self._make_20922_tab_message("+who", self._lobby_who_fields_for(self.user, game_active=False)),
                        self._make_20922_tab_message("+ust", fields),
                        self._make_20922_tab_message("+usm", self._lobby_usm_fields_for_user(self.user, active=False)),
                    ]
                )
            self._send_bootstrap_bytes(b"".join(parts))

    def _cmd_uadm(self, cmd, kv, payload, reserved_be32, tail):
        game = self.srv.games.get(int(getattr(self.user, "game", 0) or 0)) if getattr(self.user, "game", 0) else None
        if game is not None:
            if "NAME" in kv and kv.get("NAME", "").strip():
                setattr(game, "_lobby_userset_name", kv.get("NAME", "").strip())
            if "CF" in kv:
                setattr(game, "_lobby_userset_cf", _normalize_lobby_userset_cf(kv.get("CF")))
            if "CUSTFLAGS" in kv:
                setattr(game, "_lobby_userset_cf", _normalize_lobby_userset_cf(kv.get("CUSTFLAGS")))
            if "SYSFLAGS" in kv:
                setattr(game, "_lobby_userset_sf", kv.get("SYSFLAGS") or "KV")
            if "SF" in kv:
                setattr(game, "_lobby_userset_sf", kv.get("SF") or "KV")
            if "PARAMS" in kv:
                setattr(game, "_lobby_userset_params", _normalize_aux_text(kv.get("PARAMS", "")))
            if "DESC" in kv:
                setattr(game, "_lobby_userset_desc", kv.get("DESC", ""))
            self._lobby_ensure_game_userset_for(self.user, game)
            fields = self._lobby_userset_fields(game)
            self._lobby_broadcast_userset_update(game, fields, exclude_uid=int(self.user.uid))
        else:
            if kv.get("NAME", "").strip():
                self._lobby_remember_pending_userset(kv)
            fields = self._lobby_pending_userset_fields()
        self._lobby_last_uadm_at = time.time()
        if cmd == "UADM" or reserved_be32:
            self._send_bootstrap_bytes(self._make_20922_tab_message("+ust", fields))
        else:
            parts = [self._make_20922_tab_message("uadm", fields)]
            if game is not None:
                display_game = self._lobby_room_ident(game)
                force_room_view = self._lobby_should_force_room_view(game)
                parts.extend(
                    [
                        self._make_20922_tab_message(
                            "+who",
                            self._lobby_who_fields_for(
                                self.user,
                                aux_text=self._lobby_aux_for_presence(self.user),
                                game_active=False,
                                game_id=int(game.id),
                                display_game_id=display_game,
                                force_display_game_id=True,
                            ),
                        ),
                        self._make_20922_tab_message("+ust", fields),
                        self._make_20922_tab_message(
                            "+usm",
                            self._lobby_usm_fields_for_user(self.user, game_id=int(game.id), display_game_id=display_game),
                        ),
                        self._make_20922_tab_message(
                            "+mgm",
                            self._lobby_game_ready_snapshot_fields(
                                game,
                                viewer_uid=int(self.user.uid),
                                force_room_view=force_room_view,
                            ),
                        ),
                    ]
                )
            self._send_bootstrap_bytes(b"".join(parts))

    def _cmd_ujoi(self, cmd, kv, payload, reserved_be32, tail):
        name = kv.get("NAME", "").strip()
        game = self._find_game_by_name_or_ident(name, open_only=True)
        if game is not None:
            self._lobby_ensure_game_userset_for(self.srv.users.get(int(game.host_uid)) or self.user, game)
            fields = self._lobby_userset_fields(game)
            fields = ["C=2" if field.startswith("C=") else field for field in fields]
        else:
            fields = ["IDENT=0", f"NAME={name}", "C=0"]
        self._send_bootstrap_bytes(self._make_20922_tab_message("ujoi", fields))

    def _cmd_gsea(self, cmd, kv, payload, reserved_be32, tail):
        self._lobby_last_gsea_kv = dict(kv)
        games = self._lobby_search_games(self._visible_open_games(self.user), kv, userset=False)
        parts = [self._make_20922_tab_message("gsea", [f"COUNT={len(games)}"])]
        for game in games:
            parts.append(self._make_20922_tab_message("+gam", self._lobby_gam_fields(game)))
            parts.append(self._make_20922_tab_message("+mgm", self._lobby_game_ready_snapshot_fields(game, viewer_uid=int(self.user.uid))))
        parts.append(self._lobby_sst_frame(gcr=len(games), gcm=1 if games else 0))
        self._send_bootstrap_bytes(b"".join(parts))

    def _cmd_gcre(self, cmd, kv, payload, reserved_be32, tail):
        name = kv.get("NAME", "").strip() or self._lobby_display_name()
        limit = max(2, _safe_int(kv.get("MAXSIZE", kv.get("LIMIT", 4)), 4))
        minsize = max(2, _safe_int(kv.get("MINSIZE", 2), 2))
        params = _normalize_aux_text(kv.get("PARAMS", ""))
        host, port = self._race_endpoint_for(self.user)
        game = self.srv.games.get(int(getattr(self.user, "game", 0) or 0)) if getattr(self.user, "game", 0) else None
        active_host_reentry = (
            game is not None
            and getattr(game, "state", GAME_STATE_OPEN) == GAME_STATE_ACTIVE
            and int(getattr(game, "host_uid", 0) or 0) == int(self.user.uid)
        )
        if active_host_reentry:
            self.user.stat = STAT_ROOM
            self._lobby_active_room_reentry_game_id = int(game.id)
            if "PARAMS" in kv:
                setattr(game, "_lobby_params", params)
            if "MINSIZE" in kv:
                setattr(game, "_lobby_minsize", minsize)
            if "CUSTFLAGS" in kv:
                setattr(game, "_lobby_game_custflags", _u32(_parse_lobby_int(kv.get("CUSTFLAGS"), 0)))
            if "SYSFLAGS" in kv:
                setattr(game, "_lobby_game_sysflags", _u32(_parse_lobby_int(kv.get("SYSFLAGS"), 0)) & ~RACE_SYSFLAGS)
            self._lobby_apply_pending_userset_to_game(game)
            self._lobby_ensure_game_userset_for(self.user, game)
            parts = [
                self._make_20922_tab_message(
                    "+who",
                    self._lobby_who_fields_for(
                        self.user,
                        aux_text=self._lobby_aux_for_presence(self.user),
                        game_active=False,
                        game_id=int(game.id),
                    ),
                ),
                self._make_20922_tab_message("+ust", self._lobby_userset_fields(game)),
                self._make_20922_tab_message("+usm", self._lobby_usm_fields_for_user(self.user, game_id=int(game.id), active=False)),
                self._make_20922_tab_message(
                    "gcre",
                    self._lobby_game_ready_snapshot_fields(
                        game,
                        viewer_uid=int(self.user.uid),
                        snapshot_params="",
                        force_room_view=True,
                    ),
                ),
            ]
            self._send_bootstrap_bytes(b"".join(parts))
            return
        if game is not None and (
            getattr(game, "state", GAME_STATE_OPEN) != GAME_STATE_OPEN
            or int(getattr(game, "host_uid", 0) or 0) != int(self.user.uid)
        ):
            old_state = getattr(game, "state", GAME_STATE_OPEN)
            old_game, removed = self.srv.games.leave(int(game.id), int(self.user.uid))
            self.user.game = 0
            self.user.stat = STAT_LOBBY
            if old_state == GAME_STATE_OPEN:
                self._lobby_on_game_departure(old_game or game, departed_uid=int(self.user.uid), removed=removed)
            game = None
        if game is None:
            game = self.srv.games.create(0, self.user.uid, limit=limit, custom=name, addr=host or self.user.ip, port=port or 3658, minsize=minsize)
            if game is None:
                self._send_bootstrap_bytes(self._make_20922_tab_message("gcre", ["ERR=full"]))
                return
            self.srv.games.join(game.id, self.user.uid)
            self.user.game = game.id
            self.user.stat = STAT_ROOM
            setattr(game, "_lobby_when", _stock_when(getattr(game, "created_at", None)))
            self._lobby_apply_pending_userset_to_game(game)
        setattr(game, "_lobby_params", params)
        setattr(game, "_lobby_minsize", minsize)
        if "CUSTFLAGS" in kv:
            setattr(game, "_lobby_game_custflags", _u32(_parse_lobby_int(kv.get("CUSTFLAGS"), 0)))
        if "SYSFLAGS" in kv:
            setattr(game, "_lobby_game_sysflags", _u32(_parse_lobby_int(kv.get("SYSFLAGS"), 0)) & ~RACE_SYSFLAGS)
        self._lobby_apply_pending_userset_to_game(game)
        self._lobby_ensure_game_userset_for(self.user, game)
        if cmd == "GCRE" or reserved_be32:
            self._send_bootstrap_bytes(
                self._make_20922_tab_message("+usr", self._lobby_usr_fields(sync=6, game_id=game.id, user=self.user))
                + self._make_20922_tab_message("+gam", self._lobby_gam_fields(game))
            )
            return
        parts = [
            self._make_20922_tab_message(
                "+who",
                self._lobby_who_fields_for(
                    self.user,
                    aux_text=self._lobby_aux_for_presence(self.user),
                    game_active=False,
                    game_id=int(game.id),
                ),
            ),
            self._make_20922_tab_message("+usm", self._lobby_usm_fields_for_user(self.user, game_id=int(game.id), active=False)),
            self._make_20922_tab_message("gcre", self._lobby_game_ready_snapshot_fields(game, viewer_uid=int(self.user.uid), snapshot_params="")),
        ]
        self._send_bootstrap_bytes(b"".join(parts))

    def _cmd_gjoi(self, cmd, kv, payload, reserved_be32, tail):
        name = kv.get("NAME", "").strip()
        ident = _safe_int(kv.get("IDENT", 0), 0)
        game = self._find_game_by_name_or_ident(name, ident, open_only=True)
        reentry_game = None
        if game is None:
            reentry_game = self._lobby_active_room_reentry_game(kv)
            game = reentry_game
        if game is None:
            frame = self._make_token_tab_reply(reserved_be32, ["ERR=not_found"]) if reserved_be32 else self._make_20922_tab_message(cmd[:4], ["ERR=not_found"])
            self._send_bootstrap_bytes(frame)
            return
        if reentry_game is not None:
            self.user.game = game.id
            self.user.stat = STAT_ROOM
            self._send_bootstrap_bytes(
                self._lobby_active_room_reentry_frames(
                    game,
                    include_userset=False,
                    include_members=False,
                    include_status=False,
                )
                + self._make_20922_tab_message(
                    cmd[:4],
                    self._lobby_game_ready_snapshot_fields(
                        game,
                        viewer_uid=int(self.user.uid),
                        force_room_view=True,
                    ),
                )
            )
            display_game = self._lobby_room_ident(game)
            for uid in self._lobby_game_participant_uids(game):
                if int(uid) == int(self.user.uid):
                    continue
                handler = self._lobby_handler_for_uid(uid)
                if handler is None:
                    continue
                handler._send_bootstrap_bytes(
                    handler._make_20922_tab_message(
                        "+who",
                        handler._lobby_who_fields_for(
                            self.user,
                            aux_text=handler._lobby_aux_for_presence(self.user),
                            game_active=False,
                            game_id=int(game.id),
                            display_game_id=display_game,
                            force_display_game_id=True,
                        ),
                    )
                    + handler._make_20922_tab_message(
                        "+usm",
                        handler._lobby_usm_fields_for_user(
                            self.user,
                            game_id=int(game.id),
                            display_game_id=display_game,
                        ),
                    )
                    + handler._make_20922_tab_message(
                        "+mgm",
                        handler._lobby_game_ready_snapshot_fields(
                            game,
                            viewer_uid=int(uid),
                            force_room_view=True,
                        ),
                    ),
                    label="post-race-reentry-peer",
                )
            return
        joined = self.srv.games.join(game.id, self.user.uid)
        if not joined and int(self.user.uid) not in self._lobby_game_participant_uid_set(game):
            frame = self._make_token_tab_reply(reserved_be32, ["ERR=not_joinable"]) if reserved_be32 else self._make_20922_tab_message(cmd[:4], ["ERR=not_joinable"])
            self._send_bootstrap_bytes(frame)
            return
        self.user.game = game.id
        self.user.stat = STAT_ROOM
        self._lobby_ensure_game_userset_for(self.srv.users.get(int(game.host_uid)) or self.user, game)
        self._lobby_pending_join_gset_game_id = int(game.id)
        self._lobby_pending_join_gset_uid = int(self.user.uid)
        self._lobby_post_gjoi_auxi_game_id = int(game.id)
        self._lobby_post_gjoi_promoted = False
        self._lobby_post_gjoi_staged = False
        setattr(game, "_lobby_post_gjoi_promoted", False)
        self._lobby_mark_countdown_reset(game, reason="GJOI")
        if _cfg_bool(getattr(self.srv, "cfg", {}), "LOBBY_GJOI_FORCE_READY", False):
            for uid in self._lobby_game_participant_uids(game):
                game.set_ready(uid, True)
            self.user.stat = STAT_GAME
            self._emit_force_ready_join(game)
            return
        if cmd == "GJOI" or reserved_be32:
            parts = [
                self._make_token_tab_reply(reserved_be32, self._lobby_game_ready_snapshot_fields(game, viewer_uid=int(self.user.uid))),
                self._make_20922_tab_message("+usr", self._lobby_usr_fields(sync=3, game_id=game.id, user=self.user)),
                self._make_20922_tab_message("+gam", self._lobby_gam_fields(game)),
                self._lobby_sst_frame(gcr=1, gcm=1, gip=0),
            ]
            self._send_bootstrap_bytes(b"".join(parts))
            return
        self._send_bootstrap_bytes(self._make_20922_tab_message("gjoi", self._lobby_game_ready_snapshot_fields(game, viewer_uid=int(self.user.uid))))
        self._schedule_post_gjoi_staging(game)

    def _emit_force_ready_join(self, game) -> None:
        guest_parts = [
            self._make_20922_tab_message("gjoi", self._lobby_game_ready_snapshot_fields(game, viewer_uid=int(self.user.uid))),
            self._make_20922_tab_message("+usr", self._lobby_usr_fields(sync=3, game_id=game.id, user=self.user)),
            self._make_20922_tab_message("+mgm", self._lobby_game_ready_snapshot_fields(game, viewer_uid=int(self.user.uid))),
            self._lobby_sst_frame(gcr=1, gcm=1, gip=0),
        ]
        self._send_bootstrap_bytes(b"".join(guest_parts))
        for uid in self._lobby_game_participant_uids(game):
            if uid == int(self.user.uid):
                continue
            handler = self._lobby_handler_for_uid(uid)
            if handler:
                handler._send_bootstrap_bytes(
                    handler._make_20922_tab_message("+usr", handler._lobby_usr_fields(sync=3, game_id=game.id, user=self.user))
                    + handler._make_20922_tab_message("+mgm", handler._lobby_game_ready_snapshot_fields(game, viewer_uid=uid))
                )

    def _schedule_post_gjoi_staging(self, game) -> None:
        game_id = int(game.id)
        join_uid = int(self.user.uid)

        def _staging() -> None:
            if self._lobby_post_gjoi_promoted or self._lobby_post_gjoi_auxi_game_id != game_id:
                return
            current = self.srv.games.get(game_id)
            if current is None:
                return
            guest = self.user
            self._send_bootstrap_bytes(
                self._post_gjoi_guest_staging_frames(current, guest),
                label="post-gjoi-staging-guest",
            )
            self._lobby_post_gjoi_staged = True
            for uid in self._lobby_game_participant_uids(current):
                if uid == join_uid:
                    continue
                handler = self._lobby_handler_for_uid(uid)
                if handler:
                    handler._send_bootstrap_bytes(
                        handler._make_20922_tab_message("+ust", handler._lobby_userset_fields(current))
                        + handler._make_20922_tab_message("+usm", handler._lobby_usm_fields_for_user(guest, game_id=game_id, active=False)),
                        label="post-gjoi-staging-host",
                    )

        self._send_later_bytes(0.03, b"", should_send=lambda: True)
        timer = threading.Timer(0.03, _staging)
        timer.daemon = True
        timer.start()

    def _post_gjoi_guest_staging_frames(self, game, guest: User) -> bytes:
        frames = [
            self._make_20922_tab_message("+who", self._lobby_who_fields_for(guest, game_active=False)),
            self._make_20922_tab_message("+ust", self._lobby_userset_fields(game)),
        ]
        for uid in self._lobby_game_participant_uids(game):
            user = self.srv.users.get(uid)
            if user is None:
                continue
            if int(uid) == int(getattr(guest, "uid", 0) or 0):
                frames.append(self._make_20922_tab_message("+usm", self._lobby_usm_fields_for_user(user, game_id=int(game.id), active=False)))
            else:
                frames.append(
                    self._make_20922_tab_message(
                        "+usm",
                        self._lobby_usm_fields_for_user(
                            user,
                            game_id=int(game.id),
                            display_game_id=self._lobby_room_ident(game),
                        ),
                    )
                )
        return b"".join(frames)

    def _cmd_gset(self, cmd, kv, payload, reserved_be32, tail):
        game = self.srv.games.get(int(getattr(self.user, "game", 0) or 0)) if getattr(self.user, "game", 0) else None
        name = kv.get("NAME", "").strip()
        if game is None:
            ident = _safe_int(kv.get("IDENT", 0), 0)
            game = self._find_game_by_name_or_ident(name, ident)
        if game is not None and "PARAMS" in kv:
            setattr(game, "_lobby_params", _normalize_aux_text(kv.get("PARAMS", "")))
        force_room_view = self._lobby_should_force_room_view(game)
        if game is not None and getattr(game, "state", "") == GAME_STATE_ACTIVE and not force_room_view:
            fields = self._lobby_game_ready_snapshot_fields(
                game,
                viewer_uid=int(self.user.uid),
                tunnel_addrs=True,
                include_relay=_cfg_bool(getattr(self.srv, "cfg", {}), "INCLUDE_RELAY_FIELDS", False),
            )
        elif game is not None:
            fields = self._lobby_game_ready_snapshot_fields(
                game,
                viewer_uid=int(self.user.uid),
                tunnel_addrs=False,
                force_room_view=force_room_view,
            )
        else:
            fields = ["COUNT=0"]
        if self._lobby_pending_join_gset_game_id == int(getattr(game, "id", 0) or 0):
            self._lobby_pending_join_gset_game_id = 0
            self._lobby_pending_join_gset_uid = 0
        if cmd == "GSET" or reserved_be32:
            if game is not None and getattr(game, "state", "") == GAME_STATE_ACTIVE:
                self._send_bootstrap_bytes(self._lobby_alt_token_ack(reserved_be32))
                return
            self._send_bootstrap_bytes(self._make_token_tab_reply(reserved_be32, fields))
        else:
            self._send_bootstrap_bytes(self._make_20922_tab_message("gset", fields))

    def _cmd_auxi(self, cmd, kv, payload, reserved_be32, tail):
        game = self.srv.games.get(int(getattr(self.user, "game", 0) or 0)) if getattr(self.user, "game", 0) else None
        rank = int(getattr(game, "_lobby_ready_aux_phase_rank", 0) or 0) if game is not None else 0
        text = _normalize_aux_text(kv.get("TEXT", "").strip())
        if text and rank < 2:
            self._probe_aux_text = text
            self.user.aux = text
        elif rank >= 2:
            self._probe_aux_text = self._lobby_aux_for(self.user)
        ack = self._make_token_tab_reply(reserved_be32, [f"TEXT={self._probe_aux_text}"]) if reserved_be32 else self._make_20922_tab_message("auxi", [f"TEXT={self._probe_aux_text}"])
        self._send_bootstrap_bytes(ack)
        if game is None:
            return
        if getattr(game, "state", "") == GAME_STATE_ACTIVE:
            self._emit_active_auxi_refresh(game, self.user)
            return
        if self._lobby_post_gjoi_auxi_game_id == int(game.id) and not self._lobby_post_gjoi_promoted:
            self._promote_post_gjoi(game)
            return
        if rank >= 2:
            self._emit_presence_refresh(game, delays=(0.02,), include_self=True, ignore_countdown=True)
            return
        if rank == 1:
            self._lobby_emit_auxi_game_refresh_for_subject(game, self.user, delays=(0.03,), phase_rank=rank)
        else:
            self._emit_presence_refresh(game, delays=(0.02,), include_onln=True)

    def _promote_post_gjoi(self, game) -> None:
        display_game = self._lobby_room_ident(game)
        self._lobby_post_gjoi_promoted = True
        self._lobby_post_gjoi_auxi_game_id = 0
        setattr(game, "_lobby_post_gjoi_promoted", True)
        staging = b"" if self._lobby_post_gjoi_staged else self._post_gjoi_guest_staging_frames(game, self.user)
        self._lobby_post_gjoi_staged = True
        self._send_bootstrap_bytes(
            staging
            + self._lobby_join_slot_reanchor_frames(game, self.user)
            + self._make_20922_tab_message(
                "+who",
                self._lobby_who_fields_for(
                    self.user,
                    aux_text=self._lobby_aux_for_presence(self.user),
                    game_active=False,
                    game_id=int(game.id),
                    display_game_id=display_game,
                    force_display_game_id=True,
                ),
            )
            + self._make_20922_tab_message(
                "+usm",
                self._lobby_usm_fields_for_user(self.user, game_id=int(game.id), display_game_id=display_game),
            )
            + self._make_20922_tab_message("+mgm", self._lobby_game_ready_snapshot_fields(game, viewer_uid=int(self.user.uid))),
            label="post-gjoi-promote-guest",
        )
        for uid in self._lobby_game_participant_uids(game):
            if uid == int(self.user.uid):
                continue
            handler = self._lobby_handler_for_uid(uid)
            if handler:
                handler._send_bootstrap_bytes(
                    handler._lobby_join_slot_reanchor_frames(game, self.user)
                    + handler._make_20922_tab_message("+ust", handler._lobby_userset_fields(game))
                    + handler._make_20922_tab_message(
                        "+usm",
                        handler._lobby_usm_fields_for_user(self.user, game_id=int(game.id), active=False),
                    ),
                    label="post-gjoi-promote-peer",
                )

    def _cmd_mesg(self, cmd, kv, payload, reserved_be32, tail):
        attr = (kv.get("ATTR", "") or kv.get("F", "") or kv.get("FLAGS", "")).strip().upper()
        text = kv.get("TEXT", "").strip()
        game = self.srv.games.get(int(getattr(self.user, "game", 0) or 0)) if getattr(self.user, "game", 0) else None
        if game is not None and not attr:
            if text == "42":
                attr = "EGS"
            elif "TIME%3d" in text or "TIME=" in text:
                attr = "EGT"
        ack = self._make_token_tab_reply(reserved_be32, []) if reserved_be32 else self._make_20922_tab_message(cmd[:4], [])
        if game is not None and attr == "EGS":
            game.set_ready(int(self.user.uid), True)
            setattr(game, "_lobby_ready_aux_phase_rank", max(1, int(getattr(game, "_lobby_ready_aux_phase_rank", 0) or 0)))
            self._send_bootstrap_bytes(ack)
            self._broadcast_game_message(
                game,
                text,
                attr,
                exclude_uid=0,
                sender_uid=int(self.user.uid),
                sender_flag="EGSU",
            )
            self._lobby_emit_settled_ready_refresh(game, reason="post-egs", delays=(0.10,))
            self._lobby_emit_ready_onln_peer_push(game, reason="post-egs", delays=(0.10,))
            return
        if game is not None and attr == "EGT":
            if "TIME%3d-1" not in text and "TIME=-1" not in text:
                if self._lobby_countdown_reset_active(game):
                    self._send_bootstrap_bytes(ack)
                    return
                setattr(game, "_lobby_ready_aux_phase_rank", max(2, int(getattr(game, "_lobby_ready_aux_phase_rank", 0) or 0)))
            self._send_bootstrap_bytes(ack)
            self._broadcast_game_message(
                game,
                text,
                attr,
                exclude_uid=0,
                sender_uid=int(self.user.uid),
                sender_flag="EGTU",
            )
            return
        frame = self._make_20922_tab_message("+msg", self._lobby_msg_fields(text, sender=self._lobby_persona(), attr=attr, flag=attr))
        self._send_bootstrap_bytes(ack)
        self._broadcast_lobby_bytes(frame)

    def _broadcast_game_message(
        self,
        game,
        text: str,
        attr: str,
        *,
        exclude_uid: int = 0,
        sender_uid: int = 0,
        sender_flag: str = "",
    ) -> None:
        for uid in self._lobby_game_participant_uids(game):
            if exclude_uid and int(uid) == int(exclude_uid):
                continue
            handler = self._lobby_handler_for_uid(uid)
            if handler:
                flag = sender_flag if sender_uid and int(uid) == int(sender_uid) and sender_flag else attr
                frame = handler._make_20922_tab_message(
                    "+msg",
                    handler._lobby_msg_fields(text, sender=self._lobby_persona(), attr=flag, flag=flag),
                )
                handler._send_bootstrap_bytes(frame)

    def _cmd_onln(self, cmd, kv, payload, reserved_be32, tail):
        game = self.srv.games.get(int(getattr(self.user, "game", 0) or 0)) if getattr(self.user, "game", 0) else None
        target = self._resolve_onln_target(game, kv)
        if target is None:
            target = self.user
        target_in_game = (
            game is not None
            and int(getattr(target, "uid", 0) or 0) in self._lobby_game_participant_uid_set(game)
        )
        target_game = game if target_in_game else None
        stale_wire_id = 0 if target_in_game or game is None else self._lobby_existing_game_wire_id_for(game, target)
        parts = []
        promoted = bool(getattr(game, "_lobby_post_gjoi_promoted", False)) if game is not None else False
        is_host = game is not None and int(getattr(self.user, "uid", 0) or 0) == int(getattr(game, "host_uid", 0) or 0)
        reentry_room_view = (
            game is not None
            and target_in_game
            and int(getattr(self, "_lobby_active_room_reentry_game_id", 0) or 0) == int(getattr(game, "id", 0) or 0)
        )
        if reentry_room_view:
            pass
        elif game is not None and target_in_game and promoted and is_host and int(target.uid) != int(self.user.uid):
            display_game = self._lobby_room_ident(game)
            parts.append(
                self._make_20922_tab_message(
                    "+who",
                    self._lobby_who_fields_for(
                        target,
                        aux_text=self._lobby_aux_for_presence(target),
                        game_active=False,
                        game_id=int(game.id),
                        display_game_id=display_game,
                        force_display_game_id=True,
                    ),
                )
            )
            parts.append(self._make_20922_tab_message("+ust", self._lobby_userset_fields(game)))
            parts.append(self._make_20922_tab_message("+usm", self._lobby_usm_fields_for_user(target, game_id=int(game.id), display_game_id=display_game)))
            parts.append(self._make_20922_tab_message("+mgm", self._lobby_game_ready_snapshot_fields(game, viewer_uid=int(self.user.uid))))
        elif game is not None and target_in_game and int(target.uid) != int(self.user.uid):
            display_game = self._lobby_room_ident(game)
            parts.append(
                self._make_20922_tab_message(
                    "+who",
                    self._lobby_who_fields_for(
                        self.user,
                        aux_text=self._lobby_aux_for_presence(self.user),
                        game_active=False,
                        game_id=int(game.id),
                        display_game_id=display_game,
                        force_display_game_id=True,
                    ),
                )
            )
            parts.append(self._make_20922_tab_message("+usm", self._lobby_usm_fields_for_user(self.user, game_id=int(game.id), display_game_id=display_game)))
        parts.append(
            self._make_20922_tab_message(
                "onln",
                self._lobby_onln_room_view_fields_for_user(target, game, viewer_uid=int(self.user.uid))
                if reentry_room_view
                else self._lobby_onln_fields_for_user(
                    target,
                    target_game,
                    viewer_uid=int(self.user.uid),
                    wire_id=stale_wire_id,
                ),
            )
        )
        self._send_bootstrap_bytes(b"".join(parts))

    def _resolve_onln_target(self, game, kv: dict[str, str]) -> Optional[User]:
        wanted = (kv.get("PERS", "") or kv.get("NAME", "")).strip().lower()
        if not wanted:
            return self.user
        if game is not None:
            for uid in self._lobby_game_participant_uids(game):
                user = self.srv.users.get(uid)
                if user is not None and wanted in {self._lobby_display_name_for(user).lower(), self._lobby_persona_for(user).lower()}:
                    return user
        return getattr(self.srv.users, "get_by_name", lambda name: None)(wanted)

    def _cmd_gsta(self, cmd, kv, payload, reserved_be32, tail):
        game = self.srv.games.get(int(getattr(self.user, "game", 0) or 0)) if getattr(self.user, "game", 0) else None
        if game is None:
            game = self._find_game_by_name_or_ident(kv.get("NAME", ""), _safe_int(kv.get("IDENT", 0), 0))
        if game is None:
            return
        if cmd == "gsta" and self._lobby_countdown_reset_active(game):
            self._send_bootstrap_bytes(self._make_20922_tab_message("gsta", []), label="gsta-ack-reset")
            return
        legacy = cmd == "gsta" and _cfg_bool(getattr(self.srv, "cfg", {}), "LOBBY_GSTA_LEGACY_FLOW", False)
        if getattr(game, "state", "") != GAME_STATE_ACTIVE:
            setattr(game, "_lobby_seed", int(time.time()) & 0xFFFFFFFF)
            game.start()
        self._lobby_mark_game_participants_in_race(game)
        if cmd == "GSTA" or reserved_be32:
            callback = [
                self._make_token_tab_reply(reserved_be32, []),
                self._make_20922_tab_message("+usr", self._lobby_usr_fields(sync=3, game_id=game.id, user=self.user, flags="G")),
            ]
            for uid in self._lobby_game_participant_uids(game):
                if uid == int(self.user.uid):
                    continue
                user = self.srv.users.get(uid)
                if user:
                    callback.append(self._make_20922_tab_message("+usr", self._lobby_usr_fields(sync=3, game_id=game.id, user=user, flags="G")))
            callback.append(self._make_20922_tab_message("+gam", self._lobby_gam_fields(game)))
            self._send_bootstrap_bytes(b"".join(callback), label="gsta-callback")
        else:
            self._send_bootstrap_bytes(self._make_20922_tab_message("gsta", []), label="gsta-ack")
        self._emit_race_start_bursts(game, legacy=legacy)

    def _emit_race_start_bursts(self, game, *, legacy: bool = False) -> None:
        include_relay = legacy and _cfg_bool(getattr(self.srv, "cfg", {}), "INCLUDE_RELAY_FIELDS", False)
        for handler in self._lobby_game_handlers(int(game.id)):
            fields = handler._lobby_game_ready_snapshot_fields(
                game,
                viewer_uid=int(handler.user.uid),
                tunnel_addrs=True,
                include_relay=include_relay,
            )
            seed = int(getattr(game, "_lobby_seed", 11572858) or 11572858)
            ses_fields = list(fields) + [f"SEED={seed}", f"SELF={handler._lobby_display_name_for(handler.user)}"]
            parts = []
            if legacy:
                parts.append(handler._make_20922_binary_message("gsta", b"\x00" * 9))
                parts.append(handler._make_20922_tab_message("+mgm", fields))
                parts.append(handler._make_20922_tab_message("+ses", ses_fields))
            else:
                parts.append(handler._make_20922_tab_message("+who", handler._lobby_who_fields_for(handler.user, game_active=True, game_id=int(game.id), flags="GU")))
                for uid in handler._lobby_game_snapshot_participant_uids(game):
                    user = handler.srv.users.get(uid)
                    if user is not None:
                        parts.append(handler._make_20922_tab_message("+usm", handler._lobby_usm_fields_for_user(user, game_id=int(game.id), active=True, flags="G")))
                parts.append(handler._make_20922_tab_message("+mgm", fields))
                parts.append(handler._make_20922_tab_message("+ses", ses_fields))
                handler._lobby_race_bootstrap_done = True
            handler._send_bootstrap_bytes(b"".join(parts), label="race-start")

    def _cmd_rank(self, cmd, kv, payload, reserved_be32, tail):
        game = self.srv.games.get(int(getattr(self.user, "game", 0) or 0)) if getattr(self.user, "game", 0) else None
        elapsed = 0
        if game is not None and getattr(game, "started_at", None):
            elapsed = int(max(0, time.time() - game.started_at))
        parts = [
            self._make_token_tab_reply(reserved_be32, [f"RANK=Unranked", f"TIME={elapsed}"]),
            self._make_20922_tab_message("+usr", self._lobby_usr_fields(sync=3, game_id=0, user=self.user)),
        ]
        if game is not None:
            parts.append(self._make_20922_tab_message("+gam", self._lobby_gam_fields(game)))
        parts.append(self._lobby_sst_frame(gip=0))
        self._send_bootstrap_bytes(b"".join(parts))

    def _cmd_leave(self, cmd, kv, payload, reserved_be32, tail):
        game = self.srv.games.get(int(getattr(self.user, "game", 0) or 0)) if getattr(self.user, "game", 0) else None
        removed = False
        if game is not None:
            _, removed = self.srv.games.leave(int(game.id), int(self.user.uid))
        self.user.game = 0
        self.user.stat = STAT_LOBBY
        self._send_bootstrap_bytes(self._make_20922_tab_message(cmd[:4], ["RESULT=OK"]))
        self._lobby_on_game_departure(game, departed_uid=int(self.user.uid), removed=removed)

    def _cmd_kick(self, cmd, kv, payload, reserved_be32, tail):
        self._send_bootstrap_bytes(self._make_20922_tab_message("KICK", ["RESULT=OK"]))

    # ------------------------------------------------------------------ #
    # Ready/countdown/presence helpers                                    #
    # ------------------------------------------------------------------ #

    def _lobby_set_aux_fields_for_user(self, user: User, fields: dict[str, object]) -> str:
        text = _aux_fields_to_text(fields)
        user.aux = text
        if int(getattr(user, "uid", 0) or 0) == int(self.user.uid):
            self._probe_aux_text = text
        return text

    def _lobby_mark_countdown_reset(self, game, reason: str = "") -> None:
        delay = _cfg_float(getattr(self.srv, "cfg", {}), "LOBBY_JOIN_COUNTDOWN_DELAY", 10.7)
        setattr(game, "_lobby_countdown_reset_until", time.time() + max(delay, 0.1))
        setattr(game, "_lobby_countdown_reset_reason", reason)

    def _lobby_countdown_reset_remaining(self, game) -> float:
        return max(0.0, float(getattr(game, "_lobby_countdown_reset_until", 0.0) or 0.0) - time.time())

    def _lobby_countdown_reset_active(self, game) -> bool:
        return self._lobby_countdown_reset_remaining(game) > 0.0

    def _lobby_room_phase_active(self, game) -> bool:
        return bool(game is not None and getattr(game, "state", "") == GAME_STATE_ACTIVE)

    def _lobby_store_aux_without_countdown(self, user: User) -> str:
        user.aux = _remove_aux_keys(getattr(user, "aux", ""), "CE")
        return user.aux

    def _lobby_preserve_ready_aux_phase_for_user(self, game, user: User) -> str:
        rank = int(getattr(game, "_lobby_ready_aux_phase_rank", 0) or 0)
        if rank >= 2:
            aux = _remove_aux_keys(getattr(user, "aux", ""), "CE", "LT")
            if aux:
                user.aux = aux
            return getattr(user, "aux", "")
        return ""

    def _lobby_emit_auxi_game_refresh_for_subject(
        self,
        game,
        subject: User,
        *,
        reason: str = "",
        delays: tuple[float, ...] = (0.02,),
        ignore_uadm_guard: bool = False,
        phase_rank: int | None = None,
    ) -> None:
        if phase_rank is None:
            phase_rank = int(getattr(game, "_lobby_ready_aux_phase_rank", 0) or 0)
        for delay in delays:
            def _send(game=game, phase_rank=phase_rank):
                if int(getattr(game, "_lobby_ready_aux_phase_rank", 0) or 0) != int(phase_rank):
                    return
                self._emit_presence_refresh(game, delays=(0.0,), include_onln=False, include_self=True, ignore_countdown=True)

            timer = threading.Timer(max(0.0, float(delay)), _send)
            timer.daemon = True
            timer.start()

    def _lobby_emit_settled_ready_refresh(self, game, *, reason: str = "", delays: tuple[float, ...] = (0.10,)) -> None:
        phase_rank = int(getattr(game, "_lobby_ready_aux_phase_rank", 0) or 0)
        for delay in delays:
            def _send(game=game, phase_rank=phase_rank):
                if int(getattr(game, "_lobby_ready_aux_phase_rank", 0) or 0) != int(phase_rank):
                    return
                self._emit_presence_refresh(game, delays=(0.0,), include_onln=False)

            timer = threading.Timer(max(0.0, float(delay)), _send)
            timer.daemon = True
            timer.start()

    def _lobby_emit_ready_onln_peer_push(self, game, *, reason: str = "", delays: tuple[float, ...] = (0.10,)) -> None:
        phase_rank = int(getattr(game, "_lobby_ready_aux_phase_rank", 0) or 0)
        for delay in delays:
            def _send(game=game, phase_rank=phase_rank):
                if int(getattr(game, "_lobby_ready_aux_phase_rank", 0) or 0) != int(phase_rank):
                    return
                self._emit_presence_refresh(game, delays=(0.0,), include_onln=True, only_onln=True)

            timer = threading.Timer(max(0.0, float(delay)), _send)
            timer.daemon = True
            timer.start()

    def _emit_active_auxi_refresh(self, game, subject: User) -> None:
        if self.srv.games.get(int(getattr(game, "id", 0) or 0)) is not game:
            return
        subject_uid = int(getattr(subject, "uid", 0) or 0)
        if subject_uid not in self._lobby_game_participant_uid_set(game):
            return
        sync = self._lobby_next_usr_sync_for(subject)
        for handler in self._lobby_game_handlers(int(game.id)):
            if int(getattr(handler.user, "uid", 0) or 0) == subject_uid:
                fields = handler._lobby_usr_fields(sync=sync, game_id=int(game.id), user=subject)
                fields.append(f"AUX={_normalize_aux_text(handler._lobby_aux_for_presence(subject))}")
                frames = handler._make_20922_tab_message("+usr", fields)
            else:
                frames = handler._make_20922_tab_message(
                    "+usm",
                    handler._lobby_usm_fields_for_user(subject, game_id=int(game.id), active=True),
                )
            handler._send_bootstrap_bytes(frames, label="active-auxi-refresh")

    def _emit_presence_refresh(
        self,
        game,
        *,
        delays: tuple[float, ...] = (0.0,),
        include_onln: bool = False,
        only_onln: bool = False,
        include_self: bool = False,
        ignore_countdown: bool = False,
    ) -> None:
        for delay in delays:
            def _send(
                game=game,
                include_onln=include_onln,
                only_onln=only_onln,
                include_self=include_self,
                ignore_countdown=ignore_countdown,
            ):
                if self.srv.games.get(int(getattr(game, "id", 0) or 0)) is not game:
                    return
                participants = self._lobby_game_participant_uids(game)
                for viewer_uid in participants:
                    handler = self._lobby_handler_for_uid(viewer_uid)
                    if handler is None:
                        continue
                    frames: list[bytes] = []
                    for uid in participants:
                        if not include_self and uid == viewer_uid and len(participants) > 1:
                            continue
                        user = self.srv.users.get(uid)
                        if user is None:
                            continue
                        if not only_onln:
                            aux_text = handler._lobby_aux_for(user) if ignore_countdown else handler._lobby_aux_for_presence(user)
                            display_game = handler._lobby_room_ident(game)
                            frames.append(
                                handler._make_20922_tab_message(
                                    "+who",
                                    handler._lobby_who_fields_for(
                                        user,
                                        aux_text=aux_text,
                                        game_active=False,
                                        game_id=int(game.id),
                                        display_game_id=display_game,
                                        force_display_game_id=True,
                                    ),
                                )
                            )
                            usm_fields = handler._lobby_usm_fields_for_user(user, game_id=int(game.id), display_game_id=display_game)
                            if ignore_countdown:
                                usm_fields = [field for field in usm_fields if not field.startswith("AUX=") and not field.startswith("X=")]
                                aux = _normalize_aux_text(aux_text)
                                usm_fields.append(f"X={aux}")
                            frames.append(handler._make_20922_tab_message("+usm", usm_fields))
                        if include_onln:
                            reentered = getattr(game, "_lobby_room_reentry_uids", set()) or set()
                            if (
                                int(getattr(handler, "_lobby_active_room_reentry_game_id", 0) or 0) == int(getattr(game, "id", 0) or 0)
                                or int(uid) in {int(value) for value in reentered}
                            ):
                                onln_fields = handler._lobby_onln_room_view_fields_for_user(user, game, viewer_uid=viewer_uid)
                            else:
                                onln_fields = handler._lobby_onln_fields_for_user(user, game, viewer_uid=viewer_uid)
                            frames.append(handler._make_20922_tab_message("onln", onln_fields))
                    if frames:
                        handler._send_bootstrap_bytes(b"".join(frames), label="presence-refresh")

            if delay <= 0:
                _send()
            else:
                timer = threading.Timer(float(delay), _send)
                timer.daemon = True
                timer.start()

    # ------------------------------------------------------------------ #
    # Race/session helpers                                                #
    # ------------------------------------------------------------------ #

    def _lobby_mark_game_participants_in_race(self, game) -> None:
        for uid in self._lobby_game_participant_uids(game):
            user = self.srv.users.get(uid)
            if user is not None:
                user.game = int(game.id)
                user.stat = STAT_GAME
            handler = self._lobby_handler_for_uid(uid)
            if handler is not None:
                handler._lobby_active_room_reentry_game_id = 0
        setattr(game, "_lobby_room_reentry_uids", set())

    def _race_endpoint_for(self, user: User) -> tuple[str, int]:
        endpoint = getattr(self.srv, "race_udp_endpoint_for", None)
        if callable(endpoint):
            host, port = endpoint(conn=getattr(user, "conn", None), name=self._lobby_display_name_for(user), persona=self._lobby_persona_for(user))
            return str(host or ""), int(port or 0)
        advertised = getattr(self.srv, "advertised_game_endpoint_for", None)
        if callable(advertised):
            host, port = advertised(conn=getattr(user, "conn", None), uid=int(user.uid), name=user.name, persona=user.pers)
            return str(host or ""), int(port or 0)
        return getattr(user, "ip", "127.0.0.1"), 3658

    def _lobby_race_addr_fields(self, game, *, include_relay: bool = False) -> list[str]:
        participants = self._lobby_game_snapshot_participant_uids(game)
        users = [self.srv.users.get(uid) for uid in participants]
        users = [user for user in users if user is not None]
        addrs = self._virtualized_race_addrs(users)
        fields: list[str] = []
        for idx, user in enumerate(users):
            host = addrs.get(int(user.uid), getattr(user, "addr", "") or getattr(user, "ip", "127.0.0.1"))
            lhost = addrs.get(int(user.uid), _normalize_peer_laddr(host, getattr(user, "laddr", "") or host))
            _, port = self._race_endpoint_for(user)
            fields.extend([f"ADDR{idx}={host}", f"LADDR{idx}={lhost}", f"PORT{idx}={port or 3658}"])
        if include_relay:
            host, port = self._race_endpoint_for(self.user)
            fields.extend([f"RLYHOST={host}", f"RLYPORT={port or 3658}"])
        return fields

    def _participant_addr_fields(self, game, *, active: bool = False) -> dict[int, tuple[str, str, int]]:
        participants = self._lobby_game_snapshot_participant_uids(game)
        users = [self.srv.users.get(uid) for uid in participants]
        users = [user for user in users if user is not None]
        virtual = self._virtualized_race_addrs(users)
        out: dict[int, tuple[str, str, int]] = {}
        for user in users:
            uid = int(getattr(user, "uid", 0) or 0)
            host = virtual.get(uid) or str(getattr(user, "addr", "") or getattr(user, "ip", "") or getattr(game, "addr", "") or "127.0.0.1")
            laddr = virtual.get(uid) or _normalize_peer_laddr(host, getattr(user, "laddr", "") or host)
            _adv_host, port = self._race_endpoint_for(user)
            out[uid] = (host, laddr, int(port or getattr(game, "port", 0) or 3658))
        return out

    def _virtualized_race_addrs(self, users: list[User]) -> dict[int, str]:
        cfg = getattr(self.srv, "cfg", {})
        mode = str(getattr(cfg, "get", lambda k, d=None: d)("RACE_VIRTUAL_PEER_MODE", "off") or "off").strip().lower()
        if mode not in ("auto", "on", "1", "true"):
            return {}
        raw_addrs = [str(getattr(user, "addr", "") or getattr(user, "ip", "") or "") for user in users]
        if mode == "auto" and len(set(raw_addrs)) == len(raw_addrs):
            return {}
        subnet_text = str(getattr(cfg, "get", lambda k, d=None: d)("RACE_VIRTUAL_PEER_SUBNET", "100.64.0.0/24") or "100.64.0.0/24")
        try:
            hosts = list(ipaddress.ip_network(subnet_text, strict=False).hosts())
        except ValueError:
            hosts = list(ipaddress.ip_network("100.64.0.0/24").hosts())
        if len(hosts) < len(users):
            hosts = list(ipaddress.ip_network("100.64.0.0/24").hosts())
        out: dict[int, str] = {}
        for idx, user in enumerate(users):
            if idx < len(hosts):
                out[int(user.uid)] = str(hosts[idx])
        return out

    def _virtual_addr_for_user(self, game, user: User | None) -> str:
        if game is None or user is None:
            return ""
        participants = self._lobby_game_snapshot_participant_uids(game)
        users = [self.srv.users.get(uid) for uid in participants]
        users = [participant for participant in users if participant is not None]
        return self._virtualized_race_addrs(users).get(int(getattr(user, "uid", 0) or 0), "")

    # ------------------------------------------------------------------ #
    # Admin/control compatibility                                         #
    # ------------------------------------------------------------------ #

    def _lobby_news_burst(self) -> bytes:
        mode = str(getattr(self.srv, "cfg", {}).get("LOBBY_NEWS_MODE", "captured") or "captured").strip().lower()
        if mode in {"legacy", "png", "~png"}:
            probe_ref = self._format_probe_ref()
            self._probe_last_ref = probe_ref
            return b"".join(
                [
                    self._make_20922_tab_message("~png", [f"REF={probe_ref}"]),
                    self._make_20922_tab_message("skey", [f"SKEY={LOBBY_SERVER_SKEY}"]),
                    make_short_frame("newsbadc"),
                ]
            )

        conn = getattr(self.user, "conn", None)
        news_host = str(
            getattr(self.srv, "cfg", {}).get("LOBBY_NEWS_HOST", "")
            or getattr(self.srv, "control_host", lambda _conn=None: "")(conn)
            or self._lobby_server_addr()
            or "127.0.0.1"
        ).strip()
        try:
            buddy_port = int(getattr(self.srv, "cfg", {}).get("LOBBY_NEWS_BUDDY_PORT", 0) or 0)
        except (TypeError, ValueError):
            buddy_port = 0
        if buddy_port <= 0:
            try:
                buddy_port = int(getattr(self.srv, "control_port", lambda: 20923)() or 20923)
            except Exception:
                buddy_port = 20923
        try:
            http_port = int(getattr(self.srv, "cfg", {}).get("LOBBY_NEWS_HTTP_PORT", 0) or 0)
        except (TypeError, ValueError):
            http_port = 0
        if http_port <= 0:
            http_port = buddy_port
        http_host = news_host if http_port in (0, 80) else f"{news_host}:{http_port}"
        lines = [
            f"TOSA_URL=http://{http_host}/tos",
            f"TOSAC_URL=http://{http_host}/tos",
            "CIRCUIT_TIER_POINTS=0,1999,4999,9999,19999,39999,59999,79999,99999,119999",
            "DRAG_TIER_POINTS=0,1999,4999,9999,19999,39999,59999,79999,99999,119999",
            f"BUDDY_SERVER={news_host}",
            f"BUDDY_PORT={buddy_port}",
            "STREET_CROSS_TIER_POINTS=0,1999,4999,9999,19999,39999,59999,79999,99999,119999",
            f"NEWS_URL=http://{http_host}/news",
            "SPRINT_TIER_POINTS=0,1999,4999,9999,19999,39999,59999,79999,99999",
            "DRIFT_TIER_POINTS=0,1999,4999,9999,19999,39999,59999,79999,99999,119999",
        ]
        payload = ("\n".join(lines) + "\n").encode("utf-8") + b"\x00"
        return self._make_20922_signed_binary_message("news", payload, 567, reserved_be32=0x6E657737)

    def _make_dir_reply(self) -> bytes:
        ip = getattr(self.srv, "lobby_tcp_host", lambda conn=None: "127.0.0.1")(getattr(self.user, "conn", None))
        port = getattr(self.srv, "lobby_tcp_port", lambda: 9900)()
        fields = [
            f"ADDR={ip}",
            f"PORT={port}",
            f"SESS={DIR_SESS}",
            f"MASK={DIR_MASK}",
            "TIME=0",
        ]
        remember = getattr(self.srv, "remember_lobby_dir_challenge", None)
        if callable(remember):
            remember(getattr(self.user, "ip", ""), DIR_SESS, DIR_MASK)
        return self._make_20922_tab_message("@dir", fields)

    def _lobby_deliver_invite(self, target: str, text: str) -> int:
        target_key = str(target or "").strip().lower()
        delivered = 0
        for handler in self._snapshot_lobby_handlers():
            names = {handler._lobby_display_name().lower(), handler._lobby_persona().lower()}
            if target_key in names:
                handler._send_later_bytes(
                    0.01,
                    handler._make_20922_tab_message("+msg", handler._lobby_msg_fields(text, sender=self._lobby_persona(), flag="P")),
                    label="invite",
                )
                delivered += 1
        return delivered

    def _lobby_emit_game_leave_reset(self, handler: "ClientHandler", game, *, delay_s: float = 0.0, self_leave: bool = False) -> None:
        data = handler._make_20922_tab_message("+usr", handler._lobby_usr_fields(sync=3, game_id=0, user=handler.user)) + handler._lobby_sst_frame()
        handler._send_later_bytes(delay_s, data, label="leave-reset")

    def _lobby_usm_clear_fields(self, user: Optional[User], wire_id: int) -> list[str]:
        if user is None:
            return [f"I={int(wire_id)}", "S=0"]
        return [
            f"I={int(wire_id)}",
            f"N={self._lobby_display_name_for(user)}",
            "F=",
            "G=0",
            f"X={_normalize_aux_text(self._lobby_aux_for_presence(user))}",
            "S=0",
        ]

    def _lobby_on_game_departure(self, game, *, departed_uid: int = 0, removed: bool = False) -> None:
        if game is None:
            return
        if removed:
            game_id = int(getattr(game, "id", 0) or 0)
            display_game = self._lobby_room_ident(game)
            participants = []
            for uid in list(getattr(game, "participants", []) or []):
                uid = int(uid)
                if departed_uid and uid == int(departed_uid):
                    continue
                if uid not in participants:
                    participants.append(uid)
            if not participants:
                return
            reset_uids = []
            if departed_uid:
                reset_uids.append(int(departed_uid))
            for uid in participants:
                if int(uid) not in reset_uids:
                    reset_uids.append(int(uid))
            wire_ids = {}
            for uid in reset_uids:
                user = self.srv.users.get(uid)
                if user is not None:
                    wire_ids[int(uid)] = self._lobby_wire_id_for(user)
            for uid in participants:
                user = self.srv.users.get(uid)
                if user is not None:
                    user.game = 0
                    user.stat = STAT_LOBBY
            departed_user = self.srv.users.get(int(departed_uid)) if departed_uid else None
            if departed_user is not None:
                departed_user.game = 0
                departed_user.stat = STAT_LOBBY
            for uid in participants:
                handler = self._lobby_handler_for_uid(uid)
                if handler is None:
                    continue
                frames: list[bytes] = []
                for reset_uid, wire_id in sorted(wire_ids.items(), key=lambda item: int(item[1])):
                    user = self.srv.users.get(int(reset_uid))
                    if user is not None:
                        frames.append(
                            handler._make_20922_tab_message(
                                "+usr",
                                handler._lobby_usr_fields(sync=3, game_id=0, user=user, wire_id=int(wire_id)),
                            )
                        )
                frames.append(handler._make_20922_tab_message("+gam", [f"IDENT={display_game}"]))
                for clear_uid, wire_id in sorted(wire_ids.items(), key=lambda item: int(item[1])):
                    user = self.srv.users.get(int(clear_uid))
                    frames.append(handler._make_20922_tab_message("+usm", handler._lobby_usm_clear_fields(user, int(wire_id))))
                frames.append(
                    handler._make_20922_tab_message(
                        "+who",
                        handler._lobby_who_fields_for(
                            handler.user,
                            game_active=False,
                            wire_id=int(wire_ids.get(int(uid), 0) or 0),
                        ),
                    )
                )
                frames.append(handler._make_20922_tab_message("+ust", [f"I={display_game}"]))
                frames.append(handler._make_20922_tab_message("+mgm", [f"IDENT={display_game}"]))
                frames.append(handler._lobby_sst_frame())
                handler._send_later_bytes(
                    0.01,
                    b"".join(frames),
                    label="departure-removed",
                    should_send=lambda handler=handler: bool(getattr(handler.user, "connected", False)),
                )
            return
        departed_user = self.srv.users.get(int(departed_uid)) if departed_uid else None
        departed_wire_id = (
            self._lobby_wire_id_for(departed_user)
            if departed_user is not None
            else 0
        )
        game_id = int(getattr(game, "id", 0) or 0)
        for uid in self._lobby_game_participant_uids(game):
            if departed_uid and int(uid) == int(departed_uid):
                continue
            handler = self._lobby_handler_for_uid(uid)
            if handler is None:
                continue
            frames: list[bytes] = []
            if departed_wire_id:
                frames.append(handler._make_20922_tab_message("+usm", handler._lobby_usm_clear_fields(departed_user, departed_wire_id)))
            frames.append(handler._make_20922_tab_message("+mgm", handler._lobby_game_ready_snapshot_fields(game, viewer_uid=uid)))
            frames.append(handler._lobby_sst_frame())
            handler._send_later_bytes(
                0.01,
                b"".join(frames),
                label="departure",
                should_send=lambda handler=handler, game_id=game_id: int(getattr(handler.user, "game", 0) or 0) == int(game_id),
            )
            if departed_wire_id:
                handler._send_later_bytes(
                    0.12,
                    handler._make_20922_tab_message("+usm", [f"I={int(departed_wire_id)}", "S=0"]),
                    label="departure-clear",
                    should_send=lambda handler=handler, game_id=game_id: int(getattr(handler.user, "game", 0) or 0) == int(game_id),
                )

    def _cmd_server_stat(self, fields: dict) -> None:
        active_games = len(self.srv.games.active_games()) if hasattr(self.srv.games, "active_games") else 0
        active_users = self.srv.games.active_participant_count() if hasattr(self.srv.games, "active_participant_count") else 0
        payload = f"usersInGames={active_users}\ngamesInProgress={active_games}\ntime={format_lobby_time()}\n"
        self.user.send_bytes(payload.encode("utf-8"))

    def _cleanup_on_close(self) -> None:
        with ClientHandler._lobby_handlers_lock:
            ClientHandler._lobby_handlers.discard(self)
        self.user.connected = False
        game = None
        detached = False
        try:
            game_id = int(getattr(self.user, "game", 0) or 0)
            if game_id and hasattr(self.srv, "games"):
                game = self.srv.games.get(game_id)
                if (
                    game is not None
                    and _cfg_bool(getattr(self.srv, "cfg", {}), "LOBBY_DETACH_ON_PEER_CLOSE", True)
                ):
                    self.user.race_detached_at = time.time()
                    detached = True
                else:
                    game_after, removed = self.srv.games.leave(game_id, int(self.user.uid))
                    self.user.game = 0
                    self.user.stat = STAT_LOBBY
                    self._lobby_on_game_departure(game or game_after, departed_uid=int(self.user.uid), removed=removed)
        except Exception:
            pass
        if not detached:
            try:
                room_id = int(getattr(self.user, "room", 0) or 0)
                if room_id and hasattr(self.srv, "rooms"):
                    self.srv.rooms.leave(room_id, int(self.user.uid))
                    self.user.room = 0
            except Exception:
                pass
            try:
                if self._registered and hasattr(self.srv.users, "remove"):
                    self.srv.users.remove(int(self.user.uid))
            except Exception:
                pass
        try:
            refresh = getattr(self.srv, "request_master_stat_refresh", None)
            if callable(refresh):
                refresh()
        except Exception:
            pass
        try:
            self.user.conn.close()
        except Exception:
            pass
