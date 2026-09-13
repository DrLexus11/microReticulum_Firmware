"""The fix timestamp, which was zero for every position ever sent.

The wire format has carried fix_unix_s from the beginning and the parser
hard-coded it to zero, so every fix on the mesh claimed a timestamp of zero.
Anything downstream that tried to order fixes by age was comparing nothing with
nothing -- including the receiver's own out-of-order guard, which was written
against this field and was silently a no-op. Found 2026-09-13.
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cot_position  # noqa: E402


def event(stamp='2026-09-13T14:30:05.000Z'):
    return (
        '<event version="2.0" uid="ANDROID-abc" type="a-f-G-U-C" how="m-g" '
        'time="%s" start="%s" stale="2026-09-13T14:35:00.000Z">'
        '<point lat="41.0" lon="29.0" hae="30.0" ce="10.0" le="9999999.0"/>'
        '<detail><takv device="T" os="36" platform="ATAK-CIV" version="5.6"/>'
        '<contact callsign="LEXUS"/></detail></event>' % (stamp, stamp)
    )


class FixTimeTests(unittest.TestCase):
    def test_the_senders_own_time_is_carried(self):
        fix = cot_position.fix_from_cot(event(), sender_id=0x01020304)
        self.assertNotEqual(fix.fix_unix_s, 0)

    def test_it_is_when_the_fix_was_taken_not_when_it_was_relayed(self):
        """A relay time would make every fix look fresh at every hop, which is
        the opposite of what ordering needs."""
        early = cot_position.fix_from_cot(
            event('2026-09-13T14:30:05.000Z'), sender_id=1)
        late = cot_position.fix_from_cot(
            event('2026-09-13T14:31:05.000Z'), sender_id=1)
        self.assertEqual(late.fix_unix_s - early.fix_unix_s, 60)

    def test_an_unusable_time_stays_zero(self):
        """Zero is this format's word for unreported. Inventing `now` would
        date somebody else's fix by our clock."""
        for bad in ('', 'not-a-time', 'yesterday'):
            with self.subTest(stamp=bad):
                fix = cot_position.fix_from_cot(event(bad), sender_id=1)
                self.assertEqual(fix.fix_unix_s, 0)

    def test_a_missing_time_attribute_stays_zero(self):
        xml = (
            '<event version="2.0" uid="ANDROID-abc" type="a-f-G-U-C" how="m-g" '
            'start="2026-09-13T14:30:05.000Z" stale="2026-09-13T14:35:00.000Z">'
            '<point lat="41.0" lon="29.0" hae="30.0" ce="10.0" le="9999999.0"/>'
            '<detail/></event>'
        )
        fix = cot_position.fix_from_cot(xml, sender_id=1)
        self.assertEqual(fix.fix_unix_s, 0)

    def test_the_fix_still_carries_everything_it_did_before(self):
        fix = cot_position.fix_from_cot(event(), sender_id=0x01020304)
        self.assertEqual(fix.lat_e7, 410000000)
        self.assertEqual(fix.lon_e7, 290000000)
        self.assertEqual(fix.sender_id, 0x01020304)


if __name__ == "__main__":
    unittest.main()
