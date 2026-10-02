"""The board health report, telemetry wire format v1 -- the deck's twin of
TelemetryCodec.h.

The gateway decodes what boards send; the encoder is here so the shared fixture,
tests/fixtures/telemetry_v1.json, can be generated and checked on both sides.
The layout and its reasons are in TelemetryCodec.h; this file follows it field
for field. Big-endian throughout; counts saturate; heap figures are whole KB.
"""

import struct

WIRE_VERSION = 1
WIRE_BASE_LEN = 29
WIRE_MAX_LEN = 34

FLAG_RELAYING = 0x01
FLAG_RELAY_EXPECTED = 0x02
FLAG_TIME_CURRENT = 0x04
FLAG_PSRAM = 0x08
FLAG_BATTERY = 0x10

IF_LORA = 0x01
IF_BLE = 0x02
IF_WIFI = 0x04
IF_ESPNOW = 0x08
IF_HALOW = 0x10

RESET_NAMES = {
    0: "unknown", 1: "poweron", 2: "software", 3: "panic", 4: "task_wdt",
    5: "int_wdt", 6: "brownout", 7: "external", 8: "other",
}

_BASE = ">BBIIBHHHHHBBBBHH"   # 29 bytes


class Telemetry:
    """Mirrors NodeTelemetry. Byte counts in, whole KB on the wire."""

    __slots__ = ("sender_id", "uptime_s", "reset", "boots", "crashes", "panics",
                 "heap_bytes", "largest_bytes", "psram_known", "psram_bytes",
                 "if_present", "if_up", "ble_peers", "espnow_peers", "paths", "nodes",
                 "relaying", "relay_expected", "time_current",
                 "battery_known", "battery_mv", "battery_pct")

    def __init__(self, **fields):
        for name in self.__slots__:
            setattr(self, name, fields.pop(name, False if name.endswith(("_known", "relaying", "relay_expected", "time_current")) else 0))
        if fields:
            raise TypeError("unknown fields: %s" % ", ".join(sorted(fields)))


def _sat16(value):
    return min(value, 0xFFFF)


def _sat8(value):
    return min(value, 0xFF)


def encode(t):
    flags = 0
    if t.relaying:
        flags |= FLAG_RELAYING
    if t.relay_expected:
        flags |= FLAG_RELAY_EXPECTED
    if t.time_current:
        flags |= FLAG_TIME_CURRENT
    if t.psram_known:
        flags |= FLAG_PSRAM
    if t.battery_known:
        flags |= FLAG_BATTERY
    out = struct.pack(_BASE, WIRE_VERSION, flags, t.sender_id, t.uptime_s, t.reset,
                      _sat16(t.boots), _sat16(t.crashes), _sat16(t.panics),
                      _sat16(t.heap_bytes // 1024), _sat16(t.largest_bytes // 1024),
                      t.if_present, t.if_up, _sat8(t.ble_peers), _sat8(t.espnow_peers),
                      _sat16(t.paths), _sat16(t.nodes))
    if flags & FLAG_PSRAM:
        out += struct.pack(">H", _sat16(t.psram_bytes // 1024))
    if flags & FLAG_BATTERY:
        out += struct.pack(">HB", t.battery_mv, min(t.battery_pct, 100))
    return out


def decode(data):
    """A Telemetry, or None for anything this version cannot read. Bytes past
    the announced fields are ignored, so a later version can append."""
    if data is None or len(data) < WIRE_BASE_LEN or data[0] != WIRE_VERSION:
        return None
    (_, flags, sender_id, uptime_s, reset, boots, crashes, panics, heap_kb, largest_kb,
     if_present, if_up, ble_peers, espnow_peers, paths, nodes) = struct.unpack(_BASE, data[:WIRE_BASE_LEN])
    t = Telemetry(sender_id=sender_id, uptime_s=uptime_s, reset=reset, boots=boots,
                  crashes=crashes, panics=panics, heap_bytes=heap_kb * 1024,
                  largest_bytes=largest_kb * 1024, if_present=if_present, if_up=if_up,
                  ble_peers=ble_peers, espnow_peers=espnow_peers, paths=paths, nodes=nodes,
                  relaying=bool(flags & FLAG_RELAYING),
                  relay_expected=bool(flags & FLAG_RELAY_EXPECTED),
                  time_current=bool(flags & FLAG_TIME_CURRENT))
    at = WIRE_BASE_LEN
    if flags & FLAG_PSRAM:
        if at + 2 > len(data):
            return None
        t.psram_known = True
        t.psram_bytes = struct.unpack(">H", data[at:at + 2])[0] * 1024
        at += 2
    if flags & FLAG_BATTERY:
        if at + 3 > len(data):
            return None
        t.battery_known = True
        t.battery_mv, t.battery_pct = struct.unpack(">HB", data[at:at + 3])
        at += 3
    return t
