"""The BLE soak harness reads the lines Columba and Android actually write.

Line shapes are copied from the A54's and the Nexus's logs, 2026-09-26/27.
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import ble_link_soak  # noqa: E402

LINES = [
    "09-27 10:40:00.001 I/python.stdout( 3888): [2026-09-27 10:40:00] [Info]     BLEInterface[Bluetooth LE] "
    "peers: 60ba5291[RNode 1114]=online, 53ccf1db[BLE-8E:37:28]=offline detach-in=97s",
    "09-27 10:40:30.001 I/python.stdout( 3888): [2026-09-27 10:40:30] [Info]     BLEInterface[Bluetooth LE] "
    "peers: 60ba5291[RNode 1114]=online, 53ccf1db[BLE-8E:37:28]=online",
    "09-26 16:51:19.598 I/python.stdout( 3888): [2026-09-26 16:51:19] [Info]     BLEInterface[Bluetooth LE] "
    "created peer interface for RNode 1114 (60ba5291), type=central",
    "09-26 16:51:34.483 I/python.stdout( 3888): [2026-09-26 16:51:34] [Debug]    BLEInterface[Bluetooth LE] "
    "scheduled detach for 60ba5291 in 120s",
    "09-26 16:51:28.525 D/Columba:BLE:K:Bridge( 3888): Not connecting to 63:EF:6D:C3:68:05: PEER_INITIATES",
    "09-26 16:56:32.738 W/Columba:BLE:K:Server( 3888): Rejecting pre-identity write from 67:16:8E:08:AC:19: len=1",
    "09-26 16:55:26.722 D/BluetoothGatt( 3888): onClientConnectionState() - status=133 clientIf=71 "
    "connected=false device=XX:XX:XX:XX:4B:71",
    "09-26 16:55:45.067 D/Columba:BLE:K:Client( 3888): Disconnected from 6F:9E:B4:8E:4B:71: Error (status: 8)",
    "09-26 16:51:59.594 D/Columba:BLE:K:Client( 3888): Disconnected from 70:B9:87:05:C0:8D (manual)",
    "09-26 16:52:10.662 D/BluetoothGatt(26353): onConnectionUpdated() - Device=80:B5:4E:F4:C7:A5 "
    "interval=12 latency=0 timeout=2000 status=0",
    "09-26 16:28:19.954 D/BluetoothGatt(26313): onConnectionUpdated() - Device=XX:XX:XX:XX:C9:4A "
    "interval=0 latency=0 timeout=0 status=30",
    "09-26 16:30:06.351 I/python.stdout(24294): [2026-09-26 16:30:06] [Debug]    "
    "BLEPeerInterface[BLE-9A:36:59] RX: 483 bytes from BLE-9A:36:59",
    "09-26 16:30:06.202 I/python.stdout(24294): [2026-09-26 16:30:06] [Debug]    "
    "BLEPeerInterface[BLE-9A:36:59] TX: 131 bytes to BLE-9A:36:59",
    "09-26 16:30:09.269 I/TakFileTransfers(24266): 20260926_162737.jpg.zip: 16384 of 3491288 bytes, "
    "last part in 6585ms (14928 bit/s); the rest ~1862s",
]


class SummaryTests(unittest.TestCase):
    def setUp(self):
        self.summary = ble_link_soak.summarise(LINES)

    def test_a_peer_that_drops_out_of_the_heartbeats_is_counted_offline(self):
        # Online in one beat, then detached and no longer listed: 1 of 3, not 100%.
        summary = ble_link_soak.summarise([
            "BLEInterface[Bluetooth LE] peers: 60ba5291[RNode 1114]=online",
            "BLEInterface[Bluetooth LE] peers: none",
            "BLEInterface[Bluetooth LE] peers: none",
        ])
        self.assertEqual(summary["peers"]["60ba5291"]["heartbeats"], 3)
        self.assertEqual(summary["peers"]["60ba5291"]["online"], 1)

    def test_a_peer_met_late_is_not_charged_for_before_it_was_met(self):
        summary = ble_link_soak.summarise([
            "BLEInterface[Bluetooth LE] peers: none",
            "BLEInterface[Bluetooth LE] peers: 60ba5291[RNode 1114]=online",
        ])
        self.assertEqual(summary["peers"]["60ba5291"]["heartbeats"], 1)

    def test_online_share_comes_from_the_heartbeats(self):
        self.assertEqual(self.summary["heartbeats"], 2)
        self.assertEqual(self.summary["peers"]["60ba5291"],
                         {"name": "RNode 1114", "heartbeats": 2, "listed": 2, "online": 2})
        self.assertEqual(self.summary["peers"]["53ccf1db"]["online"], 1)

    def test_link_events_are_counted(self):
        events = self.summary["events"]
        self.assertEqual(events["peer interface created"], 1)
        self.assertEqual(events["detach scheduled"], 1)
        self.assertEqual(events["declined: peer initiates"], 1)
        self.assertEqual(events["write refused before identity"], 1)

    def test_failures_by_status(self):
        self.assertEqual(self.summary["gatt_failures"], {"133": 1})
        self.assertEqual(self.summary["disconnects"], {"status 8": 1, "manual": 1})

    def test_only_intervals_actually_negotiated(self):
        """A failed update reports interval=0 and is not an interval."""
        self.assertEqual(self.summary["intervals"], {"15.00 ms, timeout 20000 ms": 1})

    def test_bytes_and_part_rates(self):
        self.assertEqual(self.summary["carried"], {"BLE-9A:36:59": {"RX": 483, "TX": 131}})
        self.assertEqual(self.summary["part_rates_bps"], [14928])


if __name__ == "__main__":
    unittest.main()
