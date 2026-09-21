"""One oversized event, cut into packets that each stand on their own.

Tier 2 refuses anything over 383 B, which is why drawings and a nine-line
MEDEVAC do not cross. Tier 3 is the answer, and the shape was chosen by
measurement rather than by taste -- see *Tier 3: how it fragments* in
docs/TAKDeliveryPlan.md.

**Each fragment is a whole LXMF message.** That is the point. LXMF already
gives every message a delivery proof and a retry, so cutting a payload into
fragments and sending each as its own message inherits selective
retransmission instead of reimplementing it. The far end only has to
reassemble, which is a buffer, an ordering and a timeout.

The alternative was one `Resource` over one `Link`, which RNS does well and
LXMF will reach for by itself above roughly six fragments. Below that the link
handshake costs more than the per-fragment proofs, and everything on the LoRa
requirement list is below it: a compressed drawing is two fragments, a MEDEVAC
is one.

The airtime gap is about four seconds for a team of seven. The failure mode
matters more: a `Resource` is all-or-nothing on a link that may not establish
-- measured at 5 of 15 on a congested channel -- while independent proved
fragments retry one at a time, so a drawing arrives late rather than not at all.

## The wire

    byte 0      kind (tak_payload.FRAGMENT_V1)
    bytes 1-4   transfer id, four random bytes
    byte 5      index, zero-based
    byte 6      count, total fragments in this transfer
    bytes 7+    this slice of the tier-2 frame

Seven bytes of header against a 383 B packet. The transfer id is random rather
than sequential because two nodes fragmenting at once must not collide, and a
counter would have to survive a restart to promise that.

Index and count are a byte each, so a transfer is at most 255 fragments -- far
above the crossover where this scheme stops being the right one anyway, and
small enough that a corrupt count cannot ask a receiver for megabytes.
"""

import os
import struct
import time

import tak_payload

HEADER = struct.Struct(">BIBB")
HEADER_BYTES = HEADER.size          # 7

# What one fragment may carry, so the whole frame fits ONE LXMF message in ONE
# packet.
#
# Not 383. That is the bare-packet MDU, and a fragment does not travel as a
# bare packet -- it travels as an LXMF message, because that is the only way it
# gets a proof and a retry. Measured 2026-09-21 against LXMF 1.1.1: the
# envelope costs a constant **107 bytes** on the air, so 276 B of frame is the
# largest that still fits a single packet, and at 309 B LXMF stops using a
# packet at all and builds a Resource over a Link.
#
# Getting this wrong is not a small loss. A 383 B fragment packs to 490 B on
# the air, which is a Link and a Resource *per fragment* -- eight fragments,
# eight link handshakes, on a path where establishment was measured at 5 of 15
# when the channel was busy. That is strictly worse than the single Resource
# this design rejected, and it would have looked like tier 3 working.
#
# The margin below 276 is for fields LXMF may add to a message that this bench
# measurement did not carry -- a ticket, a stamp. Twenty bytes of headroom
# costs nothing here: at this size a 736 B drawing is three fragments either
# way.
LXMF_ENVELOPE_BYTES = 107
LXMF_HEADROOM_BYTES = 20
MAX_FRAGMENT_FRAME_BYTES = 383 - LXMF_ENVELOPE_BYTES - LXMF_HEADROOM_BYTES
MAX_FRAGMENT_BYTES = MAX_FRAGMENT_FRAME_BYTES - HEADER_BYTES

# Above this, LXMF's own Resource is cheaper and this scheme should not be used.
# Measured crossover is about six; see the plan. Kept as the honest bound of the
# wire format rather than the policy -- the policy lives with the caller.
MAX_FRAGMENTS = 255

# How long a partial transfer is worth holding.
#
# Long enough for LXMF to exhaust its retries on a slow path, short enough that
# a sender who gave up does not leave a buffer resident for the session. A
# partial drawing is worth nothing, so holding one longer buys nothing either.
REASSEMBLY_TIMEOUT_SECONDS = 5 * 60


def fragments(payload, transfer_id=None):
    """Cut a payload into fragments. Returns a list of whole frames.

    A payload that already fits is still returned as one fragment rather than
    special-cased: a receiver that only understands fragments is simpler than
    one that must also recognise an unfragmented form, and the seven bytes buy
    that.
    """
    payload = bytes(payload)
    if not payload:
        raise ValueError("nothing to fragment")
    count = -(-len(payload) // MAX_FRAGMENT_BYTES)
    if count > MAX_FRAGMENTS:
        raise ValueError(
            "%d bytes needs %d fragments, over the %d-fragment bound; this "
            "belongs in a Resource, not here"
            % (len(payload), count, MAX_FRAGMENTS))
    if transfer_id is None:
        # Random, not sequential: two nodes fragmenting at the same moment must
        # not collide, and a counter would have to survive a restart to promise
        # that.
        transfer_id = struct.unpack(">I", os.urandom(4))[0]
    out = []
    for index in range(count):
        slice_ = payload[index * MAX_FRAGMENT_BYTES:(index + 1) * MAX_FRAGMENT_BYTES]
        out.append(HEADER.pack(tak_payload.FRAGMENT_V1, transfer_id, index, count)
                   + slice_)
    return out


def decode(frame):
    """(transfer_id, index, count, slice) or None if this is not a fragment.

    None is ordinary: every other codec shares this destination, and a frame
    that is not ours is not an error.
    """
    if not isinstance(frame, (bytes, bytearray)) or len(frame) <= HEADER_BYTES:
        return None
    frame = bytes(frame)
    kind, transfer_id, index, count = HEADER.unpack(frame[:HEADER_BYTES])
    if kind != tak_payload.FRAGMENT_V1:
        return None
    # A count of zero cannot be satisfied and an index outside it never will
    # be. Both mean a frame that is not what it claims, and holding a buffer
    # for one is how a malformed peer costs us memory.
    if count < 1 or count > MAX_FRAGMENTS or index >= count:
        return None
    return transfer_id, index, count, frame[HEADER_BYTES:]


class Reassembler:
    """Partial transfers, until they are whole or too old.

    Keyed on the sender as well as the transfer id. Two peers can pick the same
    random id, and merging their fragments would produce a frame that decodes
    to something neither of them sent -- which is worse than dropping both.
    """

    def __init__(self, timeout_seconds=REASSEMBLY_TIMEOUT_SECONDS,
                 max_transfers=16, observer=None):
        self.timeout = timeout_seconds
        self.max_transfers = max_transfers
        self.partial = {}
        self.completed = 0
        self.dropped = 0
        # A diagnostic seam, not part of the wire. The reassembly window was
        # chosen against an estimate of how long a transfer takes on LoRa, and
        # the only way to find out whether that estimate was right is to watch
        # fragments arrive. `feed` deletes the entry before it returns the
        # payload, so elapsed time cannot be recovered afterwards -- it has to
        # be observed as it happens.
        self.observer = observer

    def feed(self, sender, frame, now=None):
        """Take one fragment. Returns the whole payload, or None if not yet.

        `sender` is whatever identifies the peer -- a destination hash. It is
        part of the key, never trusted as content.
        """
        decoded = decode(frame)
        if decoded is None:
            return None
        transfer_id, index, count, slice_ = decoded
        now = time.time() if now is None else now
        self._expire(now)
        key = (bytes(sender or b""), transfer_id)
        entry = self.partial.get(key)
        if entry is None:
            if len(self.partial) >= self.max_transfers:
                # A node that never completes a transfer must not be able to
                # grow this without limit. The oldest goes, because the newest
                # is the one still arriving.
                oldest = min(self.partial, key=lambda k: self.partial[k]["first"])
                del self.partial[oldest]
                self.dropped += 1
            entry = {"count": count, "first": now, "slices": {}}
            self.partial[key] = entry
        if entry["count"] != count:
            # The same transfer id describing two different lengths. One of the
            # two is not what it claims; neither is worth guessing at.
            del self.partial[key]
            self.dropped += 1
            return None
        entry["slices"][index] = slice_
        if self.observer is not None:
            # Elapsed since the *first* fragment of this transfer, which is the
            # number the timeout is measured against.
            self.observer(index, count, len(entry["slices"]), now - entry["first"])
        if len(entry["slices"]) < count:
            return None
        del self.partial[key]
        self.completed += 1
        return b"".join(entry["slices"][i] for i in range(count))

    def _expire(self, now):
        """A partial transfer is worth nothing, so it is not worth keeping."""
        stale = [key for key, entry in self.partial.items()
                 if now - entry["first"] > self.timeout]
        for key in stale:
            del self.partial[key]
            self.dropped += 1

    def pending(self):
        return len(self.partial)
