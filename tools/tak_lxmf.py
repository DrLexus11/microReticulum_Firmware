"""Chat that survives a partition, by putting it on LXMF.

PR B made chat cheap and, in doing so, quietly moved it onto tier-1 transport:
one bare `Packet(...).send()` per member, no proof, no retry, no queue. That is
correct for a position report, where a fresher one is seconds away, and wrong
for a chat line, where there is no fresher one. `TAKNative.md` said so before
the code did -- tier 2 is *must arrive*, and it named LXMF.

ATAK cannot rescue this on its own. Its delivery receipt is a **report** that
something landed; it is not a retransmission mechanism, and ATAK never resends.
So a chat line lost on LoRa is lost, the sender sees a message with no tick and
no explanation, and on a callout that is the failure the whole system exists to
prevent.

What LXMF brings that we would otherwise rebuild: proof-backed delivery,
retries, and propagation nodes that hold a message for somebody who is out of
range and hand it over when they come back. Rebuilding those badly is worse
than using them.

## What travels this way, and what does not

Addressed chat only -- and that split is the one `TAKNative.md` already drew:

    addressed (a direct message)   LXMF      receipts, retry, store-and-forward
    team broadcast (a room line)   Packet    best effort, and that is all

It is a property of broadcast, not a shortcut. A room has no member list to
retry against and no receipt to wait for; reliable multicast over a
partitionable mesh means either an acknowledgement from every member or blind
repetition, and both spend airtime on the assumption that somebody missed it.
A direct message has exactly one recipient, so all three guarantees are cheap.

## Why a direct message is also a real Columba message

The frame travels in `FIELD_CUSTOM_DATA`, tagged by `FIELD_CUSTOM_TYPE` --
upstream LXMF's own pair for an application's payload, which any other client
skips. The **text also travels as ordinary LXMF content**, so the same line
appears in the recipient's Columba conversation and in their ATAK. One person is one conversation whichever app is open, and an
operator who missed a message in ATAK can still answer it from the phone.

Room traffic carries no content for the same reason in reverse: operational
chatter from a ten-person room has no business burying somebody's personal
conversations, so a room line never becomes a Columba message at all.

The receiving TAK endpoint always reads the frame from the field and never from
the content. Content is for humans; the field is the protocol. Keeping that
distinction absolute is what stops a rendered message being parsed back as one.
"""

import RNS
import LXMF

# Upstream LXMF's own pair for an application's payload: CUSTOM_TYPE names
# whose data it is, CUSTOM_DATA carries it. Both are flat, which matters more
# than it looks -- the nested alternative (a dict under CUSTOM_META, 0xFD)
# collides with the telemetry extras Columba already keeps there, and a nested
# structure has to be pre-shaped with backend-private helpers the app module
# cannot reach. A flat pair crosses both of Columba's backends unchanged, and
# CUSTOM_DATA is the semantically right field besides: this is an app's data,
# not metadata about somebody's message.
FIELD_CUSTOM_TYPE = 0xFB
FIELD_CUSTOM_DATA = 0xFC

# What CUSTOM_TYPE says, so a client that does not know us skips the payload
# rather than guessing at it.
TAK_CUSTOM_TYPE = "tak.chat.v1"

LXMF_APP_NAME = "lxmf"
LXMF_DELIVERY_ASPECT = "delivery"


def delivery_destination(identity, direction):
    """A peer's LXMF inbox, derived from the identity we already hold.

    Nothing extra has to be announced or configured for this: the TAK node
    destination and the LXMF delivery destination are built from the *same*
    identity, so knowing a peer well enough to address their node is knowing
    them well enough to address their inbox.
    """
    return RNS.Destination(identity, direction, RNS.Destination.SINGLE,
                           LXMF_APP_NAME, LXMF_DELIVERY_ASPECT)


def frame_from_message(message):
    """The TAK frame carried by an LXMF message, or None if it carries none.

    None is ordinary: most LXMF traffic is somebody's actual conversation and
    has nothing to do with TAK.
    """
    fields = getattr(message, "fields", None) or {}
    kind = fields.get(FIELD_CUSTOM_TYPE)
    if isinstance(kind, (bytes, bytearray)):
        kind = kind.decode("utf-8", errors="replace")
    if kind != TAK_CUSTOM_TYPE:
        return None
    frame = fields.get(FIELD_CUSTOM_DATA)
    if isinstance(frame, (bytes, bytearray)) and frame:
        return bytes(frame)
    return None


class Carrier:
    """The LXMF router this endpoint sends and receives chat through.

    One per endpoint rather than one per process, matching CotOutbound: the
    router is bound to the node's identity, and changing team tears the
    endpoint down.
    """

    def __init__(self, identity, storage_path, callsign, on_chat,
                 propagation_node=None):
        self.identity = identity
        self.on_chat = on_chat
        self.sent = 0
        self.delivered = 0
        self.failed = 0
        self.propagated = 0
        self.router = LXMF.LXMRouter(identity=identity, storagepath=storage_path)
        self.destination = self.router.register_delivery_identity(
            identity, display_name=callsign)
        self.router.register_delivery_callback(self._inbound)
        if propagation_node:
            self.router.set_outbound_propagation_node(propagation_node)
        self.propagation_node = propagation_node

    def stop(self):
        try:
            self.router.exit_handler()
        except Exception:
            # Shutting down is not a place to raise. A router that will not
            # close cleanly must not stop the endpoint from closing.
            pass

    def build(self, destination, frame, text, method):
        """One LXMF message carrying one TAK frame.

        Shared by the direct send and by the propagation fallback so the two
        cannot drift: an escalated message that carried different fields would
        arrive as a different message, or as nothing a TAK endpoint recognises.
        """
        return LXMF.LXMessage(
            destination, self.destination,
            # Content is for the human; the field is the protocol. A room line
            # carries no content precisely so that it never becomes a Columba
            # conversation.
            content=text or "",
            fields={FIELD_CUSTOM_TYPE: TAK_CUSTOM_TYPE,
                    FIELD_CUSTOM_DATA: bytes(frame)},
            desired_method=method)

    def send_chat(self, identity, frame, text=""):
        """Send one chat frame to one peer. Returns True if it was accepted.

        Accepted, not delivered: LXMF takes ownership here and reports the
        outcome later through the delivery and failure callbacks. That is the
        whole point -- the caller is not blocked on a radio, and a peer who is
        out of range gets the message when they return rather than never.
        """
        try:
            destination = delivery_destination(identity, RNS.Destination.OUT)
            # Link-based, so it is proof-backed and retried. A chat line is an
            # operator action and rare; the link is affordable here in a way it
            # would not be for a position beacon.
            message = self.build(destination, frame, text,
                                 LXMF.LXMessage.DIRECT)
            # What the fallback needs to build a *fresh* message if this one
            # fails, carried on the message itself so it cannot outlive it.
            # See _failed for why a second message is required rather than a
            # second attempt at this one.
            message.tak_rebuild = (destination, bytes(frame), text or "")
            message.register_delivery_callback(self._delivered)
            message.register_failed_callback(self._failed)
            self.router.handle_outbound(message)
            self.sent += 1
            # Printed rather than only counted: whether a direct message took
            # the reliable path or quietly fell back to a bare packet is the
            # one fact PR C turns on, and a passing round-trip test cannot
            # tell the two apart. tak_team_acceptance.sh greps for this.
            print("[lxmf] direct message sent over LXMF (%d bytes, %d of text)"
                  % (len(frame), len(text or "")), flush=True)
            return True
        except Exception as error:
            # An unreachable peer is an ordinary event on a mesh, not a fault
            # in this process. Counted so the endpoint can say so, never fatal:
            # one unreachable member must not cost the others their copy.
            self.failed += 1
            print("[lxmf] could not send chat: %s" % error, flush=True)
            return False

    # ---- outcomes ----

    def _delivered(self, message):
        self.delivered += 1

    def _failed(self, message):
        """A direct delivery that did not land.

        Retried through a propagation node when one is configured, which is
        what turns "they were out of range" into "they got it when they came
        back". LXMF 1.1.1 has no try-propagation-on-fail of its own, so the
        fallback is explicit here rather than assumed.

        **A new message, not the same one re-aimed.** That is upstream's
        constraint, not a preference. `LXMessage.pack()` is one-shot and raises
        on a second call, and `transient_id` -- which the propagation stamp is
        computed over -- is only ever assigned in pack()'s PROPAGATED branch. A
        message packed for DIRECT therefore has no transient_id and cannot
        acquire one, so asking the router to propagate it lands in
        `get_propagation_stamp`, which calls `pack()` again and raises inside
        LXMF's own stamp thread. Observed on the bench 2026-09-13: every
        escalation died there, which is why nothing ever reached the message
        store. Flipping `desired_method` and resetting `state` looked like
        enough and was not.

        Escalated once, and once only: the fresh message carries
        `_propagation_failed`, not this callback. Leaving this one on it would
        mean a propagation node that is itself unreachable produces an
        unbounded loop -- escalate, fail, escalate -- each turn regenerating a
        proof-of-work stamp (0.59-1.70 s of CPU on the deck at LXMF's minimum
        cost of 13) and putting another Link attempt on the air. A propagation
        node is unreachable exactly when the mesh is partitioned, which is
        exactly when this path runs, so the loop would begin at the moment the
        channel can least afford it.
        """
        if not self.propagation_node:
            self.failed += 1
            return
        ingredients = getattr(message, "tak_rebuild", None)
        if ingredients is None:
            # Somebody else's message on a shared router, or one from before a
            # restart. Not ours to rebuild, and guessing at its fields would
            # put a message on the air that nothing can read.
            self.failed += 1
            return
        destination, frame, text = ingredients
        try:
            escalated = self.build(destination, frame, text,
                                   LXMF.LXMessage.PROPAGATED)
            escalated.register_delivery_callback(self._delivered)
            escalated.register_failed_callback(self._propagation_failed)
            self.router.handle_outbound(escalated)
            self.propagated += 1
        except Exception as error:
            self.failed += 1
            print("[lxmf] could not propagate chat: %s" % error, flush=True)

    def _propagation_failed(self, message):
        """The escalation did not land either, and there is nowhere left.

        Counted as a failure rather than left as a propagation, because
        `propagated` is read as "somebody is holding this for them" -- and
        after this, nobody is. PR F renders that distinction to an operator as
        queued-for-whom, so it has to mean what it says.
        """
        if self.propagated > 0:
            self.propagated -= 1
        self.failed += 1
        print("[lxmf] message could not be propagated either; "
              "no node is holding it", flush=True)

    def _inbound(self, message):
        frame = frame_from_message(message)
        if frame is None:
            # Somebody's actual conversation, not ours. Left alone: this
            # router is shared with the operator's own messaging.
            return
        source = None
        try:
            source = message.source_hash
        except AttributeError:
            pass
        self.on_chat(frame, source)
