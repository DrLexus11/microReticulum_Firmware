"""The LXMF carrier: what a direct message rides, and what it must not disturb."""

import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

try:
    import RNS
    import LXMF
    import tak_lxmf
except ImportError:  # pragma: no cover - LXMF is not installed everywhere
    tak_lxmf = None


class Message:
    """Enough of an LXMessage to exercise extraction, without a live router."""

    def __init__(self, fields=None, source_hash=b"\x01" * 16):
        self.fields = fields or {}
        self.source_hash = source_hash


FIXTURES = json.loads(
    (Path(__file__).resolve().parent / "fixtures" / "tak_native_v1.json").read_text())


@unittest.skipIf(tak_lxmf is None, "LXMF is not installed in this interpreter")
class CrossLanguageTests(unittest.TestCase):
    """The field ids and the tag are hand-written in two languages. This is
    what stops them drifting: both sides assert against one fixture, and the
    fixture is copied byte-for-byte between the repositories."""

    def test_the_carrier_constants_match_the_fixture(self):
        lxmf = FIXTURES["lxmf"]
        self.assertEqual(tak_lxmf.FIELD_CUSTOM_TYPE, lxmf["field_custom_type"])
        self.assertEqual(tak_lxmf.FIELD_CUSTOM_DATA, lxmf["field_custom_data"])
        self.assertEqual(tak_lxmf.TAK_CUSTOM_TYPE, lxmf["custom_type_tag"])
        self.assertEqual(tak_lxmf.LXMF_APP_NAME, lxmf["app_name"])
        self.assertEqual(tak_lxmf.LXMF_DELIVERY_ASPECT, lxmf["delivery_aspect"])


@unittest.skipIf(tak_lxmf is None, "LXMF is not installed in this interpreter")
class FieldTests(unittest.TestCase):
    def test_the_frame_rides_upstreams_own_payload_fields(self):
        """0xFB/0xFC are LXMF's CUSTOM_TYPE and CUSTOM_DATA: the pair upstream
        provides for an application's payload. Both are flat, which is what
        lets the same shape cross both of Columba's backends -- and CUSTOM_META
        (0xFD) is the wrong field besides, being metadata about a message
        rather than an app's data, and already occupied by Columba's telemetry
        extras."""
        self.assertEqual(tak_lxmf.FIELD_CUSTOM_TYPE, 0xFB)
        self.assertEqual(tak_lxmf.FIELD_CUSTOM_DATA, 0xFC)

    def test_the_type_tag_is_versioned(self):
        """A client that does not know us skips the payload rather than
        guessing, and a future frame layout gets a new tag instead of
        pretending to be this one."""
        self.assertTrue(tak_lxmf.TAK_CUSTOM_TYPE.endswith(".v1"))

    def test_a_frame_survives_the_round_trip_through_the_fields(self):
        frame = bytes([3, 0, 1, 2, 3])
        message = Message({tak_lxmf.FIELD_CUSTOM_TYPE: tak_lxmf.TAK_CUSTOM_TYPE,
                           tak_lxmf.FIELD_CUSTOM_DATA: frame})
        self.assertEqual(tak_lxmf.frame_from_message(message), frame)

    def test_the_tag_survives_arriving_as_bytes(self):
        """msgpack round trips a str as a str, but a peer built on another
        stack may send it as bytes. Reading only one of the two would drop
        every message from that peer with no error anywhere."""
        frame = bytes([3, 9])
        message = Message({tak_lxmf.FIELD_CUSTOM_TYPE: tak_lxmf.TAK_CUSTOM_TYPE.encode(),
                           tak_lxmf.FIELD_CUSTOM_DATA: frame})
        self.assertEqual(tak_lxmf.frame_from_message(message), frame)

    def test_somebody_elses_message_is_not_ours(self):
        """The router is shared with the operator's own messaging, so most of
        what arrives is a real conversation. Claiming it would be worse than
        missing ours."""
        frame = b"\x03x"
        for fields in ({},
                       {tak_lxmf.FIELD_CUSTOM_DATA: frame},
                       {tak_lxmf.FIELD_CUSTOM_TYPE: "someone.else.v1",
                        tak_lxmf.FIELD_CUSTOM_DATA: frame},
                       {tak_lxmf.FIELD_CUSTOM_TYPE: tak_lxmf.TAK_CUSTOM_TYPE},
                       {tak_lxmf.FIELD_CUSTOM_TYPE: tak_lxmf.TAK_CUSTOM_TYPE,
                        tak_lxmf.FIELD_CUSTOM_DATA: b""},
                       {tak_lxmf.FIELD_CUSTOM_TYPE: tak_lxmf.TAK_CUSTOM_TYPE,
                        tak_lxmf.FIELD_CUSTOM_DATA: "not bytes"},
                       {0xFD: {"cease": True}}):
            self.assertIsNone(tak_lxmf.frame_from_message(Message(fields)),
                              "claimed %r" % (fields,))

    def test_a_message_with_no_fields_at_all_is_not_an_error(self):
        self.assertIsNone(tak_lxmf.frame_from_message(Message(None)))


@unittest.skipIf(tak_lxmf is None, "LXMF is not installed in this interpreter")
class AddressingTests(unittest.TestCase):
    def test_the_inbox_is_derived_from_the_identity_we_already_hold(self):
        """Nothing extra is announced for this. The TAK node destination and
        the LXMF inbox are built from the same identity, so knowing a peer well
        enough to address their node is knowing them well enough to address
        their inbox -- which is pivot 1 paying for itself a second time."""
        import RNS
        identity = RNS.Identity()
        inbox = tak_lxmf.delivery_destination(identity, RNS.Destination.OUT)
        # A SINGLE destination's name carries the identity hash after the
        # aspects, which is exactly why it resolves to one peer's inbox.
        self.assertTrue(inbox.name.startswith("lxmf.delivery."), inbox.name)
        self.assertIn(identity.hash.hex(), inbox.name)
        # The same aspects any other LXMF client uses, so a peer's ordinary
        # messaging app is what answers.
        self.assertEqual(tak_lxmf.LXMF_APP_NAME, "lxmf")
        self.assertEqual(tak_lxmf.LXMF_DELIVERY_ASPECT, "delivery")


class FakeRouter:
    def __init__(self):
        self.sent = []

    def handle_outbound(self, message):
        self.sent.append(message)


class FakeMessage:
    """Enough of an LXMessage to exercise the fallback.

    It keeps the failed callback the way a real one does, because whether the
    carrier moves that callback off this message is the whole question in the
    loop test below.
    """

    def __init__(self, rebuild=None):
        self.desired_method = None
        self.state = None
        self.failed_callback = None
        if rebuild is not None:
            self.tak_rebuild = rebuild

    def register_failed_callback(self, callback):
        self.failed_callback = callback

    def register_delivery_callback(self, callback):
        pass


def carrier(propagation_node=None, real_messages=False):
    """A Carrier without a live LXMF router behind it.

    Constructed field by field rather than through __init__, which would want a
    Reticulum instance and a storage directory. What is under test is the
    fallback decision, not LXMF's own propagation.

    With real_messages, `build` produces genuine LXMessages so that upstream's
    own packing rules apply -- which is the only way the re-pack bug below is
    visible at all.
    """
    made = tak_lxmf.Carrier.__new__(tak_lxmf.Carrier)
    made.router = FakeRouter()
    made.propagation_node = propagation_node
    made.sent = made.delivered = made.failed = made.propagated = 0
    if real_messages:
        made.destination = tak_lxmf.delivery_destination(RNS.Identity(),
                                                         RNS.Destination.OUT)
    else:
        made.build = lambda destination, frame, text, method: FakeMessage()
    return made


def rebuild_for(frame=b"\x01\x02", text="hello"):
    """The ingredients send_chat leaves on a message for the fallback."""
    destination = tak_lxmf.delivery_destination(RNS.Identity(),
                                                RNS.Destination.OUT)
    return (destination, frame, text)


@unittest.skipIf(tak_lxmf is None, "LXMF is not installed in this interpreter")
class PropagationTests(unittest.TestCase):
    """What turns "they were out of range" into "they got it when they came
    back". LXMF 1.1.1 has no try-propagation-on-fail of its own, so this
    fallback is ours and has to be tested as ours."""

    def test_a_failed_direct_message_is_retried_through_a_propagation_node(self):
        made = carrier(propagation_node=b"\x11" * 16)
        made._failed(FakeMessage(rebuild=rebuild_for()))
        self.assertEqual(made.propagated, 1)
        self.assertEqual(made.failed, 0)
        self.assertEqual(len(made.router.sent), 1)

    def test_the_escalation_is_a_new_message_that_has_never_been_packed(self):
        """The bug that made this whole guarantee a fiction, and it was
        invisible until it ran against real LXMF objects.

        LXMessage.pack() is one-shot and raises on a second call. transient_id
        -- which the propagation stamp is computed over -- is assigned *only*
        in pack()'s PROPAGATED branch, so a message packed for DIRECT has none
        and can never acquire one. Re-aiming that message therefore lands in
        get_propagation_stamp, which calls pack() again and raises inside
        LXMF's own stamp thread.

        Observed on the bench 2026-09-13: every escalation died there, the
        message store stayed empty through six polls over two minutes, and the
        bridge log filled with re-pack tracebacks. The old test passed the
        whole time, because a fake router that only records a call never packs
        anything. So this one asserts against a real LXMessage."""
        made = carrier(propagation_node=b"\x11" * 16, real_messages=True)
        original = FakeMessage(rebuild=rebuild_for())
        made._failed(original)

        self.assertEqual(len(made.router.sent), 1)
        escalated = made.router.sent[0]
        self.assertIsNot(escalated, original)
        self.assertEqual(escalated.desired_method, LXMF.LXMessage.PROPAGATED)
        # Never packed, so pack() is still available to the PROPAGATED branch
        # that alone can give it a transient_id.
        self.assertIsNone(escalated.packed)
        self.assertIsNone(escalated.transient_id)
        # And it still carries what makes it a TAK frame rather than a bare
        # line of text.
        self.assertEqual(escalated.fields[tak_lxmf.FIELD_CUSTOM_TYPE],
                         tak_lxmf.TAK_CUSTOM_TYPE)
        self.assertEqual(escalated.fields[tak_lxmf.FIELD_CUSTOM_DATA], b"\x01\x02")

    def test_with_no_propagation_node_it_is_a_failure_rather_than_a_pretence(self):
        """The honest outcome. A node with nowhere to propagate to cannot hold
        a message for somebody, and counting it as propagated would report a
        guarantee that was never made."""
        made = carrier(propagation_node=None)
        made._failed(FakeMessage(rebuild=rebuild_for()))
        self.assertEqual(made.failed, 1)
        self.assertEqual(made.propagated, 0)
        self.assertEqual(made.router.sent, [])

    def test_a_message_this_carrier_did_not_send_is_not_rebuilt(self):
        """The router is shared with the operator's own Columba messaging. A
        failed message with no rebuild on it is somebody's actual
        conversation, or one from before a restart; guessing at its fields
        would put a message on the air that nothing can read."""
        made = carrier(propagation_node=b"\x11" * 16)
        made._failed(FakeMessage())
        self.assertEqual(made.failed, 1)
        self.assertEqual(made.router.sent, [])

    def test_a_propagation_that_itself_fails_is_counted_not_raised(self):
        """Called from an LXMF callback, where raising would take the router's
        thread down and stop every later message rather than this one."""
        made = carrier(propagation_node=b"\x11" * 16)

        def explode(_message):
            raise OSError("no path")
        made.router.handle_outbound = explode
        made._failed(FakeMessage(rebuild=rebuild_for()))
        self.assertEqual(made.failed, 1)
        self.assertEqual(made.propagated, 0)

    def test_a_propagation_that_fails_later_is_not_escalated_again(self):
        """The loop this guards against fires at the worst possible moment.

        handle_outbound accepts a PROPAGATED message and reports the outcome
        later, through whatever failed callback is registered on it. Put this
        callback on the escalation and a propagation node that is itself
        unreachable means: direct fails, escalate, propagation fails, escalate
        again -- forever. Each turn regenerates a proof-of-work stamp, which
        measured 0.59-1.70 s of CPU on the deck at LXMF's minimum cost of 13,
        and puts another Link attempt on the air.

        A propagation node is unreachable exactly when the mesh is
        partitioned, which is exactly when this path runs. So the failure mode
        is a node that starts hammering a channel at the moment the channel is
        already in trouble. One escalation, then the truth."""
        made = carrier(propagation_node=b"\x11" * 16)
        made._failed(FakeMessage(rebuild=rebuild_for()))
        self.assertEqual(made.propagated, 1)
        escalated = made.router.sent[0]

        # LXMF reports the propagated attempt failing, through whatever
        # callback is registered on the escalated message now.
        self.assertIsNotNone(escalated.failed_callback)
        escalated.failed_callback(escalated)

        self.assertEqual(len(made.router.sent), 1, "escalated a second time")
        self.assertEqual(made.propagated, 0, "still counted as propagated")
        self.assertEqual(made.failed, 1)


if __name__ == "__main__":
    unittest.main()
