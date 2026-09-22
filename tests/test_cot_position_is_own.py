"""Only ATAK's own report is this node's position.

A friendly unit marker has a position-shaped type (a-f-G-U-C) just as ATAK's
self-report does. Routed as position, it moved the operator's own track to
wherever they dropped the marker. The self-report carries <takv>; a marker
never does, which is the same test the pipeline uses to learn ATAK's uid.
"""

import sys
import unittest
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from cot_bridge import CotBridge      # noqa: E402

POINT = '<point lat="41.0151234" lon="28.9791234" hae="12" ce="10" le="10"/>'
SELF_REPORT = ('<event version="2.0" uid="ANDROID-7b97a84747a7a79d" type="a-f-G-U-C" how="m-g" '
               'time="2026-09-22T10:30:00Z" start="2026-09-22T10:30:00Z" stale="2026-09-22T10:32:00Z">'
               + POINT + '<detail><takv platform="ATAK-CIV" version="5.8"/>'
               '<contact callsign="IDLER"/></detail></event>')
FRIENDLY_MARKER = ('<event version="2.0" uid="c4f2e1a0-1111-2222-3333-444455556666" type="a-f-G-U-C" '
                   'how="h-g-i-g-o" time="2026-09-22T10:30:00Z" start="2026-09-22T10:30:00Z" '
                   'stale="2026-09-23T10:30:00Z">' + POINT
                   + '<detail><contact callsign="Rescue 2"/></detail></event>')


def bridge():
    made = CotBridge.__new__(CotBridge)
    made.pipeline = Mock()
    made.pipeline.is_echo.return_value = False
    made.pipeline.atak_uid = "ANDROID-7b97a84747a7a79d"
    made._position_from_atak = Mock(return_value=True)
    made._chat_from_atak = Mock(return_value=False)
    made._marker_from_atak = Mock(return_value=True)
    return made


class OwnReportTests(unittest.TestCase):
    def test_atak_s_self_report_is_sent_as_position(self):
        made = bridge()
        made._from_atak(SELF_REPORT)
        made._position_from_atak.assert_called_once()
        made._marker_from_atak.assert_not_called()

    def test_a_friendly_unit_marker_is_a_marker_not_our_position(self):
        made = bridge()
        made._from_atak(FRIENDLY_MARKER)
        made._position_from_atak.assert_not_called()
        made._marker_from_atak.assert_called_once()


if __name__ == "__main__":
    unittest.main()
