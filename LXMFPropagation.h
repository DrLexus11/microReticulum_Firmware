// LXMF propagation node (store-and-forward) for the ESP32.
//
// WHY THIS EXISTS
//
// Without a propagation node, LXMF can only deliver opportunistically: the
// sender opens a link straight to the recipient, which requires the recipient
// to be online at that instant. An overnight test on 2026-08-23 showed exactly
// what that means in practice -- NomadNet pages were flawless all night because
// they terminate on an always-on node, while phone-to-phone messages were
// unreliable because they terminate on a phone, and iOS suspends backgrounded
// apps. See docs/Messaging.md.
//
// In a disaster, simultaneous presence is precisely what you do not have.
// Store-and-forward is not an optimisation here; it is the feature.
//
// A Linux box can run `lxmd` and solve this with no code at all, and every
// blackbox should. This exists so a building with no Linux box still has
// store-and-forward -- and so that if the blackbox dies, the nodes inside the
// flats keep holding messages for each other instead of the building going
// silent.
//
// WHAT MAKES THIS TRACTABLE ON A MICROCONTROLLER
//
// A propagation node never decrypts anything. Messages are encrypted end to end
// between their real endpoints, so this node only stores opaque blobs keyed by
// destination hash and hands them over when that destination asks. No message
// parsing, no delivery logic, no stamp generation -- only stamp validation,
// which is a single hash. That is a small fraction of LXMF (~4,500 lines in
// Python, nearly all of it client-side concerns).
//
// THE PROTOCOL, as read from Python LXMF (LXMRouter.py / LXMPeer.py):
//
//   destination : (identity, IN, SINGLE, "lxmf", "propagation")
//   announce    : msgpack [ legacy(bool), timebase(int), state(bool),
//                           per_transfer_limit_kb, per_sync_limit_kb,
//                           [stamp_cost, flexibility, peering_cost],
//                           metadata(map) ]
//   /offer      : request  [peering_key, [transient_ids]]
//                 response True (want all) | False (want none)
//                          | [wanted_ids] | error byte
//                 the client then sends the wanted messages as a Resource
//   /get        : request  [wanted_ids, purge_ids]
//                 both nil -> list the transient_ids held for the caller
//
// BIT-COMPATIBILITY IS THE RISK. If any of the above diverges from Python
// LXMF, Sideband and Reticchat will silently fail to sync rather than report an
// error -- the same silent-failure signature that has cost this project more
// time than anything else. Interop-test against a real `lxmd`, not just against
// another one of these nodes.

#pragma once

#include <microReticulum.h>
#include <MsgPack.h>

#define LXMF_APP_NAME    "lxmf"
#define LXMF_PN_ASPECT   "propagation"
#define LXMF_OFFER_PATH  "/offer"
#define LXMF_GET_PATH    "/get"

// Error codes, from LXMPeer.py. Returned as a single byte where the protocol
// expects a response.
#define LXMF_ERROR_NO_IDENTITY   0xf0
#define LXMF_ERROR_NO_ACCESS     0xf1
#define LXMF_ERROR_INVALID_KEY   0xf3
#define LXMF_ERROR_INVALID_DATA  0xf4
#define LXMF_ERROR_INVALID_STAMP 0xf5
#define LXMF_ERROR_THROTTLED     0xf6
#define LXMF_ERROR_NOT_FOUND     0xfd
#define LXMF_ERROR_TIMEOUT       0xfe

// Per-message limit we advertise, in kilobytes.
//
// Python defaults to 256 KB. One such message would consume a sixth of this
// board's entire message store, so we advertise far less and expect text. The
// limit travels in the announce, so clients honour it -- this is a supported
// knob, not a local fudge.
#ifndef LXMF_PN_TRANSFER_LIMIT_KB
#define LXMF_PN_TRANSFER_LIMIT_KB 8
#endif

// Per-sync limit, in kilobytes. Python uses 40x the transfer limit.
#ifndef LXMF_PN_SYNC_LIMIT_KB
#define LXMF_PN_SYNC_LIMIT_KB 64
#endif

// Proof-of-work cost demanded of senders. LXMPeer's minimum is 13; Python
// defaults to 16. We only ever *validate* a stamp, which is one hash.
#ifndef LXMF_PN_STAMP_COST
#define LXMF_PN_STAMP_COST 16
#endif
#ifndef LXMF_PN_STAMP_FLEX
#define LXMF_PN_STAMP_FLEX 3
#endif
#ifndef LXMF_PN_PEERING_COST
#define LXMF_PN_PEERING_COST 18
#endif

// How often to re-announce the propagation node.
//
// Longer than the NomadNet page announce: a client only needs to discover a
// propagation node occasionally, and every announce is airtime taken from a
// shared half-duplex channel that already carries one announce per node per
// five minutes.
#ifndef LXMF_PN_ANNOUNCE_INTERVAL_MS
#define LXMF_PN_ANNOUNCE_INTERVAL_MS 1800000   // 30 minutes
#endif

extern RNS::Destination lxmf_propagation_destination;

// Build the announce payload. Field order and types must match
// LXMRouter.get_propagation_node_app_data() exactly.
inline RNS::Bytes lxmf_pn_app_data() {
	MsgPack::Packer packer;
	packer.serialize(MsgPack::arr_size_t(7));
	packer.serialize(false);                                    // 0: legacy PN support
	packer.serialize((uint64_t)RNS::Utilities::OS::time());     // 1: node timebase
	packer.serialize(true);                                     // 2: we are a propagation node
	packer.serialize((uint32_t)LXMF_PN_TRANSFER_LIMIT_KB);      // 3: per-transfer limit (KB)
	packer.serialize((uint32_t)LXMF_PN_SYNC_LIMIT_KB);          // 4: per-sync limit (KB)
	packer.serialize(MsgPack::arr_size_t(3));                   // 5: stamp costs
	packer.serialize((uint32_t)LXMF_PN_STAMP_COST);
	packer.serialize((uint32_t)LXMF_PN_STAMP_FLEX);
	packer.serialize((uint32_t)LXMF_PN_PEERING_COST);
	packer.serialize(MsgPack::map_size_t(0));                   // 6: metadata (empty)
	return RNS::Bytes(packer.data(), packer.size());
}

// --- Message store ---------------------------------------------------------
//
// On-disk format deliberately mirrors Python's, because both keys derive from
// the blob itself and nothing else is needed:
//
//   transient_id     = full_hash(blob)      -- sha256, 32 bytes
//   destination_hash = blob[0:16]           -- literally the first 16 bytes
//   file name        = <transient_id_hex>_<received>
//   file content     = the raw blob, unmodified
//
// So there is no header to invent, no parsing, and a store written here is
// readable by a Python node and vice versa. The node never learns anything
// about the message beyond who it is for.

#ifndef LXMF_PN_STORE_DIR
#define LXMF_PN_STORE_DIR "/lxmf"
#endif

// Hard caps. Flash is 1.6 MB free on this board and LittleFS slows down with
// very large directories, so bound both count and bytes. Oldest is evicted
// first when either is hit.
#ifndef LXMF_PN_MAX_MESSAGES
#define LXMF_PN_MAX_MESSAGES 128
#endif
#ifndef LXMF_PN_MAX_BYTES
#define LXMF_PN_MAX_BYTES (512 * 1024)
#endif

#define LXMF_TRANSIENT_ID_LEN 32
#define LXMF_DESTINATION_LEN  16

struct LXMFEntry {
	RNS::Bytes transient_id;      // sha256 of the blob
	RNS::Bytes destination_hash;  // blob[0:16] -- who it is for
	uint32_t   received;          // node-local receive time
	uint32_t   size;
};

// In-RAM index, rebuilt from the directory at boot. The files are the source of
// truth; this is only a lookup so /offer and /get do not have to stat the
// filesystem on every request.
extern std::vector<LXMFEntry> lxmf_store_index;

inline std::string lxmf_store_path() {
	return std::string(RNS::Reticulum::storagepath()) + LXMF_PN_STORE_DIR;
}

inline std::string lxmf_entry_path(const RNS::Bytes& transient_id, uint32_t received) {
	char name[80];
	snprintf(name, sizeof(name), "/%s_%lu",
	         transient_id.toHex().c_str(), (unsigned long)received);
	return lxmf_store_path() + name;
}

inline size_t lxmf_store_bytes() {
	size_t total = 0;
	for (const auto& e : lxmf_store_index) total += e.size;
	return total;
}

inline bool lxmf_store_has(const RNS::Bytes& transient_id) {
	for (const auto& e : lxmf_store_index) {
		if (e.transient_id == transient_id) return true;
	}
	return false;
}

// Evict the oldest entry. Called when a cap is hit.
inline bool lxmf_store_evict_oldest() {
	if (lxmf_store_index.empty()) return false;
	size_t oldest = 0;
	for (size_t i = 1; i < lxmf_store_index.size(); i++) {
		if (lxmf_store_index[i].received < lxmf_store_index[oldest].received) oldest = i;
	}
	const LXMFEntry& e = lxmf_store_index[oldest];
	std::string path = lxmf_entry_path(e.transient_id, e.received);
	RNS::Utilities::OS::remove_file(path.c_str());
	printf("[lxmf] store full, evicted %s\n", e.transient_id.toHex().substr(0, 16).c_str());
	lxmf_store_index.erase(lxmf_store_index.begin() + oldest);
	return true;
}

// Store one message blob. Returns false if it was rejected or already held.
inline bool lxmf_store_put(const RNS::Bytes& blob) {
	if (blob.size() < LXMF_DESTINATION_LEN) {
		printf("[lxmf] rejecting %u-byte blob: shorter than a destination hash\n",
		       (unsigned)blob.size());
		return false;
	}
	if (blob.size() > (size_t)LXMF_PN_TRANSFER_LIMIT_KB * 1024) {
		// We advertised this limit in the announce, so a well-behaved client
		// will not send anything larger; enforce it anyway.
		printf("[lxmf] rejecting %u-byte blob: over the %d KB advertised limit\n",
		       (unsigned)blob.size(), LXMF_PN_TRANSFER_LIMIT_KB);
		return false;
	}

	RNS::Bytes transient_id = RNS::Identity::full_hash(blob);
	if (lxmf_store_has(transient_id)) return false;   // already held; not an error

	while (lxmf_store_index.size() >= LXMF_PN_MAX_MESSAGES ||
	       lxmf_store_bytes() + blob.size() > (size_t)LXMF_PN_MAX_BYTES) {
		if (!lxmf_store_evict_oldest()) break;
	}

	uint32_t received = (uint32_t)RNS::Utilities::OS::time();
	std::string path = lxmf_entry_path(transient_id, received);
	if (RNS::Utilities::OS::write_file(path.c_str(), blob) != blob.size()) {
		printf("[lxmf] FAILED to write %s\n", path.c_str());
		return false;
	}

	LXMFEntry entry;
	entry.transient_id     = transient_id;
	entry.destination_hash = blob.left(LXMF_DESTINATION_LEN);
	entry.received         = received;
	entry.size             = blob.size();
	lxmf_store_index.push_back(entry);

	printf("[lxmf] stored %u bytes for <%s> (%u held, %u bytes)\n",
	       (unsigned)blob.size(), entry.destination_hash.toHex().c_str(),
	       (unsigned)lxmf_store_index.size(), (unsigned)lxmf_store_bytes());
	return true;
}

// Rebuild the index by listing the store directory. The filename carries both
// keys, so nothing else needs reading -- important on a board where opening 128
// files at boot would be slow.
inline void lxmf_store_load() {
	lxmf_store_index.clear();
	std::string dir = lxmf_store_path();
	if (!RNS::Utilities::OS::directory_exists(dir.c_str())) {
		RNS::Utilities::OS::create_directory(dir.c_str());
		printf("[lxmf] created message store at %s\n", dir.c_str());
		return;
	}
	for (const std::string& name : RNS::Utilities::OS::list_directory(dir.c_str())) {
		// <64 hex chars>_<received>
		size_t sep = name.find('_');
		if (sep != LXMF_TRANSIENT_ID_LEN * 2) continue;
		RNS::Bytes transient_id;
		transient_id.assignHex(name.substr(0, sep).c_str());
		if (transient_id.size() != LXMF_TRANSIENT_ID_LEN) continue;

		// The destination hash lives in the first 16 bytes of the blob, so this
		// one read per entry is unavoidable at boot.
		RNS::Bytes blob;
		std::string path = dir + "/" + name;
		if (RNS::Utilities::OS::read_file(path.c_str(), blob) < LXMF_DESTINATION_LEN) continue;

		LXMFEntry entry;
		entry.transient_id     = transient_id;
		entry.destination_hash = blob.left(LXMF_DESTINATION_LEN);
		entry.received         = (uint32_t)strtoul(name.substr(sep + 1).c_str(), nullptr, 10);
		entry.size             = blob.size();
		lxmf_store_index.push_back(entry);
	}
	printf("[lxmf] store loaded: %u message(s), %u bytes\n",
	       (unsigned)lxmf_store_index.size(), (unsigned)lxmf_store_bytes());
}

// --- Request handlers ------------------------------------------------------
//
// Signature matches RNS::RequestHandler::response_generator, the same shape
// serve_page() uses. The return value is spliced verbatim into the response
// envelope, so error replies are a bare msgpack byte, exactly as Python returns
// LXMPeer.ERROR_* constants.

inline RNS::Bytes lxmf_pn_error(uint8_t code) {
	MsgPack::Packer packer;
	packer.serialize((uint8_t)code);
	return RNS::Bytes(packer.data(), packer.size());
}

// /offer -- a client (or peer node) lists transient ids it holds, and we reply
// with which of them we want. Phase 1 stub: accept nothing.
inline RNS::Bytes lxmf_offer_request(
	const RNS::Bytes& path, const RNS::Bytes& data, const RNS::Bytes& request_id,
	const RNS::Bytes& link_id, const RNS::Identity& remote_identity, double requested_at
) {
	// Python returns ERROR_NO_IDENTITY when the peer has not identified. The
	// caller's identity is what keys the message store, so this is not optional.
	if (!remote_identity) {
		printf("[lxmf] /offer from unidentified peer, refusing\n");
		return lxmf_pn_error(LXMF_ERROR_NO_IDENTITY);
	}
	printf("[lxmf] /offer from <%s> (%u bytes)\n",
	       remote_identity.hash().toHex().c_str(), (unsigned)data.size());

	// Phase 1: the store is not implemented, so want nothing. `false` is a
	// legitimate protocol response ("I want none of these"), which leaves the
	// client's messages queued with it rather than failing -- the honest
	// behaviour for a node that cannot yet hold them.
	MsgPack::Packer packer;
	packer.serialize(false);
	return RNS::Bytes(packer.data(), packer.size());
}

// /get -- a client asks what we are holding for it, collects, and purges.
// Phase 1 stub: we hold nothing, so return an empty list.
inline RNS::Bytes lxmf_message_get_request(
	const RNS::Bytes& path, const RNS::Bytes& data, const RNS::Bytes& request_id,
	const RNS::Bytes& link_id, const RNS::Identity& remote_identity, double requested_at
) {
	if (!remote_identity) {
		printf("[lxmf] /get from unidentified peer, refusing\n");
		return lxmf_pn_error(LXMF_ERROR_NO_IDENTITY);
	}
	printf("[lxmf] /get from <%s>\n", remote_identity.hash().toHex().c_str());

	MsgPack::Packer packer;
	packer.serialize(MsgPack::arr_size_t(0));
	return RNS::Bytes(packer.data(), packer.size());
}

// Announce the propagation node periodically so clients can discover it.
inline void lxmf_propagation_announce_watch() {
	static uint32_t last = 0;
	static bool armed = false;
	if (!lxmf_propagation_destination) return;
	// Jittered like the NomadNet announce, and for the same reason: a block of
	// nodes restored to mains power must not transmit in lockstep.
	if (!armed) { armed = true; last = millis(); return; }
	if (millis() - last < LXMF_PN_ANNOUNCE_INTERVAL_MS) return;
	last = millis();
	RNS::Bytes app_data = lxmf_pn_app_data();
	lxmf_propagation_destination.announce(app_data);
	printf("[lxmf] announced propagation node (%u byte app_data)\n",
	       (unsigned)app_data.size());
}
