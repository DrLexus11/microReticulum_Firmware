"""A batch of telemetry reports, wire format 0x31 -- the deck's twin of
TelemetryBatchCodec.h (T2, LXMF reach).

A board that cannot reach a gateway keeps its health (0x01) and detail (0x21)
reports and sends them, about hourly, as one LXMF message through its own
propagation store. The gateway decodes the batch here and each report with the
codec it already has. The layout and its reasons are in TelemetryBatchCodec.h;
this file follows it field for field, pinned by
tests/fixtures/telemetry_batch_v1.json. Big-endian.
"""

import struct

WIRE_VERSION = 0x31
MAX_LEN = 2048
HEADER_LEN = 11
ENTRY_HEADER = 7
ENTRY_MAX = 380
MAX_ENTRIES = 255

FLAG_TRUNCATED = 0x01

TIME_RELATIVE = 0x00   # seconds before composed (the board's clock was not set)
TIME_ABSOLUTE = 0x01   # unix seconds


def _entry_valid(e):
    return e["time_kind"] in (TIME_RELATIVE, TIME_ABSOLUTE) and 0 < len(e["report"]) <= ENTRY_MAX


def encode(sender_id, composed_unix, entries, truncated=False, out_len=MAX_LEN):
    """Entries oldest first, each {"time_kind", "time", "report": bytes}. The
    oldest are left out when they do not all fit. None when an entry is invalid
    or not even the header fits."""
    limit = min(out_len, MAX_LEN)
    if limit < HEADER_LEN or not all(_entry_valid(e) for e in entries):
        return None
    first, total = len(entries), HEADER_LEN
    while first > 0 and len(entries) - first < MAX_ENTRIES:
        need = ENTRY_HEADER + len(entries[first - 1]["report"])
        if total + need > limit:
            break
        total += need
        first -= 1
    flags = FLAG_TRUNCATED if (truncated or first > 0) else 0
    out = bytearray(struct.pack(">BBIIB", WIRE_VERSION, flags, sender_id, composed_unix,
                                len(entries) - first))
    for e in entries[first:]:
        out += struct.pack(">BIH", e["time_kind"], e["time"], len(e["report"])) + bytes(e["report"])
    return bytes(out)


def decode(data):
    """{"flags", "truncated", "sender_id", "composed_unix", "entries": [...]},
    entries as encode() takes them; None if malformed."""
    if data is None or not HEADER_LEN <= len(data) <= MAX_LEN or data[0] != WIRE_VERSION:
        return None
    _, flags, sender_id, composed_unix, count = struct.unpack_from(">BBIIB", data, 0)
    at, entries = HEADER_LEN, []
    for _ in range(count):
        if at + ENTRY_HEADER > len(data):
            return None
        kind, t, n = struct.unpack_from(">BIH", data, at)
        if kind not in (TIME_RELATIVE, TIME_ABSOLUTE) or not 0 < n <= ENTRY_MAX:
            return None
        at += ENTRY_HEADER
        if at + n > len(data):
            return None
        entries.append({"time_kind": kind, "time": t, "report": bytes(data[at:at + n])})
        at += n
    if at != len(data):
        return None
    return {"flags": flags, "truncated": bool(flags & FLAG_TRUNCATED), "sender_id": sender_id,
            "composed_unix": composed_unix, "entries": entries}


def entry_times(batch, received_at):
    """Each entry's unix time. Absolute times are the board's; relative ones are
    counted back from when the batch was composed -- or, when the board's clock
    was not set, from `received_at`, which is late by however long the batch
    waited in a propagation store. The second value says which: True = exact."""
    out = []
    for e in batch["entries"]:
        if e["time_kind"] == TIME_ABSOLUTE:
            out.append((e["time"], True))
        elif batch["composed_unix"]:
            out.append((batch["composed_unix"] - e["time"], True))
        else:
            out.append((received_at - e["time"], False))
    return out
