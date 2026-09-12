"""The LXMF carrier: what a direct message rides, and what it must not disturb."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

try:
    import tak_lxmf
except ImportError:  # pragma: no cover - LXMF is not installed everywhere
    tak_lxmf = None


class Message:
    """Enough of an LXMessage to exercise extraction, without a live router."""

    def __init__(self, fields=None, source_hash=b"\x01" * 16):
        self.fields = fields or {}
        self.source_hash = source_hash


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


if __name__ == "__main__":
    unittest.main()
