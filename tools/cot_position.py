"""Position as twenty bytes, not as seven hundred.

794 of the 853 events ATAK authored in this lab were position reports -- 94% of
everything it emits. PR A put all of them through tier 2 as compressed CoT, and
pivot 5 then addressed every event to every member, which multiplies. Measured
with the firmware's own airtime model at SF7/BW250:

    one PLI as tier 2 CoT          142 B frame     142 ms
    one PLI as this codec           21 B frame      53 ms

    ten nodes, four hops, one report each per minute
      as tier 2 CoT                 85% of the channel
      as this codec                 32% of the channel

Neither number is survivable on its own, which is the point worth keeping: the
codec is necessary and not sufficient. What makes position affordable is
sending fewer of them, so the cadence gate here matters as much as the encoding
and uses the same rules the firmware already uses -- an interval floor, plus a
movement threshold so a node that *has* moved does not wait out the interval,
because that is exactly when its position matters.

The wire format is `position_codec`, unchanged: the firmware, Columba and this
all speak the same nineteen-to-twenty-four bytes, and a second dialect of the
same thing is how two implementations start disagreeing about where somebody is.
"""

import position_codec
from cot_endpoint import _parse

# ATAK's own position type. Matching the prefix rather than the whole string
# because the tail varies with affiliation and battle dimension -- a-f-G-U-C is
# a friendly ground unit, a-h-G-U-C a hostile one -- and all of them are
# somebody reporting where a unit is.
PLI_TYPE_PREFIX = "a-"
PLI_TYPE_INFIX = "-U-"

# Matches POSITION_MOVE_THRESHOLD_M in PositionReport.h, and for the reason
# given there: 1e-7 degrees of latitude is about 1.11 cm, the same scale is
# applied to longitude, and away from the equator that makes the gate fire
# early rather than late. Reporting movement that did not happen is the safe
# direction; missing movement that did is not.
MOVE_THRESHOLD_M = 25
MOVE_THRESHOLD_E7 = int(MOVE_THRESHOLD_M * 10000000 / 111320)

# The floor between two reports from the same node.
#
# Sixty seconds is what TAKCapability.md measured as affordable for ten nodes,
# and it is a floor rather than a schedule: nothing here generates reports, it
# only declines to forward the ones ATAK produces faster than this.
DEFAULT_INTERVAL_SECONDS = 60


def is_position(cot_xml):
    """Whether this event is somebody reporting a unit's position.

    Not 'is it from us' -- that is the pipeline's question. This is about which
    codec the event belongs in.
    """
    try:
        event = _parse(cot_xml)
    except ValueError:
        return False
    kind = event.get("type") or ""
    return kind.startswith(PLI_TYPE_PREFIX) and PLI_TYPE_INFIX in kind


def fix_from_cot(cot_xml, sender_id):
    """A PositionFix from a CoT event, or None if it does not carry one.

    The inverse of cot_gateway.build_cot. Only the fields the wire format has
    room for are taken; everything else in the event is rendered locally at the
    far end and never travels.
    """
    try:
        event = _parse(cot_xml)
    except ValueError:
        return None
    point = event.find("point")
    if point is None:
        return None
    try:
        lat_e7 = int(round(float(point.get("lat")) * 1e7))
        lon_e7 = int(round(float(point.get("lon")) * 1e7))
    except (TypeError, ValueError):
        return None
    # ATAK reports 9999999.0 for "unknown", which as metres of accuracy would
    # be a claim about the whole planet. Zero is the format's own word for
    # unreported, and saying nothing is better than saying something absurd.
    accuracy = _bounded(point.get("ce"), 1, 254, unknown=0)
    fix = position_codec.PositionFix(
        sender_id=sender_id, lat_e7=lat_e7, lon_e7=lon_e7,
        fix_unix_s=0, accuracy_m=accuracy)
    altitude = _number(point.get("hae"))
    if altitude is not None and abs(altitude) < 32000:
        fix.alt_known = True
        fix.alt_m = int(round(altitude))
    track = event.find("detail/track")
    if track is not None:
        course = _number(track.get("course"))
        if course is not None:
            fix.course_known = True
            fix.course_ddeg = int(round(course)) % 360
        speed = _number(track.get("speed"))
        if speed is not None and speed > 0:
            fix.speed_cms = int(round(speed * 100))
    return fix


def _number(text):
    try:
        value = float(text)
    except (TypeError, ValueError):
        return None
    # ATAK's sentinel for "not known". Treating it as a measurement is how a
    # track ends up nine thousand kilometres in the air.
    return None if abs(value) >= 9999999.0 else value


def _bounded(text, low, high, unknown):
    value = _number(text)
    if value is None:
        return unknown
    return max(low, min(high, int(round(value))))


class PositionGate:
    """Decides whether a position is worth its airtime yet.

    The same two rules the firmware applies, for the same reasons: a floor
    between reports so a chatty client cannot spend the channel, and a movement
    threshold so a node that has actually moved does not sit behind that floor.
    """

    def __init__(self, interval_seconds=DEFAULT_INTERVAL_SECONDS,
                 move_threshold_e7=MOVE_THRESHOLD_E7):
        self.interval = interval_seconds
        self.move_threshold = move_threshold_e7
        self.last_sent = None
        self.last_lat_e7 = None
        self.last_lon_e7 = None
        self.suppressed = 0

    def allows(self, fix, now):
        """True if this fix should go out, and record it if so."""
        moved = (self.last_lat_e7 is not None and
                 (abs(fix.lat_e7 - self.last_lat_e7) >= self.move_threshold or
                  abs(fix.lon_e7 - self.last_lon_e7) >= self.move_threshold))
        due = self.last_sent is None or now - self.last_sent >= self.interval
        if not (due or moved):
            self.suppressed += 1
            return False
        self.last_sent = now
        self.last_lat_e7 = fix.lat_e7
        self.last_lon_e7 = fix.lon_e7
        return True
