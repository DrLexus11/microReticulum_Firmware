"""The board detail report's shared fixture, tests/fixtures/telemetry_detail_v1.json.

The firmware encodes (TelemetryDetailCodec.h, host-tested in
test/test_firmware_core); the gateway decodes (tools/telemetry_detail_codec.py).
The fixture is the contract: the Python codec is checked against it here, and
every byte string in it must appear in the C++ host test.
"""

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import telemetry_detail_codec as dc  # noqa: E402

FIXTURE = json.loads((ROOT / "tests/fixtures/telemetry_detail_v1.json").read_text())
CPP_TEST = (ROOT / "test/test_firmware_core/test_firmware_core.cpp").read_text()
CPP_CODEC = (ROOT / "TelemetryDetailCodec.h").read_text()


class TelemetryDetailFixture(unittest.TestCase):

    def test_the_fixture_is_this_wire_version(self):
        self.assertEqual(FIXTURE["version"], dc.WIRE_VERSION)

    def test_python_encodes_every_case_to_the_pinned_bytes(self):
        for case in FIXTURE["encode"]:
            with self.subTest(case["name"]):
                self.assertEqual(dc.encode(case["detail"]).hex(), case["hex"])

    def test_python_decodes_every_case_as_pinned(self):
        for case in FIXTURE["decode"]:
            with self.subTest(case["name"]):
                self.assertEqual(dc.decode(bytes.fromhex(case["hex"])), case["detail"])

    def test_every_report_fits_one_encrypted_packet(self):
        for case in FIXTURE["encode"]:
            with self.subTest(case["name"]):
                self.assertLessEqual(len(case["hex"]) // 2, dc.WIRE_MAX_LEN)

    def test_the_firmware_host_test_pins_the_same_bytes(self):
        for case in FIXTURE["encode"] + FIXTURE["decode"]:
            with self.subTest(case["name"]):
                self.assertIn('"%s"' % case["hex"], CPP_TEST)

    def test_the_constants_agree_with_the_firmware(self):
        for name, value in (("TELEMETRY_DETAIL_WIRE_VERSION", dc.WIRE_VERSION),
                            ("TELEMETRY_DETAIL_WIRE_MAX_LEN", dc.WIRE_MAX_LEN),
                            ("DETAIL_ENV_MAX", dc.ENV_MAX),
                            ("DETAIL_MAX_INTERFACES", dc.MAX_INTERFACES),
                            ("DETAIL_MAX_NEIGHBOURS", dc.MAX_NEIGHBOURS),
                            ("DETAIL_FLAG_NEIGHBOURS_TRUNCATED", dc.FLAG_NEIGHBOURS_TRUNCATED),
                            ("DETAIL_IF_HALOW", 9)):
            with self.subTest(name):
                self.assertRegex(CPP_CODEC, r"#define %s\s+(0x0*%x|%d)\b" % (name, value, value))


if __name__ == "__main__":
    unittest.main()
