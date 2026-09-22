"""A handset that reports every few minutes stays current between reports.

Columba reports its own position while ATAK is closed, every few minutes on
purpose, and states the interval in the report. Drawn with the one-minute
default, that track went grey between every pair of reports.
"""

import re
import sys
import unittest
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import position_codec                                  # noqa: E402
from cot_bridge import POSITION_STALE_SECONDS          # noqa: E402
from test_cot_position_order import bridge             # noqa: E402


def drawn_for(interval_min):
    made = bridge()
    fix = position_codec.PositionFix(
        lat_e7=410000000, lon_e7=290000000, fix_unix_s=1000,
        sender_id=0x11223344, interval_min=interval_min)
    made._position_from_mesh(position_codec.encode(fix))
    return made.drawn[0].decode("utf-8") if isinstance(made.drawn[0], bytes) else made.drawn[0]


def believed_for(xml):
    """Seconds between the event's time and its stale."""
    stamps = dict(re.findall(r'\b(time|stale)="([^"]+)"', xml))
    parse = lambda value: datetime.strptime(value[:19], "%Y-%m-%dT%H:%M:%S")
    return (parse(stamps["stale"]) - parse(stamps["time"])).total_seconds()


class StatedIntervalTests(unittest.TestCase):
    def test_a_report_that_states_no_interval_keeps_the_default(self):
        self.assertAlmostEqual(believed_for(drawn_for(0)), POSITION_STALE_SECONDS, delta=1)

    def test_a_five_minute_reporter_is_current_for_two_reports(self):
        self.assertAlmostEqual(believed_for(drawn_for(5)), 10 * 60, delta=1)

    def test_a_stated_interval_never_shortens_the_default(self):
        self.assertGreaterEqual(believed_for(drawn_for(1)), POSITION_STALE_SECONDS - 1)


if __name__ == "__main__":
    unittest.main()
