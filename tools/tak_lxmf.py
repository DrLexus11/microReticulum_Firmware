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

The frame travels in `FIELD_CUSTOM_META`, which upstream LXMF documents as the
extension point other clients ignore. The **text also travels as ordinary LXMF
content**, so the same line appears in the recipient's Columba conversation and
in their ATAK. One person is one conversation whichever app is open, and an
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

# Upstream LXMF's documented extension point for app-specific metadata that
# other clients should ignore (0xFD). Columba already carries its own
# telemetry extras here under string keys -- "cease", "expires",
# "approxRadius" -- so this uses a distinct key rather than a second field id,
# which would risk colliding with whatever upstream assigns next.
FIELD_CUSTOM_META = 0xFD
TAK_META_KEY = "tak"

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
    meta = fields.get(FIELD_CUSTOM_META)
    if not isinstance(meta, dict):
        return None
    frame = meta.get(TAK_META_KEY)
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

    def send_chat(self, identity, frame, text=""):
        """Send one chat frame to one peer. Returns True if it was accepted.

        Accepted, not delivered: LXMF takes ownership here and reports the
        outcome later through the delivery and failure callbacks. That is the
        whole point -- the caller is not blocked on a radio, and a peer who is
        out of range gets the message when they return rather than never.
        """
        try:
            destination = delivery_destination(identity, RNS.Destination.OUT)
            message = LXMF.LXMessage(
                destination, self.destination,
                # Content is for the human; the field is the protocol. A room
                # line carries no content precisely so that it never becomes a
                # Columba conversation.
                content=text or "",
                fields={FIELD_CUSTOM_META: {TAK_META_KEY: bytes(frame)}},
                # Link-based, so it is proof-backed and retried. A chat line is
                # an operator action and rare; the link is affordable here in a
                # way it would not be for a position beacon.
                desired_method=LXMF.LXMessage.DIRECT)
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
        """
        if not self.propagation_node:
            self.failed += 1
            return
        try:
            message.desired_method = LXMF.LXMessage.PROPAGATED
            message.state = LXMF.LXMessage.GENERATING
            self.router.handle_outbound(message)
            self.propagated += 1
        except Exception as error:
            self.failed += 1
            print("[lxmf] could not propagate chat: %s" % error, flush=True)

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
