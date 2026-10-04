"""The board detail report, telemetry wire format 0x21 -- the deck's twin of
TelemetryDetailCodec.h.

The gateway decodes what boards send; the encoder is here so the shared fixture,
tests/fixtures/telemetry_detail_v1.json, can be generated and checked on both
sides. The layout and its reasons are in TelemetryDetailCodec.h; this file
follows it field for field. Big-endian; counts saturate; store size in whole
KB, ages in whole minutes.
"""

import struct

WIRE_VERSION = 0x21
WIRE_MAX_LEN = 380
ENV_MAX = 32
MAX_INTERFACES = 12
MAX_NEIGHBOURS = 48

FLAG_RADIO = 0x01
FLAG_PROPAGATION = 0x02
FLAG_NEIGHBOURS_TRUNCATED = 0x04

IF_NAMES = {0: "other", 1: "lora", 2: "ble_peer", 3: "espnow", 4: "tcp_server",
            5: "tcp_client", 6: "udp", 7: "auto", 8: "serial", 9: "halow"}
RSSI_UNKNOWN = -128


def _sat16(v):
    return 0xFFFF if v > 0xFFFF else v


def _sat8(v):
    return 0xFF if v > 0xFF else v


def _pct(v):
    return 100 if v > 100 else v


def new_detail(**fields):
    """A detail report with every field at its default, then `fields`."""
    d = {
        "sender_id": 0, "uptime_s": 0, "fw_hash": "00000000", "fw_version": 0, "env": "",
        "interfaces": [],
        "radio_known": False, "rssi": RSSI_UNKNOWN, "snr_q": 0, "noise": RSSI_UNKNOWN,
        "utilisation_pct": 0, "airtime_pct": 0,
        "propagation_known": False, "store_messages": 0, "store_bytes": 0, "pn_peers": 0,
        "sync_ok": 0, "sync_fail": 0, "last_sync_s": None,
        "neighbours": [], "neighbours_truncated": False,
    }
    d.update(fields)
    return d


def encode(d, out_len=WIRE_MAX_LEN):
    """The report's bytes, or None when even the fixed part does not fit."""
    limit = min(out_len, WIRE_MAX_LEN)
    env = d["env"].encode("utf-8")[:ENV_MAX]
    ifs = d["interfaces"][:MAX_INTERFACES]
    fixed = 17 + len(env) + 1 + len(ifs) * 6 + (5 if d["radio_known"] else 0) + \
        (11 if d["propagation_known"] else 0) + 1
    if fixed > limit:
        return None
    wanted = d["neighbours"][:MAX_NEIGHBOURS]
    nbs = len(wanted)
    while nbs > 0 and fixed + nbs * 7 > limit:
        nbs -= 1
    flags = 0
    if d["radio_known"]:
        flags |= FLAG_RADIO
    if d["propagation_known"]:
        flags |= FLAG_PROPAGATION
    if nbs < len(wanted) or d["neighbours_truncated"]:
        flags |= FLAG_NEIGHBOURS_TRUNCATED

    out = bytearray(struct.pack(">BBII", WIRE_VERSION, flags, d["sender_id"], d["uptime_s"]))
    out += bytes.fromhex(d["fw_hash"])[:4].ljust(4, b"\0")
    out += struct.pack(">HB", d["fw_version"], len(env)) + env
    out.append(len(ifs))
    for f in ifs:
        out += struct.pack(">BBHH", f["kind"], 1 if f["up"] else 0,
                           _sat16(f["rx_packets"]), _sat16(f["tx_packets"]))
    if d["radio_known"]:
        out += struct.pack(">bbbBB", d["rssi"], d["snr_q"], d["noise"],
                           _pct(d["utilisation_pct"]), _pct(d["airtime_pct"]))
    if d["propagation_known"]:
        never = d["last_sync_s"] is None
        minutes = 0xFFFF if never else min(d["last_sync_s"] // 60, 0xFFFE)
        out += struct.pack(">HHBHHH", _sat16(d["store_messages"]), _sat16(d["store_bytes"] // 1024),
                           d["pn_peers"], _sat16(d["sync_ok"]), _sat16(d["sync_fail"]), minutes)
    out.append(nbs)
    for n in wanted[:nbs]:
        out += struct.pack(">IBbB", n["id"], n["kind"], n["rssi"], _sat8(n["heard_s"] // 60))
    return bytes(out)


def decode(data):
    """The report as a dict (new_detail's shape), or None if malformed."""
    if len(data) < 18 or data[0] != WIRE_VERSION:
        return None
    flags = data[1]
    sender_id, uptime_s = struct.unpack_from(">II", data, 2)
    d = new_detail(sender_id=sender_id, uptime_s=uptime_s, fw_hash=data[10:14].hex(),
                   fw_version=struct.unpack_from(">H", data, 14)[0])
    at = 16
    env_len = data[at]
    at += 1
    if env_len > ENV_MAX or at + env_len + 1 > len(data):
        return None
    try:
        d["env"] = data[at:at + env_len].decode("utf-8")
    except UnicodeDecodeError:
        return None
    at += env_len
    count = data[at]
    at += 1
    if count > MAX_INTERFACES or at + count * 6 > len(data):
        return None
    for _ in range(count):
        kind, state, rx, tx = struct.unpack_from(">BBHH", data, at)
        d["interfaces"].append({"kind": kind, "up": bool(state & 1), "rx_packets": rx, "tx_packets": tx})
        at += 6
    if flags & FLAG_RADIO:
        if at + 5 > len(data):
            return None
        d["radio_known"] = True
        d["rssi"], d["snr_q"], d["noise"], d["utilisation_pct"], d["airtime_pct"] = \
            struct.unpack_from(">bbbBB", data, at)
        at += 5
    if flags & FLAG_PROPAGATION:
        if at + 11 > len(data):
            return None
        msgs, kb, peers, ok, fail, minutes = struct.unpack_from(">HHBHHH", data, at)
        d.update(propagation_known=True, store_messages=msgs, store_bytes=kb * 1024, pn_peers=peers,
                 sync_ok=ok, sync_fail=fail, last_sync_s=None if minutes == 0xFFFF else minutes * 60)
        at += 11
    if at + 1 > len(data):
        return None
    count = data[at]
    at += 1
    if count > MAX_NEIGHBOURS or at + count * 7 > len(data):
        return None
    for _ in range(count):
        nid, kind, rssi, minutes = struct.unpack_from(">IBbB", data, at)
        d["neighbours"].append({"id": nid, "kind": kind, "rssi": rssi, "heard_s": minutes * 60})
        at += 7
    d["neighbours_truncated"] = bool(flags & FLAG_NEIGHBOURS_TRUNCATED)
    return d
