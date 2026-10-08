"""The telemetry batch's shared fixture, tests/fixtures/telemetry_batch_v1.json.

The firmware encodes batches (TelemetryBatchCodec.h, host-tested in
test/test_firmware_core); the gateway decodes them (tools/telemetry_batch_codec.py).
The fixture is the contract: the Python codec is checked against it here, and
every byte string in it must appear in the C++ host test.
"""

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import telemetry_batch_codec as bc  # noqa: E402
import telemetry_codec as tc  # noqa: E402
import telemetry_detail_codec as dc  # noqa: E402

FIXTURE = json.loads((ROOT / "tests/fixtures/telemetry_batch_v1.json").read_text())
CPP_TEST = (ROOT / "test/test_firmware_core/test_firmware_core.cpp").read_text()
CPP_CODEC = (ROOT / "TelemetryBatchCodec.h").read_text()


def entries_of(case):
    return [{"time_kind": e["time_kind"], "time": e["time"], "report": bytes.fromhex(e["report_hex"])}
            for e in case["entries"]]


class TelemetryBatchFixture(unittest.TestCase):

    def test_the_fixture_is_this_wire_version(self):
        self.assertEqual(FIXTURE["version"], bc.WIRE_VERSION)

    def test_python_encodes_every_case_to_the_pinned_bytes(self):
        for case in FIXTURE["encode"]:
            with self.subTest(case["name"]):
                out = bc.encode(case["sender_id"], case["composed_unix"], entries_of(case),
                                truncated=case["truncated"], out_len=case["out_len"])
                self.assertEqual(out.hex(), case["hex"])

    def test_python_decodes_every_case_as_pinned(self):
        for case in FIXTURE["decode"]:
            with self.subTest(case["name"]):
                got = bc.decode(bytes.fromhex(case["hex"]))
                if case["batch"] is None:
                    self.assertIsNone(got)
                    continue
                want = case["batch"]
                self.assertEqual(got["flags"], want["flags"])
                self.assertEqual(got["sender_id"], want["sender_id"])
                self.assertEqual(got["composed_unix"], want["composed_unix"])
                self.assertEqual([{"time_kind": e["time_kind"], "time": e["time"],
                                   "report_hex": e["report"].hex()} for e in got["entries"]],
                                 want["entries"])

    def test_every_entry_is_a_report_the_gateway_can_decode(self):
        for case in FIXTURE["encode"]:
            for e in entries_of(case):
                with self.subTest(case["name"]):
                    report = e["report"]
                    decoded = tc.decode(report) if report[0] == tc.WIRE_VERSION else dc.decode(report)
                    self.assertIsNotNone(decoded)

    def test_the_oldest_are_dropped_when_they_do_not_fit(self):
        case = next(c for c in FIXTURE["encode"] if c["name"] == "oldest_dropped_to_fit")
        got = bc.decode(bytes.fromhex(case["hex"]))
        self.assertTrue(got["truncated"])
        self.assertEqual([e["time"] for e in got["entries"]], [e["time"] for e in case["entries"][1:]])

    def test_entry_times(self):
        absolute = bc.decode(bytes.fromhex(FIXTURE["encode"][0]["hex"]))
        self.assertEqual(bc.entry_times(absolute, 0), [(1791096400, True), (1791098200, True)])
        relative = bc.decode(bytes.fromhex(FIXTURE["encode"][1]["hex"]))
        self.assertEqual(bc.entry_times(relative, 1000000), [(996400, False), (998200, False), (1000000, False)])
        composed = dict(relative, composed_unix=2000000)
        self.assertEqual(bc.entry_times(composed, 0)[0], (1996400, True))

    def test_the_firmware_host_test_pins_the_same_bytes(self):
        for case in FIXTURE["encode"] + FIXTURE["decode"]:
            with self.subTest(case["name"]):
                self.assertIn('"%s"' % case["hex"], CPP_TEST)

    def test_the_constants_agree_with_the_firmware(self):
        for name, value in (("TELEMETRY_BATCH_WIRE_VERSION", bc.WIRE_VERSION),
                            ("TELEMETRY_BATCH_MAX_LEN", bc.MAX_LEN),
                            ("TELEMETRY_BATCH_HEADER_LEN", bc.HEADER_LEN),
                            ("TELEMETRY_BATCH_ENTRY_HEADER", bc.ENTRY_HEADER),
                            ("TELEMETRY_BATCH_ENTRY_MAX", bc.ENTRY_MAX),
                            ("BATCH_FLAG_TRUNCATED", bc.FLAG_TRUNCATED),
                            ("BATCH_TIME_ABSOLUTE", bc.TIME_ABSOLUTE)):
            with self.subTest(name):
                self.assertRegex(CPP_CODEC, r"#define %s\s+(0x0*%x|%d)\b" % (name, value, value))


if __name__ == "__main__":
    unittest.main()
