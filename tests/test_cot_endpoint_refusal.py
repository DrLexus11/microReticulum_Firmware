"""What happens to an event that cannot go, and whether anyone is told.

From ATAK, an event refused for being too big looks exactly like the feature
not working: a range-and-bearing line is drawn, nothing crosses, and nothing
anywhere says why. Observed 2026-09-13. Tier 3 is what would carry it, and
until that exists the least this can do is say so.
"""

import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cot_tier2 as tier2            # noqa: E402
from cot_endpoint import CotOutbound  # noqa: E402


def oversized_event():
    """A well-formed event that cannot fit one packet.

    Shaped like ATAK's range and bearing: a real type, real attributes, and a
    payload that will not deflate away. Repeated text is the wrong fixture here
    -- forty copies of one sentence compress to almost nothing and sail under
    the bound, which says more about deflate than about the limit under test.
    """
    import hashlib
    filler = "".join(
        hashlib.sha256(str(index).encode()).hexdigest() for index in range(24))
    remarks = "R and B " + filler
    return (
        '<event version="2.0" uid="rb-1" type="u-rb-a" how="h-e" '
        'time="2026-09-13T10:00:00.000Z" start="2026-09-13T10:00:00.000Z" '
        'stale="2026-09-13T11:00:00.000Z">'
        '<point lat="41.0" lon="29.0" hae="0" ce="9999999" le="9999999"/>'
        '<detail><remarks>%s</remarks></detail></event>' % remarks
    )


class RefusalTests(unittest.TestCase):
    def test_an_oversized_event_is_refused_rather_than_truncated(self):
        with self.assertRaises(ValueError):
            tier2.encode(oversized_event())

    def test_the_refusal_says_it_is_a_size_bound(self):
        """An operator reading this should know it is not the radio, and
        should know which tier would carry it."""
        try:
            tier2.encode(oversized_event())
        except ValueError as error:
            self.assertIn("tier 3", str(error))
            self.assertIn("383", str(error))

    def test_the_endpoint_reports_the_reason_not_only_a_count(self):
        stream = CotOutbound("urtn-ourselves")
        out = io.StringIO()
        with redirect_stdout(out):
            frame = stream.frame(oversized_event(), tier2.encode)
        self.assertIsNone(frame)
        self.assertEqual(stream.dropped, 1)
        self.assertIn("not sent", out.getvalue())
        self.assertIn("tier 3", out.getvalue())
        self.assertIn("tier 3", stream.last_drop)

    def test_an_event_that_fits_is_untouched(self):
        stream = CotOutbound("urtn-ourselves")
        small = (
            '<event version="2.0" uid="m-1" type="a-f-G-U-C" how="m-g" '
            'time="2026-09-13T10:00:00.000Z" start="2026-09-13T10:00:00.000Z" '
            'stale="2026-09-13T10:05:00.000Z">'
            '<point lat="41.0" lon="29.0" hae="0" ce="9999999" le="9999999"/>'
            '<detail/></event>'
        )
        out = io.StringIO()
        with redirect_stdout(out):
            frame = stream.frame(small, tier2.encode)
        self.assertIsNotNone(frame)
        self.assertEqual(stream.dropped, 0)
        self.assertEqual(out.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
