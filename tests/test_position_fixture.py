"""The position report's shared fixture, tests/fixtures/position_v2.json.

One wire format, read by three implementations: the firmware's codec
(PositionCodec.h, host-tested in test/test_firmware_core), the deck's decoder
(tools/position_codec.py), and the CoT gateway built on it. The fixture is the
contract: the Python codec is checked against it here, and every byte string in
it must appear in the C++ host test, so neither side can change the format
without the other's test failing.
"""

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import position_codec as pc  # noqa: E402

FIXTURE = json.loads((ROOT / "tests/fixtures/position_v2.json").read_text())
CPP_TEST = (ROOT / "test/test_firmware_core/test_firmware_core.cpp").read_text()


def fix_from(fields):
    return pc.PositionFix(**fields)


def fields_of(fix):
    return {name: getattr(fix, name) for name in pc.PositionFix.__slots__}


class PositionFixture(unittest.TestCase):

    def test_the_fixture_is_this_wire_version(self):
        self.assertEqual(FIXTURE["version"], pc.WIRE_VERSION)

    def test_python_encodes_every_case_to_the_pinned_bytes(self):
        for case in FIXTURE["encode"]:
            with self.subTest(case["name"]):
                self.assertEqual(pc.encode(fix_from(case["fix"])).hex(), case["hex"])

    def test_python_decodes_every_case_as_pinned(self):
        for case in FIXTURE["decode"]:
            with self.subTest(case["name"]):
                decoded = pc.decode(bytes.fromhex(case["hex"]))
                if case["fix"] is None:
                    self.assertIsNone(decoded)
                else:
                    self.assertEqual(fields_of(decoded), case["fix"])

    def test_the_firmware_host_test_pins_the_same_bytes(self):
        for case in FIXTURE["encode"] + FIXTURE["decode"]:
            with self.subTest(case["name"]):
                self.assertIn('"%s"' % case["hex"], CPP_TEST)


if __name__ == "__main__":
    unittest.main()
