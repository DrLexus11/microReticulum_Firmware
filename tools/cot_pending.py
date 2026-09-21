"""Frames from a sender this node cannot name yet, held until it can.

Membership is what turns a frame's four-byte sender id into a teammate, and
membership is learned from announces. A node that has just joined, or just
restarted into an empty table, therefore spends a while unable to name anyone
-- and a chat line or marker arriving in that window used to be dropped as
unreadable. Delivered, proved at the transport, and silently gone.

Measured on the bench 2026-09-21: a Nexus 6P joined the team and exchanged a
chat line with the deck within its first minute. The deck's reply was
delivered with a proof in 2.1 s and never appeared, because the Nexus had not
yet heard the deck announce; the next line, a few minutes later, arrived fine.

So such a frame is held, not dropped, and offered again whenever a member is
learned. Held for minutes rather than for ever: a frame from somebody who never
turns out to be a teammate is a frame from somebody else, and the team check is
not weakened by any of this -- a held frame is only ever drawn once its sender
has announced into the team.
"""

import threading
import time

HOLD_SECONDS = 10 * 60
MAX_HELD = 32


class PendingAttribution:
    """Frames waiting for their sender to be known."""

    def __init__(self, hold_seconds=HOLD_SECONDS, max_held=MAX_HELD):
        self.hold_seconds = hold_seconds
        self.max_held = max_held
        self._lock = threading.Lock()
        self._held = []   # [(when, raw, signed_by)]
        self.expired = 0

    def hold(self, raw, signed_by=None, now=None):
        """Keep one frame. The oldest goes first when the hold is full."""
        now = time.time() if now is None else now
        with self._lock:
            self._held.append((now, bytes(raw), signed_by))
            while len(self._held) > self.max_held:
                self._held.pop(0)
                self.expired += 1

    def ready(self, resolves, now=None):
        """The held frames whose sender now resolves, oldest first.

        `resolves(raw)` says whether a frame's sender can be named now. Those
        are handed back and forgotten; the rest stay, unless they have waited
        past the hold, when they are dropped for good.
        """
        now = time.time() if now is None else now
        released = []
        with self._lock:
            keep = []
            for when, raw, signed_by in self._held:
                if now - when > self.hold_seconds:
                    self.expired += 1
                elif resolves(raw):
                    released.append((raw, signed_by))
                else:
                    keep.append((when, raw, signed_by))
            self._held = keep
        return released

    def __len__(self):
        return len(self._held)
