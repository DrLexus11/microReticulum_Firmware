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
import inspect
import os
import re
import time
import unittest

HEADER = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "LXMFPropagation.h")
COMPOSE = os.path.join(os.path.dirname(HEADER), "LXMFCompose.h")
PLATFORMIO = os.path.join(os.path.dirname(HEADER), "platformio.ini")

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


class BuildConfigurationTests(unittest.TestCase):
    """Production RAD builds must not silently compile the feature out."""

    def test_propagation_node_enabled_for_both_rad_revisions(self):
        with open(PLATFORMIO, "r", encoding="utf-8") as handle:
            text = handle.read()

        for environment in ("impr-rad01-rev1", "impr-rad01-rev2"):
            with self.subTest(environment=environment):
                section = re.search(
                    rf"^\[env:{re.escape(environment)}\]\s*$"
                    rf"(?P<body>.*?)(?=^\[|\Z)",
                    text,
                    re.M | re.S,
                )
                self.assertIsNotNone(section, f"missing environment {environment}")
                self.assertIn("-DLXMF_PROPAGATION_NODE",
                              section.group("body"))


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
        self.assertLessEqual(d["LXMF_PN_TRANSFER_LIMIT_BYTES"], store // 8)
        self.assertLessEqual(d["LXMF_PN_SYNC_LIMIT_BYTES"], store)

    def test_message_count_is_primary_and_byte_cap_is_defense_in_depth(self):
        d = firmware_defines()
        maximum_valid_store = (
            d["LXMF_PN_MAX_MESSAGES"] * d["LXMF_PN_TRANSFER_LIMIT_BYTES"]
        )
        self.assertLessEqual(maximum_valid_store, d["LXMF_PN_MAX_BYTES"])

    def test_limit_bytes_match_lxmf_decimal_kilobytes(self):
        # LXMF's Python reference multiplies advertised KB values by 1000.
        d = firmware_defines()
        self.assertEqual(d["LXMF_PN_TRANSFER_LIMIT_BYTES"],
                         d["LXMF_PN_TRANSFER_LIMIT_KB"] * 1000)
        self.assertEqual(d["LXMF_PN_SYNC_LIMIT_BYTES"],
                         d["LXMF_PN_SYNC_LIMIT_KB"] * 1000)

    def test_response_budget_can_serve_one_maximum_message(self):
        d = firmware_defines()
        conservative_response_overhead = 24 + 16
        self.assertGreaterEqual(d["LXMF_PN_RESPONSE_LIMIT_BYTES"],
                                d["LXMF_PN_TRANSFER_LIMIT_BYTES"] +
                                conservative_response_overhead)
        self.assertLessEqual(d["LXMF_PN_RESPONSE_LIMIT_BYTES"],
                             d["LXMF_PN_SYNC_LIMIT_BYTES"])


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


def compose_source():
    with open(COMPOSE, "r", encoding="utf-8") as handle:
        return handle.read()


def reference_payload_order():
    """The payload field order as the LXMF reference itself writes it.

    Read from Python source rather than hardcoded, so an upstream reordering
    fails here instead of silently disagreeing with what we compose.
    """
    source = inspect.getsource(LXMessage.pack)
    match = re.search(r"self\.payload\s*=\s*\[(?P<fields>[^\]]+)\]", source)
    if match is None:
        # Not an assert: under python -O the check would be stripped and the
        # next line would fail with an AttributeError on None instead, which is
        # still a failure but says nothing about what actually went wrong.
        raise AssertionError(
            "could not find the payload assignment in LXMessage.pack; the "
            "reference may have been restructured upstream"
        )
    return [f.strip().replace("self.", "")
            for f in match.group("fields").split(",")]


@unittest.skipUnless(HAVE_LXMF, "LXMF not importable; run under the RNS virtualenv")
class ComposedMessageLayoutTests(unittest.TestCase):
    """The bridge composes LXMF rather than relaying it.

    A malformed composed message is worse than a malformed stored one: it syncs
    perfectly and is then discarded inside someone else's client, with no error
    reaching us. These pin the layout in LXMFCompose.h to the reference.
    """

    def test_payload_field_order_matches_the_reference(self):
        # The one that bites: title precedes content. Swapping them yields a
        # message that decrypts and validates cleanly, and displays the body
        # in the subject line.
        self.assertEqual(["timestamp", "title", "content", "fields"],
                         reference_payload_order())

        source = compose_source()
        body = re.search(r"lxmf_pack_payload\b.*?\n\}", source, re.S)
        self.assertIsNotNone(body, "lxmf_pack_payload not found")
        # Position 3 is written by two branches -- an empty map when no fields
        # were supplied, or a pre-packed map spliced in -- so collapse repeats
        # and assert the order of the positions themselves.
        packed_order = []
        for position, name in re.findall(r"//\s*(\d):\s*(\w+)", body.group(0)):
            if packed_order and packed_order[-1][0] == position:
                continue
            packed_order.append((position, name))
        self.assertEqual([("0", "seconds"), ("1", "title"),
                          ("2", "content"), ("3", "fields")], packed_order)

    def test_msgpack_format_families_match(self):
        """The exact type bytes the reference emits for a payload.

        MsgPack's C++ packer chooses a family per call, so these assert that
        the calls chosen in lxmf_pack_payload produce the same wire types the
        reference does -- an array of four, float64 timestamp, bin title and
        content, and an empty map of fields.
        """
        from RNS.vendor import umsgpack as msgpack

        payload = [time.time(), b"", b"hello", {}]
        packed = msgpack.packb(payload)
        self.assertEqual(0x94, packed[0], "four-element array header")
        self.assertEqual(0xCB, msgpack.packb(payload[0])[0], "float64 timestamp")
        self.assertEqual(0xC4, msgpack.packb(b"hello")[0], "bin8 for short bytes")
        self.assertEqual(0x80, msgpack.packb({})[0], "empty fixmap for fields")

        source = compose_source()
        self.assertIn("packBinary", source, "title/content must pack as bin")
        self.assertIn("map_size_t(0)", source, "fields must pack as an empty map")

    def test_packed_concatenation_order(self):
        source = inspect.getsource(LXMessage.pack)
        order = re.findall(r"self\.packed\s*\+=\s*self\.(\w+)", source)
        self.assertEqual(["signature"], order[2:3],
                         "signature is the third element in the reference")

        composed = re.search(r"RNS::Bytes packed;\s*\n\s*packed << (?P<order>[^;]+);",
                             compose_source())
        self.assertIsNotNone(composed, "packed concatenation not found")
        fields = [f.strip() for f in composed.group("order").split("<<")]
        self.assertEqual(["destination_hash", "source_destination_hash",
                          "signature", "payload"], fields)

    def test_hashes_are_delivery_destinations_not_identities(self):
        """A conversation is keyed on the delivery destination hash.

        Using an identity hash produces a message that no client can match to
        a conversation, and nothing in the transfer complains.
        """
        identity = RNS.Identity()
        delivery = RNS.Destination(identity, RNS.Destination.OUT,
                                   RNS.Destination.SINGLE, "lxmf", "delivery")
        self.assertNotEqual(identity.hash, delivery.hash)
        self.assertEqual(16, len(delivery.hash))

        source = compose_source()
        self.assertIn('LXMF_DELIVERY_ASPECT "delivery"', source)
        self.assertIn("destination_hash", source)

    def test_stamp_is_stripped_before_delivery_so_a_zero_stamp_is_inert(self):
        """Why the bridge may append a zero propagation stamp.

        The stamp gates ingest at a propagation node and is removed again
        before the message is served, so a message inserted directly into this
        node's own store never crosses the gate the stamp exists for.
        """
        router = inspect.getsource(LXMRouter)
        self.assertIn("[:-LXStamper.STAMP_SIZE]", router,
                      "the reference must still strip the stamp when serving")

        with open(HEADER, "r", encoding="utf-8") as handle:
            self.assertIn("blob.size() - LXMF_STAMP_SIZE", handle.read(),
                          "our serve path must strip it too")

    def test_signature_length_matches_the_reference(self):
        identity = RNS.Identity()
        self.assertEqual(64, len(identity.sign(b"probe")))

    def test_bridge_announces_the_address_it_sends_from(self):
        """Composing correctly is not enough; the source must be recallable.

        LXMF validates a signature by recalling the source identity from an
        announce. With no announce it reports SOURCE_UNKNOWN, and
        signature_validated is False either way -- so a perfectly signed
        message shows as unverified and looks identical to a broken one. The
        first end-to-end bridge test hit exactly this.
        """
        from RNS.vendor import umsgpack as msgpack

        bridge = os.path.join(os.path.dirname(HEADER), "RRCBridge.cpp")
        with open(bridge, "r", encoding="utf-8") as handle:
            source = handle.read()
        self.assertIn("delivery_app_data", source,
                      "the bridge must publish its delivery address")
        self.assertIn(".announce(", source)

        # Field order and types, from LXMRouter.get_announce_app_data():
        # [display_name, stamp_cost, [supported_functionality]].
        reference = msgpack.packb([b"IMPR-RAD RRC", None, [0x00]])
        self.assertEqual(0x93, reference[0], "three-element array")
        self.assertEqual(0xC4, reference[1], "display name packs as bin")
        self.assertEqual(0xC0, reference[-3], "absent stamp cost packs as nil")
        self.assertEqual(0x91, reference[-2], "functionality is a one-element array")
        self.assertEqual(0x00, reference[-1], "SF_COMPRESSION is 0x00")

        self.assertIn("arr_size_t(3)", source)
        self.assertIn("nil_t", source)


class PeerSyncStoreShareTests(unittest.TestCase):
    """The store guarantee that makes accepting peer sync safe.

    Peer sync was declined outright because a Linux node's 500 MB backlog would
    evict the residents' messages a 512 KB store exists to hold. The protection
    is not autopeer_maxdepth -- a large peer one hop away is inside any depth
    bound -- it is bounding the *share* of our own store a peer may occupy, which
    holds whatever is on the other end.
    """

    def setUp(self):
        self.d = firmware_defines()
        with open(HEADER, "r", encoding="utf-8") as handle:
            self.source = handle.read()

    def test_peer_share_leaves_room_for_local_messages(self):
        pct = self.d["LXMF_PN_PEER_SHARE_PCT"]
        self.assertGreater(pct, 0, "a zero share makes peering impossible")
        self.assertLess(pct, 100, "a full share lets a peer fill the store")

    def test_peer_caps_are_strictly_below_the_store_caps(self):
        self.assertLess(self.d["LXMF_PN_PEER_MAX_BYTES"], self.d["LXMF_PN_MAX_BYTES"])
        self.assertLess(self.d["LXMF_PN_PEER_MAX_MESSAGES"], self.d["LXMF_PN_MAX_MESSAGES"])

    def test_peer_share_cannot_starve_a_single_local_message(self):
        """Whatever the share, a full-size local message must still fit."""
        local_bytes = self.d["LXMF_PN_MAX_BYTES"] - self.d["LXMF_PN_PEER_MAX_BYTES"]
        self.assertGreaterEqual(local_bytes, self.d["LXMF_PN_TRANSFER_LIMIT_BYTES"])
        local_slots = self.d["LXMF_PN_MAX_MESSAGES"] - self.d["LXMF_PN_PEER_MAX_MESSAGES"]
        self.assertGreaterEqual(local_slots, 1)

    def test_eviction_takes_peer_messages_before_local_ones(self):
        """Origin outranks age. Age-only eviction is what let a peer displace
        the people attached to this node."""
        evict = self.source[self.source.index("inline bool lxmf_store_evict_oldest"):]
        evict = evict[:evict.index("\n}")]
        self.assertIn("oldest_of(true)", evict, "peer-received must be tried first")
        peer_first = evict.index("oldest_of(true)")
        local_next = evict.index("oldest_of(false)")
        self.assertLess(peer_first, local_next)

    def test_a_peer_message_is_refused_rather_than_evicting_to_fit(self):
        put = self.source[self.source.index("inline bool lxmf_store_put"):]
        put = put[:put.index("\n}")]
        self.assertIn("if (from_peer)", put)
        self.assertIn("return false", put)


class PeerOfferAcceptanceTests(unittest.TestCase):
    """/offer must answer with wanted ids, in the shape LXMPeer expects."""

    def setUp(self):
        self.d = firmware_defines()
        with open(HEADER, "r", encoding="utf-8") as handle:
            self.source = handle.read()
        self.offer = self.source[self.source.index("inline RNS::Bytes lxmf_offer_request"):]
        self.offer = self.offer[:self.offer.index("\n}")]

    def test_offer_no_longer_declines_unconditionally(self):
        """The old handler packed `false` and returned, whatever was offered."""
        self.assertNotIn("does not accept peer sync", self.source)
        self.assertIn("lxmf_store_has(id)", self.offer,
                      "must ask only for ids it does not already hold")

    def test_offer_response_uses_the_proven_packing_idiom(self):
        """Same as the /get response, which stock clients already accept."""
        self.assertIn("MsgPack::arr_size_t(wanted.size())", self.offer)
        self.assertIn("MsgPack::bin_t<uint8_t>", self.offer)

    def test_offer_declines_with_false_when_it_wants_nothing(self):
        """False is the protocol's 'none of these'; the peer keeps them."""
        self.assertIn("packer.serialize(false)", self.offer)

    def test_wanted_count_is_bounded_by_the_peer_share(self):
        self.assertIn("LXMF_PN_PEER_MAX_MESSAGES", self.offer)
        self.assertIn("slots", self.offer)

    def test_requested_ids_are_recorded_so_they_store_as_peer_received(self):
        """Origin is tracked by what we asked for, not by which link answered:
        Resource exposes no link in this port, and the request is the honest
        definition anyway."""
        self.assertIn("lxmf_expect_from_peer", self.offer)
        self.assertIn("lxmf_claim_peer_wanted", self.source)

    @unittest.skipUnless(HAVE_LXMF, "LXMF not importable; run under the RNS virtualenv")
    def test_transient_id_length_matches_lxmf(self):
        """We filter offered ids by length; it must be LXMF's own."""
        self.assertEqual(self.d["LXMF_TRANSIENT_ID_LEN"],
                         len(hashlib.sha256(b"x").digest()))


class PeerSyncAirtimeTests(unittest.TestCase):
    """The outbound half's bounds. These are airtime limits, not style.

    Every step of a sync crosses LoRa on a shared, duty-cycled channel that also
    carries the traffic this node exists to move. A sync that is too eager
    degrades the mesh for everyone on it, which is worse than the problem being
    solved.
    """

    def setUp(self):
        path = os.path.join(os.path.dirname(HEADER), "LXMFPeerSync.h")
        with open(path, "r", encoding="utf-8") as handle:
            self.source = handle.read()
        out = {}
        for name, value in re.findall(r'^#define\s+(\w+)\s+(.+?)\s*$',
                                      self.source, re.M):
            value = value.split("//")[0].strip()
            try:
                out[name] = int(value, 0)
            except ValueError:
                pass
        self.d = out

    def test_sync_interval_is_measured_in_tens_of_minutes(self):
        """A propagation store is a backstop, not a live feed."""
        self.assertGreaterEqual(self.d["LXMF_PEER_SYNC_INTERVAL_MS"], 600000)

    def test_first_sync_waits_for_the_node_to_settle(self):
        self.assertGreaterEqual(self.d["LXMF_PEER_SYNC_FIRST_MS"], 60000)
        self.assertLess(self.d["LXMF_PEER_SYNC_FIRST_MS"],
                        self.d["LXMF_PEER_SYNC_INTERVAL_MS"])

    def test_offer_batch_is_small_enough_for_one_lora_transfer(self):
        """32 bytes per id, so the batch size is the request's size on air."""
        request_bytes = self.d["LXMF_PEER_OFFER_BATCH"] * 32
        self.assertLessEqual(request_bytes, 1024,
                             "offering the whole store is kilobytes before a "
                             "single message moves")

    def test_peer_table_and_depth_are_bounded(self):
        self.assertLessEqual(self.d["LXMF_PEER_MAX_PEERS"], 8)
        self.assertLessEqual(self.d["LXMF_PEER_MAX_DEPTH"], 6)

    def test_sync_does_nothing_when_there_is_nothing_to_offer(self):
        """The common case must cost no airtime whatsoever."""
        watch = self.source[self.source.index("inline void lxmf_peer_sync_watch"):]
        self.assertIn("lxmf_store_index.empty()", watch)

    def test_only_one_sync_runs_at_a_time(self):
        self.assertIn("if (st.active)", self.source)
        self.assertIn("LXMF_PEER_SYNC_TIMEOUT_MS", self.source)

    def test_node_never_peers_with_itself(self):
        """Our own announce returning over a looping interface would otherwise
        make the node offer its whole store to itself, forever."""
        self.assertIn("lxmf_propagation_destination.hash()", self.source)

    def test_sync_finish_clears_state_before_tearing_down(self):
        """Link::teardown() calls the closed callback synchronously.

        Clearing st.active after the teardown left the re-entry guard true, so
        finish -> teardown -> link_closed -> closed callback -> finish recursed
        until the loopTask stack canary fired. Measured as a PANIC ~300s after
        boot, once a peer existed for the sync path to run at all.
        """
        fn = self.source[self.source.index("inline void lxmf_peer_sync_finish"):]
        fn = fn[:fn.index("\n}")]
        # Match the call, not the word: the comment above it also says
        # "teardown()", and matching that made this assert against prose.
        self.assertIn("st.active  = false", fn)
        self.assertIn("link.teardown()", fn)
        self.assertLess(fn.index("st.active  = false"), fn.index("link.teardown()"),
                        "state must be cleared before teardown can re-enter")

    def test_outbound_send_respects_the_sync_limit(self):
        self.assertIn("LXMF_PN_SYNC_LIMIT_BYTES", self.source)



if __name__ == "__main__":
    unittest.main()
