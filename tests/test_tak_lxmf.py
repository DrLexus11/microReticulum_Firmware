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
    def test_the_frame_rides_upstreams_documented_extension_point(self):
        """0xFD is FIELD_CUSTOM_META, which upstream documents as the field
        other LXMF clients should ignore. An invented id in the unassigned
        range would collide the day upstream assigns it."""
        self.assertEqual(tak_lxmf.FIELD_CUSTOM_META, 0xFD)

    def test_the_key_does_not_collide_with_columbas_own(self):
        """Columba already carries telemetry extras in the same field under
        string keys. Sharing the field is fine; sharing a key would silently
        corrupt somebody's location share."""
        self.assertNotIn(tak_lxmf.TAK_META_KEY,
                         ("cease", "expires", "approxRadius"))

    def test_a_frame_survives_the_round_trip_through_the_field(self):
        frame = bytes([3, 0, 1, 2, 3])
        message = Message({tak_lxmf.FIELD_CUSTOM_META: {tak_lxmf.TAK_META_KEY: frame}})
        self.assertEqual(tak_lxmf.frame_from_message(message), frame)

    def test_somebody_elses_message_is_not_ours(self):
        """The router is shared with the operator's own messaging, so most of
        what arrives is a real conversation. Claiming it would be worse than
        missing ours."""
        for fields in ({},
                       {tak_lxmf.FIELD_CUSTOM_META: {"cease": True}},
                       {tak_lxmf.FIELD_CUSTOM_META: "not a dict"},
                       {0x02: {tak_lxmf.TAK_META_KEY: b"\x03x"}},
                       {tak_lxmf.FIELD_CUSTOM_META: {tak_lxmf.TAK_META_KEY: b""}},
                       {tak_lxmf.FIELD_CUSTOM_META: {tak_lxmf.TAK_META_KEY: "text"}}):
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
