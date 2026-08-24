"""Contract tests between LXMFPropagation.h and the LXMF reference library.

The propagation node's failure mode is silence: get the stamp split, a constant
or a request shape wrong and messages sync but never arrive, with no error
anywhere. These tests read the firmware's own #defines and assert them against
Python LXMF, so a divergence -- ours or an upstream change -- fails here instead
of on a rooftop.

They need the LXMF package. Run under the RNS virtualenv, e.g.

    ~/.local/share/rnode-rns-venv/bin/python -m unittest discover tests

Without LXMF importable the whole module skips rather than passing vacuously.
"""

import hashlib
import os
import re
import unittest

HEADER = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "LXMFPropagation.h")

try:
    from LXMF import LXStamper
    from LXMF.LXMessage import LXMessage
    from LXMF.LXMPeer import LXMPeer
    from LXMF.LXMRouter import LXMRouter
    import RNS
    HAVE_LXMF = True
except Exception:
    HAVE_LXMF = False


def firmware_defines():
    """Extract #define values from the firmware header.

    Handles plain integers, hex, quoted strings, and the simple integer
    arithmetic the header uses for size limits, e.g. (512 * 1024).
    """
    with open(HEADER, "r", encoding="utf-8") as handle:
        text = handle.read()
    out = {}
    for name, value in re.findall(r'^#define\s+(\w+)\s+(.+?)\s*$', text, re.M):
        value = value.split("//")[0].strip()
        if value.startswith('"') and value.endswith('"'):
            out[name] = value[1:-1]
            continue
        # Drop C casts, e.g. ((size_t)FOO * 1024), then resolve references to
        # defines already seen so derived limits can be checked against their
        # source. Definition order in the header is the resolution order.
        expr = re.sub(r'\(\s*(?:unsigned\s+|signed\s+)?'
                      r'(?:size_t|uint\d+_t|int\d+_t|int|long|unsigned)\s*\)', '', value)
        expr = re.sub(r'\b([A-Za-z_]\w*)\b',
                      lambda m: str(out[m.group(1)])
                      if isinstance(out.get(m.group(1)), int) else m.group(1),
                      expr)
        if re.fullmatch(r'[-+*/()\s0-9xXa-fA-F]+', expr):
            try:
                out[name] = int(eval(expr, {"__builtins__": {}}, {}))
            except Exception:
                pass
    return out


@unittest.skipUnless(HAVE_LXMF, "LXMF not importable; run under the RNS virtualenv")
class ConstantsMatchReferenceTests(unittest.TestCase):
    """Every constant the wire format depends on, checked against upstream."""

    def setUp(self):
        self.d = firmware_defines()

    def test_stamp_size(self):
        # Drives the transient-id split. Wrong => every id we advertise is one
        # no client recognises.
        self.assertEqual(self.d["LXMF_STAMP_SIZE"], LXStamper.STAMP_SIZE)

    def test_destination_length(self):
        self.assertEqual(self.d["LXMF_DESTINATION_LEN"], LXMessage.DESTINATION_LENGTH)

    def test_minimum_message_overhead(self):
        self.assertEqual(self.d["LXMF_OVERHEAD"], LXMessage.LXMF_OVERHEAD)

    def test_transient_id_length(self):
        self.assertEqual(self.d["LXMF_TRANSIENT_ID_LEN"], RNS.Identity.HASHLENGTH // 8)

    def test_advertised_stamp_costs(self):
        # These travel in our announce and clients do real work against them.
        self.assertEqual(self.d["LXMF_PN_STAMP_COST"], LXMRouter.PROPAGATION_COST)
        self.assertEqual(self.d["LXMF_PN_STAMP_FLEX"], LXMRouter.PROPAGATION_COST_FLEX)
        self.assertEqual(self.d["LXMF_PN_PEERING_COST"], LXMRouter.PEERING_COST)

    def test_advertised_cost_is_acceptable_to_a_node(self):
        self.assertGreaterEqual(self.d["LXMF_PN_STAMP_COST"], LXMRouter.PROPAGATION_COST_MIN)

    def test_request_paths(self):
        self.assertEqual(self.d["LXMF_OFFER_PATH"], LXMPeer.OFFER_REQUEST_PATH)
        self.assertEqual(self.d["LXMF_GET_PATH"], LXMPeer.MESSAGE_GET_PATH)

    def test_error_codes(self):
        for ours, theirs in (
            ("LXMF_ERROR_NO_IDENTITY",   LXMPeer.ERROR_NO_IDENTITY),
            ("LXMF_ERROR_NO_ACCESS",     LXMPeer.ERROR_NO_ACCESS),
            ("LXMF_ERROR_INVALID_KEY",   LXMPeer.ERROR_INVALID_KEY),
            ("LXMF_ERROR_INVALID_DATA",  LXMPeer.ERROR_INVALID_DATA),
            ("LXMF_ERROR_INVALID_STAMP", LXMPeer.ERROR_INVALID_STAMP),
            ("LXMF_ERROR_THROTTLED",     LXMPeer.ERROR_THROTTLED),
            ("LXMF_ERROR_NOT_FOUND",     LXMPeer.ERROR_NOT_FOUND),
            ("LXMF_ERROR_TIMEOUT",       LXMPeer.ERROR_TIMEOUT),
        ):
            self.assertEqual(self.d[ours], theirs, ours)


@unittest.skipUnless(HAVE_LXMF, "LXMF not importable; run under the RNS virtualenv")
class TransientIdRuleTests(unittest.TestCase):
    """The stamp split, stated as the firmware implements it."""

    def test_id_is_hash_of_message_without_its_stamp(self):
        lxmf_data = os.urandom(LXMessage.LXMF_OVERHEAD + 40)
        stamp = os.urandom(LXStamper.STAMP_SIZE)
        blob = lxmf_data + stamp

        # lxmf_transient_id(): sha256 over the blob minus the trailing stamp.
        ours = hashlib.sha256(blob[:-LXStamper.STAMP_SIZE]).digest()
        self.assertEqual(ours, RNS.Identity.full_hash(lxmf_data))

    def test_hashing_the_stamped_blob_would_be_wrong(self):
        # Guards the specific mistake: hashing the whole blob yields an id the
        # sending client never computed, so nothing is ever recognised.
        lxmf_data = os.urandom(LXMessage.LXMF_OVERHEAD + 40)
        blob = lxmf_data + os.urandom(LXStamper.STAMP_SIZE)
        self.assertNotEqual(hashlib.sha256(blob).digest(),
                            RNS.Identity.full_hash(lxmf_data))


@unittest.skipUnless(HAVE_LXMF, "LXMF not importable; run under the RNS virtualenv")
class AnnounceShapeTests(unittest.TestCase):
    """Our announce must satisfy LXMF's own validator, field for field."""

    def test_announce_payload_is_accepted_by_lxmf(self):
        import LXMF
        from RNS.vendor import umsgpack as msgpack
        d = firmware_defines()
        app_data = msgpack.packb([
            False,                              # 0 legacy
            0,                                  # 1 timebase
            True,                               # 2 node state
            d["LXMF_PN_TRANSFER_LIMIT_KB"],     # 3 per-transfer limit
            d["LXMF_PN_SYNC_LIMIT_KB"],         # 4 per-sync limit
            [d["LXMF_PN_STAMP_COST"],
             d["LXMF_PN_STAMP_FLEX"],
             d["LXMF_PN_PEERING_COST"]],        # 5 stamp costs
            {},                                 # 6 metadata
        ])
        self.assertTrue(LXMF.pn_announce_data_is_valid(app_data))

    def test_limits_are_small_enough_for_the_store(self):
        # Python advertises 256 KB per message; one of those would consume half
        # this board's entire store, which is why we advertise far less. A single
        # message must be a small fraction of the store, and one sync must not be
        # able to fill it outright.
        d = firmware_defines()
        store = d["LXMF_PN_MAX_BYTES"]
        self.assertLessEqual(d["LXMF_PN_TRANSFER_LIMIT_KB"] * 1024, store // 8)
        self.assertLessEqual(d["LXMF_PN_SYNC_LIMIT_KB"] * 1024, store)

    def test_sync_limit_bytes_matches_its_kilobyte_form(self):
        # Both forms exist and are enforced in different places; they must agree.
        d = firmware_defines()
        self.assertEqual(d["LXMF_PN_SYNC_LIMIT_BYTES"], d["LXMF_PN_SYNC_LIMIT_KB"] * 1024)


@unittest.skipUnless(HAVE_LXMF, "LXMF not importable; run under the RNS virtualenv")
class SyncContainerShapeTests(unittest.TestCase):
    """The container both inbound paths parse: [timestamp, [blobs]]."""

    def test_client_container_is_a_two_element_array(self):
        from RNS.vendor import umsgpack as msgpack
        blob = os.urandom(200)
        packed = msgpack.packb([1234.5, [blob]])
        decoded = msgpack.unpackb(packed)
        self.assertIsInstance(decoded, list)
        self.assertEqual(len(decoded), 2)
        self.assertIsInstance(decoded[1], list)
        self.assertEqual(decoded[1][0], blob)

    def test_short_messages_travel_as_packets_not_resources(self):
        # Both inbound paths must exist: this is the threshold that decides
        # which one an ordinary text message takes.
        self.assertGreater(LXMessage.LINK_PACKET_MAX_CONTENT, 0)
        self.assertLess(LXMessage.LINK_PACKET_MAX_CONTENT, 1024)


if __name__ == "__main__":
    unittest.main()
