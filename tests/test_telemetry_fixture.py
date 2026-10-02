"""The board health report's shared fixture, tests/fixtures/telemetry_v1.json.

The firmware encodes (TelemetryCodec.h, host-tested in test/test_firmware_core);
the gateway decodes (tools/telemetry_codec.py). The fixture is the contract
between them: the Python codec is checked against it here, and every byte
string in it must appear in the C++ host test.
"""

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import telemetry_codec as tc  # noqa: E402

FIXTURE = json.loads((ROOT / "tests/fixtures/telemetry_v1.json").read_text())
CPP_TEST = (ROOT / "test/test_firmware_core/test_firmware_core.cpp").read_text()
CPP_CODEC = (ROOT / "TelemetryCodec.h").read_text()


def fields_of(t):
    return {name: getattr(t, name) for name in tc.Telemetry.__slots__}


class TelemetryFixture(unittest.TestCase):

    def test_the_fixture_is_this_wire_version(self):
        self.assertEqual(FIXTURE["version"], tc.WIRE_VERSION)

    def test_python_encodes_every_case_to_the_pinned_bytes(self):
        for case in FIXTURE["encode"]:
            with self.subTest(case["name"]):
                self.assertEqual(tc.encode(tc.Telemetry(**case["telemetry"])).hex(), case["hex"])

    def test_python_decodes_every_case_as_pinned(self):
        for case in FIXTURE["decode"]:
            with self.subTest(case["name"]):
                decoded = tc.decode(bytes.fromhex(case["hex"]))
                if case["telemetry"] is None:
                    self.assertIsNone(decoded)
                else:
                    self.assertEqual(fields_of(decoded), case["telemetry"])

    def test_the_firmware_host_test_pins_the_same_bytes(self):
        for case in FIXTURE["encode"] + FIXTURE["decode"]:
            with self.subTest(case["name"]):
                self.assertIn('"%s"' % case["hex"], CPP_TEST)

    def test_the_constants_agree_with_the_firmware(self):
        for name, value in (("TELEMETRY_WIRE_VERSION", tc.WIRE_VERSION),
                            ("TELEMETRY_WIRE_BASE_LEN", tc.WIRE_BASE_LEN),
                            ("TELEMETRY_WIRE_MAX_LEN", tc.WIRE_MAX_LEN),
                            ("TELEMETRY_FLAG_BATTERY", tc.FLAG_BATTERY),
                            ("TELEMETRY_IF_ESPNOW", tc.IF_ESPNOW)):
            with self.subTest(name):
                self.assertRegex(CPP_CODEC, r"#define %s\s+(0x0*%x|%d)\b" % (name, value, value))


if __name__ == "__main__":
    unittest.main()
