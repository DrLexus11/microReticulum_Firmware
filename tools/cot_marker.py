"""Point markers as tens of bytes: the last of the 850 typed events.

Measured in this lab, everything that is a single point on a map:

    b-m-p-s-p-i   121   pointer / SPI      558 B
    a-h-G          67   hostile marker     838 B
    a-f-G-U-C-I    35   friendly, icon
    b-m-p-s-m       1   spot marker
    b-m-p-c-cp      1   route checkpoint

Almost all of a marker is rebuildable at the far end. Taking a real hostile
marker: `creator` and `link` both name the sender, which the receiver already
knows from the packet; `usericon` is a path derived from the type; `status`,
`archive` and an empty `remarks` say nothing. What is irreducible is where it
is, what it is, what it is called, and long enough to live.

**SPI is the volume case and is also the one that leaks.** ATAK gives it the uid
`ANDROID-<device id>.SPI1`, so forwarding the uid verbatim would put the sender's
device identifier on the air -- exactly what pivot 1 removed from everything
else. A uid that is not a UUID is carried as its suffix alone and rebuilt
against the sender's Reticulum-rooted UID at the far end, so `SPI1` travels and
`ANDROID-7819dadfcf858641` does not.

SPI is also tier 1 by the classification in TAKNative.md -- ephemeral,
latest-wins, loss is fine because a fresher one is along in a moment -- so the
caller is expected to gate it the way position is gated. This module encodes;
it does not decide how often.
"""

import struct
import uuid

import tak_payload
from cot_endpoint import _parse

VERSION = tak_payload.MARKER_V1

FLAG_ALT = 0x01
FLAG_COLOR = 0x02
FLAG_REMARKS = 0x04
# Set when the uid is a UUID carried as sixteen raw bytes. Clear when it is a
# suffix to be rebuilt against the sender's UID, which is how SPI travels.
FLAG_UUID = 0x08
# Set when the stale field counts minutes rather than seconds.
#
# Sixteen bits of seconds is eighteen hours, and ATAK routinely writes a stale
# a year out for a spot marker -- which clamped to eighteen hours, quietly
# turning a permanent marker into one that vanishes overnight. Seconds where
# they fit, because an SPI lives twenty of them; minutes where they do not,
# which reaches forty-five years. No extra bytes, and the units are stated
# rather than guessed at from the magnitude.
FLAG_STALE_MINUTES = 0x10

MAX_TYPE = 32
MAX_CALLSIGN = 64
MAX_UID_SUFFIX = 32
MAX_REMARKS = 200

# ATAK's own sentinel for "not measured". Carrying it as a number would claim a
# marker is nine thousand kilometres up.
UNKNOWN = 9999999.0

# A marker that never goes stale is a marker nobody can trust later. The field
# is two bytes with a unit flag, so it reaches forty-five years without
# wrapping -- and wrapping is the failure that matters, since it would turn a
# year into a minute rather than into something obviously wrong.
MAX_STALE_UNITS = 0xFFFF
SECONDS_PER_MINUTE = 60

_HEADER = struct.Struct(">BBIiiHBBB")
HEADER_BYTES = _HEADER.size

# The pointer ATAK moves around constantly. Named here because the caller has
# to know which type to rate limit, and a second copy of the string somewhere
# else is how the two drift apart.
SPI_TYPE = "b-m-p-s-p-i"


def is_marker(cot_xml):
    """Whether this event is a single point somebody put on the map.

    Not position: a self-report describes the reporter, and goes through the
    position codec on its own cadence. This is everything else with a point.
    """
    try:
        event = _parse(cot_xml)
    except ValueError:
        return False
    kind = event.get("type") or ""
    if event.find("point") is None:
        return False
    # A unit self-report belongs to the position codec.
    if kind.startswith("a-") and "-U-" in kind:
        return False
    # Drawings and routes are more than a point and are PR C's problem.
    if kind.startswith(("u-d-", "b-m-r", "u-r-")):
        return False
    return kind.startswith(("a-", "b-m-p-"))


def marker_from_cot(cot_xml, sender_id):
    """Pack a marker, or None if this event is not one."""
    try:
        event = _parse(cot_xml)
    except ValueError:
        return None
    if not is_marker(cot_xml):
        return None
    point = event.find("point")
    try:
        lat_e7 = int(round(float(point.get("lat")) * 1e7))
        lon_e7 = int(round(float(point.get("lon")) * 1e7))
    except (TypeError, ValueError):
        return None

    kind = event.get("type") or ""
    uid = event.get("uid") or ""
    contact = event.find("detail/contact")
    callsign = (contact.get("callsign") if contact is not None else "") or ""
    remarks = event.find("detail/remarks")
    remarks_text = (remarks.text or "") if remarks is not None else ""
    color = event.find("detail/color")
    altitude = _number(point.get("hae"))

    flags = 0
    uid_bytes = b""
    suffix = b""
    try:
        uid_bytes = uuid.UUID(uid).bytes
        flags |= FLAG_UUID
    except (ValueError, AttributeError):
        # Not a UUID. Carry only the part after the last dot -- "SPI1" -- and
        # leave the sender's device identifier behind.
        tail = uid.rsplit(".", 1)[-1] if "." in uid else uid
        suffix = tail.encode("utf-8")
        if not suffix or len(suffix) > MAX_UID_SUFFIX:
            return None

    if altitude is not None and abs(altitude) < 32000:
        flags |= FLAG_ALT
    argb = None
    if color is not None:
        try:
            argb = int(color.get("argb"))
            flags |= FLAG_COLOR
        except (TypeError, ValueError):
            argb = None
    remarks_bytes = _fit_utf8(remarks_text, MAX_REMARKS)
    if remarks_bytes:
        flags |= FLAG_REMARKS

    type_bytes = kind.encode("utf-8")
    callsign_bytes = callsign.encode("utf-8")
    if not type_bytes or len(type_bytes) > MAX_TYPE:
        return None
    if len(callsign_bytes) > MAX_CALLSIGN:
        return None

    stale_value, stale_in_minutes = _stale_field(_stale_seconds(event))
    if stale_in_minutes:
        flags |= FLAG_STALE_MINUTES
    body = _HEADER.pack(VERSION, flags, sender_id, lat_e7, lon_e7,
                        stale_value, len(type_bytes), len(callsign_bytes),
                        len(suffix))
    body += uid_bytes if flags & FLAG_UUID else suffix
    body += type_bytes + callsign_bytes
    if flags & FLAG_ALT:
        body += struct.pack(">h", int(round(altitude)))
    if flags & FLAG_COLOR:
        body += struct.pack(">i", argb)
    if flags & FLAG_REMARKS:
        body += struct.pack(">B", len(remarks_bytes)) + remarks_bytes
    return body


def decode(frame):
    """Unpack a marker, or None for anything that is not one."""
    if not isinstance(frame, (bytes, bytearray)) or len(frame) < HEADER_BYTES:
        return None
    frame = bytes(frame)
    (version, flags, sender_id, lat_e7, lon_e7, stale,
     type_length, callsign_length, suffix_length) = _HEADER.unpack(frame[:HEADER_BYTES])
    if version != VERSION:
        return None
    if not 1 <= type_length <= MAX_TYPE or callsign_length > MAX_CALLSIGN:
        return None
    at = HEADER_BYTES
    if flags & FLAG_UUID:
        if suffix_length:
            # A frame claiming both a UUID and a suffix is not one we produced.
            return None
        if len(frame) < at + 16:
            return None
        marker_uid = str(uuid.UUID(bytes=frame[at:at + 16]))
        at += 16
    else:
        if not 1 <= suffix_length <= MAX_UID_SUFFIX or len(frame) < at + suffix_length:
            return None
        marker_uid = None                       # rebuilt against the sender
        suffix = frame[at:at + suffix_length]
        at += suffix_length
    if len(frame) < at + type_length + callsign_length:
        return None
    try:
        kind = frame[at:at + type_length].decode("utf-8", errors="strict")
        at += type_length
        callsign = frame[at:at + callsign_length].decode("utf-8", errors="strict")
        at += callsign_length
        suffix_text = None if marker_uid else suffix.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        return None

    altitude = None
    if flags & FLAG_ALT:
        if len(frame) < at + 2:
            return None
        altitude = struct.unpack(">h", frame[at:at + 2])[0]
        at += 2
    argb = None
    if flags & FLAG_COLOR:
        if len(frame) < at + 4:
            return None
        argb = struct.unpack(">i", frame[at:at + 4])[0]
        at += 4
    remarks = ""
    if flags & FLAG_REMARKS:
        if len(frame) < at + 1:
            return None
        length = frame[at]
        at += 1
        if len(frame) < at + length:
            return None
        try:
            remarks = frame[at:at + length].decode("utf-8", errors="strict")
        except UnicodeDecodeError:
            return None
        at += length
    # Trailing bytes mean this is not the frame it claims to be.
    if at != len(frame):
        return None
    return {
        "sender_id": sender_id, "uid": marker_uid, "uid_suffix": suffix_text,
        "type": kind, "callsign": callsign, "lat_e7": lat_e7, "lon_e7": lon_e7,
        "stale_seconds": stale * SECONDS_PER_MINUTE if flags & FLAG_STALE_MINUTES else stale,
        "alt_m": altitude, "argb": argb, "remarks": remarks,
    }


def build_marker_cot(decoded, sender_uid, sender_callsign, when, stale_when):
    """Rebuild an event ATAK accepts.

    `creator` and `link` are rebuilt from the sender rather than carried,
    because the receiver already knows who sent it -- and rebuilding them
    against the Reticulum-rooted UID is what keeps a device identifier off the
    map at this end too.
    """
    uid = decoded["uid"] or "%s.%s" % (sender_uid, decoded["uid_suffix"])
    detail = ['<contact callsign="%s"/>' % _escape(decoded["callsign"] or sender_callsign)]
    detail.append('<creator callsign="%s" type="a-f-G-U-C" uid="%s"/>'
                  % (_escape(sender_callsign), _escape(sender_uid)))
    detail.append('<link parent_callsign="%s" relation="p-p" type="a-f-G-U-C" uid="%s"/>'
                  % (_escape(sender_callsign), _escape(sender_uid)))
    if decoded["argb"] is not None:
        detail.append('<color argb="%d"/>' % decoded["argb"])
    if decoded["remarks"]:
        detail.append("<remarks>%s</remarks>" % _escape(decoded["remarks"]))
    if decoded["alt_m"] is not None:
        detail.append('<precisionlocation altsrc="GPS"/>')
    detail.append("<archive/>")
    altitude = decoded["alt_m"] if decoded["alt_m"] is not None else UNKNOWN
    return ('<event version="2.0" uid="%s" type="%s" how="h-g-i-g-o" '
            'time="%s" start="%s" stale="%s">'
            '<point lat="%.7f" lon="%.7f" hae="%.1f" ce="%.1f" le="%.1f"/>'
            '<detail>%s</detail></event>'
            % (_escape(uid), _escape(decoded["type"]), when, when, stale_when,
               decoded["lat_e7"] / 1e7, decoded["lon_e7"] / 1e7, float(altitude),
               UNKNOWN, UNKNOWN, "".join(detail)))


def _stale_seconds(event):
    """How long the author said this marker is good for, in seconds.

    Taken from the event rather than invented: a spot marker is good for a
    year and an SPI for twenty seconds, and giving them the same life would
    either clutter a map permanently or blink a pointer out of existence.
    """
    from datetime import datetime
    start, stale = event.get("start"), event.get("stale")
    if not start or not stale:
        return 300
    try:
        begin = datetime.fromisoformat(start.replace("Z", "+00:00"))
        end = datetime.fromisoformat(stale.replace("Z", "+00:00"))
    except ValueError:
        return 300
    return max(1, int((end - begin).total_seconds()))


# Shown when a note was cut, so a reader can tell a truncated remark from one
# that simply ended. Three bytes, reserved from the limit rather than added to
# it.
_ELLIPSIS = "\u2026".encode("utf-8")


def _fit_utf8(text, limit):
    """The note as UTF-8, cut at a character boundary if it is too long.

    Slicing encoded bytes at an arbitrary index splits multibyte characters,
    and the far end decodes strictly -- so one Turkish character landing on the
    boundary made decode() reject the frame and the marker vanished with no
    error anywhere. An operator loses the marker, not the tail of a sentence,
    and never learns why.

    Remarks truncate where chat text refuses (see cot_chat.encode) because they
    are different things: a chat line *is* its text, so cutting it destroys the
    message, while a note annotates a marker whose position and type are the
    payload. Losing the marker to save the note is the wrong trade.
    """
    raw = text.encode("utf-8")
    if len(raw) <= limit:
        return raw
    cut = limit - len(_ELLIPSIS)
    # A UTF-8 continuation byte is 0b10xxxxxx. While the first excluded byte is
    # one, the cut is inside a character; step back until it is not.
    while cut > 0 and (raw[cut] & 0xC0) == 0x80:
        cut -= 1
    return raw[:cut] + _ELLIPSIS


def _stale_field(seconds):
    """The two-byte stale value and whether it counts minutes.

    Seconds while they fit, so an SPI's twenty seconds survives exactly.
    Minutes beyond that, which is the only way a year-long spot marker crosses
    in two bytes without becoming eighteen hours.
    """
    if seconds <= MAX_STALE_UNITS:
        return seconds, False
    minutes = (seconds + SECONDS_PER_MINUTE - 1) // SECONDS_PER_MINUTE
    return min(minutes, MAX_STALE_UNITS), True


def _number(text):
    try:
        value = float(text)
    except (TypeError, ValueError):
        return None
    return None if abs(value) >= UNKNOWN else value


def _escape(value):
    return (str(value).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))
