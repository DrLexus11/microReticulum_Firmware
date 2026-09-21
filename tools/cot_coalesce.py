"""Latest wins, for an event sent again before its last version has settled.

An operator editing a shared drawing and sending it again, or sending it twice
to be sure, produces several versions of one event in quick succession. Each is
now a full tier-3 transfer: several LXMF messages per member, each with its own
proof and retry budget.

*Not* ATAK's auto-send, which this was first written against on a wrong
premise. Checked on the bench 2026-09-21 against ATAK 5.6's own manual and
code: auto-send exists only for unit markers, re-broadcasts about once every
60 s rather than on every change, and travels through the 54-byte marker codec
-- about 0.46 s of channel per minute for a team of seven. It never reaches
this path, and a dragged auto-sent marker kept up well on the radios.

**Costed, not guessed.** From `tools/position_budget.py`, one version of a
typical compressed drawing (736 B, three fragments) costs about 1.3 s of
channel per member for one LoRa hop, before any retry -- **about 7.7 s for a
team of seven.** A drag that emits one version a second therefore asks for
eight times what the channel has. Chat and positions would not merely slow
down; they would stop. So this is not an optimisation.

Two halves, one per end of the link:

`LatestWins` is the sender. Per uid it is a leading-and-trailing throttle: the
first version goes at once, because an operator who draws one shape should not
wait for it; anything that arrives inside the window replaces what is held,
and only the latest held version goes when the window closes. However long
the drag, one uid costs at most one transfer per window, and the final shape
always goes. A version identical to the one last sent is dropped outright.

`Freshness` is the receiver. Versions do not arrive in the order they were
sent -- an older one can finish its retries, or come back from the propagation
node, after a newer one has landed -- and drawing it would put a shape the
operator already moved back on the map. The CoT `time` attribute orders them:
ATAK stamps each version as it emits it, so an event older than the one last
drawn for the same uid is dropped.

Nothing here touches a socket, a thread or a clock of its own. The caller
supplies `now` and drives the trailing edge; that is what makes both halves
testable to the second.
"""

import threading
from datetime import datetime

# How long one uid waits between transfers.
#
# Set against the airtime, not against ATAK: a version of a drawing costs about
# 7.7 s of channel for a team of seven, so anything much shorter lets a single
# drag occupy the channel on its own. Ten seconds leaves the rest of the traffic
# some air. The first version is never delayed by this, only the ones behind it.
WINDOW_SECONDS = 10.0

# Bounded, because an operator drawing all day must not grow either table.
MAX_UIDS = 256

SEND = "send"
HOLD = "hold"
DROP = "drop"


class LatestWins:
    """Per-uid leading-and-trailing throttle for oversized events."""

    def __init__(self, window_seconds=WINDOW_SECONDS, max_uids=MAX_UIDS):
        self.window = window_seconds
        self.max_uids = max_uids
        self._lock = threading.Lock()
        self._sent = {}      # uid -> (when, digest)
        self._held = {}      # uid -> (payload, digest)
        self.coalesced = 0   # versions replaced before they were sent
        self.identical = 0   # versions dropped for matching what was sent

    def decide(self, uid, payload, digest, now):
        """Decide what to do with one version.

        Returns `(SEND, None)` to send now, `(HOLD, flush_at)` when the caller
        must arrange a flush at `flush_at`, or `(DROP, None)`. A version that
        replaces one already held also returns `(DROP, None)`: the flush that
        was already arranged will carry it.
        """
        with self._lock:
            sent = self._sent.get(uid)
            if sent is not None and sent[1] == digest:
                # Same bytes as the last version that went. Whether this is a
                # re-send of an unchanged drawing or a drag that came back to
                # where it started, the far end already has it.
                self._held.pop(uid, None)
                self.identical += 1
                return DROP, None
            if sent is None or now - sent[0] >= self.window:
                self._record_sent(uid, digest, now)
                self._held.pop(uid, None)
                return SEND, None
            replacing = uid in self._held
            self._held[uid] = (payload, digest)
            if replacing:
                self.coalesced += 1
                return DROP, None
            return HOLD, sent[0] + self.window

    def flush(self, uid, now):
        """The held version for this uid, now due, or None.

        None when nothing is held any more: a later version was identical to
        what had been sent and cleared it, or it has already gone.
        """
        with self._lock:
            held = self._held.pop(uid, None)
            if held is None:
                return None
            payload, digest = held
            sent = self._sent.get(uid)
            if sent is not None and sent[1] == digest:
                self.identical += 1
                return None
            self._record_sent(uid, digest, now)
            return payload

    def _record_sent(self, uid, digest, now):
        self._sent[uid] = (now, digest)
        if len(self._sent) > self.max_uids:
            oldest = min(self._sent, key=lambda key: self._sent[key][0])
            del self._sent[oldest]


def event_time(value):
    """A CoT timestamp as something comparable, or None.

    ATAK writes these as ISO-8601 with a trailing Z and a varying number of
    fractional digits, so they are parsed rather than compared as strings.
    """
    if not value:
        return None
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None


class Freshness:
    """Drop a version older than the one already drawn for the same uid."""

    def __init__(self, max_uids=MAX_UIDS):
        self.max_uids = max_uids
        self._lock = threading.Lock()
        self._drawn = {}   # uid -> datetime of the newest version drawn
        self.stale = 0

    def admit(self, uid, when):
        """True if this version should be drawn.

        An event without a uid or a readable time is admitted: this exists to
        stop a known older version overwriting a newer one, not to refuse
        anything it cannot place. A version with the *same* time is admitted
        too -- a replay to a reconnecting client is the same event again, and
        ATAK recognises it by uid.
        """
        if not uid or when is None:
            return True
        with self._lock:
            newest = self._drawn.get(uid)
            if newest is not None and when < newest:
                self.stale += 1
                return False
            self._drawn[uid] = when
            if len(self._drawn) > self.max_uids:
                oldest = min(self._drawn, key=lambda key: self._drawn[key])
                del self._drawn[oldest]
            return True
