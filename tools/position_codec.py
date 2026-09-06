#!/usr/bin/env python3
"""The compact position wire format, in Python.

The firmware encodes it in `PositionReport.h`; this decodes it. Two
implementations of one format is how a wire format goes wrong in the field
rather than on the bench, so `tests/test_position.py` cross-checks the
constants here against the header and round-trips a fix through both.

This is also the front half of the CoT gateway (TAKCapability.md §7 step 4):
what arrives over Reticulum is these bytes, and what ATAK wants is XML built
from them.

    off  size  field
    0    1     version
    1    1     flags
    2    4     lat_e7       int32 BE   degrees x 1e7, positive north
    6    4     lon_e7       int32 BE   degrees x 1e7, positive east
    10   4     fix_unix_s   uint32 BE  seconds; 0 = the source had no clock
    14   1     accuracy_m   uint8      0 unreported, 1..254 m, 255 = over
    -- then, in flag order, only what is present --
    +2         alt_m        int16 BE   metres HAE          FLAG_ALT
    +1         course       uint8      2-degree units      FLAG_COURSE
    +1         speed        uint8      half-metre/s units  FLAG_SPEED
    +1         sats         uint8                          FLAG_SATS

Fifteen bytes minimum, twenty full, against roughly seven hundred for the CoT
XML that comes out of the gateway.
"""

import struct

WIRE_VERSION = 1
WIRE_BASE_LEN = 15
WIRE_MAX_LEN = 20

FLAG_ALT = 0x01
FLAG_COURSE = 0x02
FLAG_SPEED = 0x04
FLAG_SATS = 0x08


class PositionFix:
    """Mirrors NodePositionFix, minus the fields that never go on the wire."""

    __slots__ = ("lat_e7", "lon_e7", "fix_unix_s", "accuracy_m", "alt_known",
                 "alt_m", "course_known", "course_ddeg", "speed_cms", "sats")

    def __init__(self, lat_e7=0, lon_e7=0, fix_unix_s=0, accuracy_m=0,
                 alt_known=False, alt_m=0, course_known=False, course_ddeg=0,
                 speed_cms=0, sats=0):
        self.lat_e7 = lat_e7
        self.lon_e7 = lon_e7
        self.fix_unix_s = fix_unix_s
        self.accuracy_m = accuracy_m
        self.alt_known = alt_known
        self.alt_m = alt_m
        self.course_known = course_known
        self.course_ddeg = course_ddeg
        self.speed_cms = speed_cms
        self.sats = sats

    @property
    def lat(self):
        return self.lat_e7 / 1e7

    @property
    def lon(self):
        return self.lon_e7 / 1e7

    def __repr__(self):
        return ("PositionFix(lat=%.7f, lon=%.7f, alt=%s, acc=%s, t=%d)"
                % (self.lat, self.lon,
                   self.alt_m if self.alt_known else None,
                   self.accuracy_m or None, self.fix_unix_s))


def encode(fix):
    """Pack a fix. Present only so the tests can round-trip; the firmware is
    the encoder that matters."""
    flags = 0
    if fix.alt_known:
        flags |= FLAG_ALT
    if fix.course_known:
        flags |= FLAG_COURSE
    if fix.speed_cms > 0:
        flags |= FLAG_SPEED
    if fix.sats > 0:
        flags |= FLAG_SATS

    # Saturate rather than wrap, matching the firmware: 400 m of error
    # arriving as 144 is a marker an operator trusts far more than it deserves.
    accuracy = 0 if fix.accuracy_m == 0 else min(fix.accuracy_m, 255)
    if fix.accuracy_m > 254:
        accuracy = 255

    out = struct.pack(">BBiiIB", WIRE_VERSION, flags, fix.lat_e7, fix.lon_e7,
                      fix.fix_unix_s, accuracy)
    if flags & FLAG_ALT:
        out += struct.pack(">h", fix.alt_m)
    if flags & FLAG_COURSE:
        # Normalised before scaling, as the other two implementations do: a
        # full turn is due north and would otherwise decode as due south.
        out += struct.pack(">B", (fix.course_ddeg % 3600) // 20)
    if flags & FLAG_SPEED:
        out += struct.pack(">B", min(fix.speed_cms // 50, 255))
    if flags & FLAG_SATS:
        out += struct.pack(">B", fix.sats)
    return out


def decode(data):
    """Unpack a report. Returns None for anything it does not recognise rather
    than raising: this parses bytes off a radio, and a malformed frame is a
    thing that happens, not an exception."""
    if data is None or len(data) < WIRE_BASE_LEN:
        return None
    version, flags, lat_e7, lon_e7, secs, accuracy = struct.unpack(
        ">BBiiIB", data[:WIRE_BASE_LEN])
    if version != WIRE_VERSION:
        return None

    fix = PositionFix(lat_e7=lat_e7, lon_e7=lon_e7, fix_unix_s=secs,
                      accuracy_m=accuracy)
    at = WIRE_BASE_LEN
    if flags & FLAG_ALT:
        if at + 2 > len(data):
            return None
        fix.alt_known = True
        fix.alt_m = struct.unpack(">h", data[at:at + 2])[0]
        at += 2
    if flags & FLAG_COURSE:
        if at + 1 > len(data):
            return None
        fix.course_known = True
        fix.course_ddeg = data[at] * 20
        at += 1
    if flags & FLAG_SPEED:
        if at + 1 > len(data):
            return None
        fix.speed_cms = data[at] * 50
        at += 1
    if flags & FLAG_SATS:
        if at + 1 > len(data):
            return None
        fix.sats = data[at]
        at += 1
    return fix


if __name__ == "__main__":
    import sys
    raw = bytes.fromhex(sys.argv[1]) if len(sys.argv) > 1 else b""
    print(decode(raw))
