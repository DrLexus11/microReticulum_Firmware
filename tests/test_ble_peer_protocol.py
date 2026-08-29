"""Contract tests for BLEPeerProtocol.h against the ble-reticulum reference.

The node and the phone must agree byte for byte on the BLE peer wire format.
A disagreement does not announce itself: fragments are silently discarded or
reassembled into garbage, and the symptom is a peer that connects and then
never exchanges anything -- which is precisely the failure this project spent
a day chasing on the RNode path.

These read the constants out of the firmware header and assert them against
Columba's BleConstants.kt, which is the client this firmware actually has to
talk to and is checked out locally. An earlier version of this file gated its
cross-checks on an upstream BLEFragmentation.py that never downloaded (the URL
404'd), so those tests skipped silently and the LONE fragment type went unnoticed
until hardware showed packets arriving and being dropped.
"""

import os
import re
import struct
import unittest

HEADER = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "BLEPeerProtocol.h")
REFERENCE = os.environ.get("BLE_REFERENCE", "")

# The Kotlin client, checked out beside this repo. Overridable, and skipped
# rather than failed when the checkout is not present.
COLUMBA = os.environ.get("COLUMBA_BLE_CONSTANTS", os.path.expanduser(
    "~/projects/columba/rns-host/src/main/kotlin/network/columba/app/rns/host/"
    "ble/model/BleConstants.kt"))


def firmware_defines():
    with open(HEADER, "r", encoding="utf-8") as handle:
        text = handle.read()
    out = {}
    for name, value in re.findall(r'^#define\s+(\w+)\s+(.+?)\s*$', text, re.M):
        value = value.split("//")[0].strip()
        if value.startswith('"') and value.endswith('"'):
            out[name] = value[1:-1]
        else:
            try:
                out[name] = int(value, 0)
            except ValueError:
                pass
    return out


class WireFormatTests(unittest.TestCase):
    def setUp(self):
        self.d = firmware_defines()

    def test_header_is_five_bytes_big_endian(self):
        """struct.pack("!BHH", ...) -- one byte and two network-order shorts."""
        self.assertEqual(5, self.d["BLE_PEER_HEADER_SIZE"])
        self.assertEqual(5, len(struct.pack("!BHH", 1, 0, 1)))

    def test_fragment_type_values(self):
        self.assertEqual(0x01, self.d["BLE_PEER_TYPE_START"])
        self.assertEqual(0x02, self.d["BLE_PEER_TYPE_CONTINUE"])
        self.assertEqual(0x03, self.d["BLE_PEER_TYPE_END"])

    def test_single_fragment_packet_is_sent_as_START(self):
        """Measured on the wire: the peer sends 01 0000 0001 for a lone packet.

        BleConstants.kt declares FRAGMENT_TYPE_LONE = 0x00, but nothing in the
        client emits it and its reassembler drops unrecognised types. Sending
        LONE therefore made every outbound packet disappear while inbound
        continued to work -- announces reached the mesh, nothing came back.

        We still ACCEPT LONE inbound, defensively; we never SEND it.
        """
        self.assertEqual(b"\x01\x00\x00\x00\x01",
                         struct.pack("!BHH", self.d["BLE_PEER_TYPE_START"], 0, 1))
        source = open(os.path.join(os.path.dirname(HEADER),
                                   "BLEPeerInterface.h"), encoding="utf-8").read()
        emit = source[source.index("const uint8_t type = "):]
        emit = emit[:emit.index(";")]
        self.assertNotIn("BLE_PEER_TYPE_LONE", emit,
                         "LONE must never be emitted; the peer drops it")
        self.assertIn("BLE_PEER_TYPE_LONE", source,
                      "LONE must still be accepted on receive")

    def test_lone_type_is_distinct_from_the_others(self):
        types = [self.d["BLE_PEER_TYPE_" + n]
                 for n in ("LONE", "START", "CONTINUE", "END")]
        self.assertEqual(len(types), len(set(types)))

    def test_header_encoding_matches_struct(self):
        for seq, total in ((0, 1), (1, 2), (255, 256), (65534, 65535)):
            with self.subTest(seq=seq, total=total):
                self.assertEqual(
                    struct.pack("!BHH", self.d["BLE_PEER_TYPE_CONTINUE"], seq, total),
                    bytes([self.d["BLE_PEER_TYPE_CONTINUE"],
                           seq >> 8, seq & 0xFF, total >> 8, total & 0xFF]))

    def test_keepalive_is_one_zero_byte_and_not_a_fragment(self):
        """Observed on the wire from Columba every 15s: a single 0x00.

        It is shorter than a header, so length alone distinguishes it. Parsing
        it as a fragment made a healthy link report continuous packet loss.
        """
        self.assertEqual(1, self.d["BLE_PEER_KEEPALIVE_SIZE"])
        self.assertEqual(0x00, self.d["BLE_PEER_KEEPALIVE_BYTE"])
        self.assertLess(self.d["BLE_PEER_KEEPALIVE_SIZE"],
                        self.d["BLE_PEER_HEADER_SIZE"])

    def test_keepalive_is_not_confusable_with_identity_or_fragment(self):
        for other in ("BLE_PEER_IDENTITY_SIZE", "BLE_PEER_HEADER_SIZE"):
            self.assertNotEqual(self.d["BLE_PEER_KEEPALIVE_SIZE"], self.d[other])

    def test_identity_handshake_is_an_rx_write_disambiguated_by_state(self):
        """Central writes its 16-byte identity to the peripheral's RX.

        From Columba's docs/ble-architecture.md: the peripheral detects the
        handshake as "exactly 16 bytes AND no existing identity" for that peer.
        Length alone is NOT sufficient -- a fragment of a 5-byte header plus an
        11-byte payload is also 16 bytes -- so the receiver must gate on not
        yet knowing the peer's identity.

        This matters because the peer spawns its per-peer Reticulum interface
        only once the handshake completes. Skip it and the link stays up and
        healthy while carrying nothing.
        """
        self.assertEqual(16, self.d["BLE_PEER_IDENTITY_SIZE"])
        self.assertEqual(self.d["BLE_PEER_IDENTITY_SIZE"],
                         self.d["BLE_PEER_HEADER_SIZE"] + 11,
                         "16 bytes is also a valid fragment, hence the state gate")

        source = open(os.path.join(os.path.dirname(HEADER),
                                   "BLEPeerInterface.h"), encoding="utf-8").read()
        self.assertIn("_peer_identity.size() == 0", source,
                      "the 16-byte check must be gated on not knowing the identity")
        # Identity goes central -> peripheral on RX. It is never a TX notify:
        # the peer feeds every notification straight to its defragmenter.
        self.assertNotIn("notify_raw(_identity_hash.data()", source,
                         "identity must be written to RX, not notified on TX")
        self.assertIn("_remote_rx->writeValue((uint8_t*)_identity_hash.data()", source,
                      "the central half of the handshake must write to peer RX")

    def test_service_and_characteristic_uuids(self):
        base = "37145b00-442d-4a94-917f-8f42c5da28e"
        self.assertEqual(base + "3", self.d["BLE_PEER_SERVICE_UUID"])
        self.assertEqual(base + "4", self.d["BLE_PEER_TX_UUID"])
        self.assertEqual(base + "5", self.d["BLE_PEER_RX_UUID"])
        self.assertEqual(base + "6", self.d["BLE_PEER_IDENTITY_UUID"])

    def test_mtu_bounds_match_the_reference(self):
        self.assertEqual(3,   self.d["BLE_PEER_ATT_HEADER"])
        self.assertEqual(23,  self.d["BLE_PEER_MIN_MTU"])
        self.assertEqual(517, self.d["BLE_PEER_MAX_MTU"])
        self.assertEqual(512, self.d["BLE_PEER_MAX_ATTR"])

    def test_bitrate_is_declared_and_non_zero(self):
        """RNS divides by the interface bitrate.

        Transport::extra_link_proof_timeout() is (1.0/bitrate)*8*MTU, so a zero
        bitrate yields an infinite link establishment deadline and no link ever
        completes -- while packets still cross and the interface looks healthy.
        """
        self.assertIn("BLE_PEER_BITRATE", self.d)
        self.assertGreater(self.d["BLE_PEER_BITRATE"], 0)
        source = open(os.path.join(os.path.dirname(HEADER),
                                   "BLEPeerInterface.h"), encoding="utf-8").read()
        self.assertIn("_bitrate = BLE_PEER_BITRATE", source)

    def test_keepalive_is_inside_the_android_idle_timeout(self):
        """Android drops idle BLE links at 20-30s; both references use 15s."""
        self.assertEqual(15000, self.d["BLE_PEER_KEEPALIVE_MS"])
        self.assertLess(self.d["BLE_PEER_KEEPALIVE_MS"], 20000)


@unittest.skipUnless(os.path.exists(COLUMBA),
                     "Columba checkout not present; set COLUMBA_BLE_CONSTANTS")
class AgainstColumbaClientTests(unittest.TestCase):
    """Assert against the peer's own constants, so a client change fails here."""

    def setUp(self):
        with open(COLUMBA, "r", encoding="utf-8") as handle:
            self.source = handle.read()
        self.d = firmware_defines()

    def kotlin_const(self, name):
        match = re.search(rf"{name}\s*(?::\s*\w+)?\s*=\s*(0x[0-9a-fA-F]+|\d+)", self.source)
        self.assertIsNotNone(match, "%s missing from BleConstants.kt" % name)
        return int(match.group(1), 0)

    def test_all_four_fragment_types_match(self):
        """LONE is the one that bit us: a lone packet is 0x00, not START."""
        for kt, ours in (("FRAGMENT_TYPE_LONE", "BLE_PEER_TYPE_LONE"),
                         ("FRAGMENT_TYPE_START", "BLE_PEER_TYPE_START"),
                         ("FRAGMENT_TYPE_CONTINUE", "BLE_PEER_TYPE_CONTINUE"),
                         ("FRAGMENT_TYPE_END", "BLE_PEER_TYPE_END")):
            self.assertEqual(self.kotlin_const(kt), self.d[ours], ours)

    def test_header_size_matches(self):
        self.assertEqual(self.kotlin_const("FRAGMENT_HEADER_SIZE"),
                         self.d["BLE_PEER_HEADER_SIZE"])

    def test_service_and_characteristic_uuids_match(self):
        for kt, ours in (("SERVICE_UUID", "BLE_PEER_SERVICE_UUID"),
                         ("CHARACTERISTIC_RX_UUID", "BLE_PEER_RX_UUID"),
                         ("CHARACTERISTIC_TX_UUID", "BLE_PEER_TX_UUID"),
                         ("CHARACTERISTIC_IDENTITY_UUID", "BLE_PEER_IDENTITY_UUID")):
            match = re.search(rf'val {kt}: UUID = UUID.fromString\("([0-9a-f-]+)"\)',
                              self.source)
            self.assertIsNotNone(match, kt)
            self.assertEqual(match.group(1).lower(), self.d[ours].lower(), ours)

    def test_att_header_overhead_matches(self):
        """Both sides must subtract the same 3 bytes, or fragments overrun."""
        self.assertEqual(self.kotlin_const("ATT_HEADER_SIZE"), 3)


if __name__ == "__main__":
    unittest.main()
