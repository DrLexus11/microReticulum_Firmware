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
#include <algorithm>
#include <vector>

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

// Per-message and per-sync limits we advertise, in kilobytes.
//
// These are not guesses. Measured on RAD-01 Rev 1 on 2026-08-24 by pushing
// Resources of increasing size at the propagation destination:
//
//     8 KB   -> COMPLETE in 2.0 s
//     16 KB  -> FAILED after 103.5 s (transfer stalls, then times out)
//     32 KB  -> FAILED after 104.5 s
//
// So the practical inbound Resource ceiling on this hardware is between 8 and
// 16 KB. We previously advertised a 64 KB sync limit, which the board could not
// honour: real traffic only worked because messages are small, and a client
// batching toward the advertised limit would have stalled for a minute and a
// half and then failed. Advertising what we can actually do is the difference
// between a slow node and a broken one.
//
// Python defaults to 256 KB per message; one of those would consume half this
// board's entire store, so text-sized limits are right on both counts.
//
// Why the stall above ~8 KB is not yet understood -- window sizing, retries, or
// memory during reassembly -- is worth chasing, but advertising honestly does
// not depend on the answer.
#ifndef LXMF_PN_TRANSFER_LIMIT_KB
#define LXMF_PN_TRANSFER_LIMIT_KB 4
#endif

// One sync may carry several messages; keep the total inside the measured
// ceiling. Larger than the per-message limit so a client can still batch, but
// not so large that it exceeds what a transfer completes at.
#ifndef LXMF_PN_SYNC_LIMIT_KB
#define LXMF_PN_SYNC_LIMIT_KB 8
#endif

// LXMF's reference implementation converts all advertised kilobyte limits with
// *1000 (not KiB's *1024). Keep the wire promise and the enforced byte ceilings
// identical. These stay outside the guards so build-time KB overrides propagate.
#define LXMF_PN_TRANSFER_LIMIT_BYTES ((size_t)LXMF_PN_TRANSFER_LIMIT_KB * 1000)
#define LXMF_PN_SYNC_LIMIT_BYTES     ((size_t)LXMF_PN_SYNC_LIMIT_KB * 1000)

// Outbound /get responses need additional headroom on a full store. An 8 KB
// response worked with a lightly loaded node but repeatedly stalled once the
// index held 128 entries; the same dataset completed cleanly in 2 KB batches.
// 4096 bytes is the lowest useful node cap: it still accommodates one maximum
// 4000-byte stored blob plus the conservative 40-byte response allowance below.
// This does not change the advertised inbound sync limit. Returning fewer
// wanted messages is supported by LXMF; the client asks again for the rest.
#ifndef LXMF_PN_RESPONSE_LIMIT_BYTES
#define LXMF_PN_RESPONSE_LIMIT_BYTES 4096
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

// A propagation node is useless until it has announced, so the first one comes
// shortly after boot rather than one full interval later. Matches the NomadNet
// announce, which learned the same lesson about the DHCP race.
#define LXMF_PN_FIRST_ANNOUNCE_MS 60000        // 1 minute
#define LXMF_PN_ANNOUNCE_JITTER_MS 60000       // up to 1 minute
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
// very large directories, so bound both count and bytes. With today's 4000-byte
// per-message ceiling, 128 messages total at most 512000 bytes, so the count cap
// deliberately fires before the 512 KiB cap. The byte cap is defense-in-depth
// for build overrides or future transfer-limit changes. Oldest is evicted first
// when either is hit.
#ifndef LXMF_PN_MAX_MESSAGES
#define LXMF_PN_MAX_MESSAGES 128
#endif
#ifndef LXMF_PN_MAX_BYTES
#define LXMF_PN_MAX_BYTES (512 * 1024)
#endif

// Both the advertised limits and the store caps are individually overridable at
// build time, so the store's guarantee must not rest on the two happening to be
// chosen sensibly. A single message that cannot fit inside the store, or a sync
// allowed to exceed it outright, is a configuration that can never satisfy its
// own cap -- catch that here rather than at run time on a deployed node.
static_assert(LXMF_PN_TRANSFER_LIMIT_BYTES <= (size_t)LXMF_PN_MAX_BYTES,
              "LXMF_PN_TRANSFER_LIMIT_KB exceeds the message store: a single "
              "accepted message could not fit within LXMF_PN_MAX_BYTES");
static_assert(LXMF_PN_SYNC_LIMIT_BYTES <= (size_t)LXMF_PN_MAX_BYTES,
              "LXMF_PN_SYNC_LIMIT_KB exceeds the message store: one sync could "
              "fill it outright");

#define LXMF_TRANSIENT_ID_LEN 32
#define LXMF_DESTINATION_LEN  16

// Every propagated message carries a proof-of-work stamp appended to the end,
// sized as one full hash. LXMF_OVERHEAD is Python's minimum plausible message:
// 2 destination hashes + a signature + a timestamp + msgpack structure.
#define LXMF_STAMP_SIZE 32
#define LXMF_OVERHEAD   112

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
	if (!RNS::Utilities::OS::remove_file(path.c_str())) {
		printf("[lxmf] FAILED to evict %s; store cap remains unchanged\n",
		       e.transient_id.toHex().substr(0, 16).c_str());
		return false;
	}
	printf("[lxmf] store full, evicted %s\n", e.transient_id.toHex().substr(0, 16).c_str());
	lxmf_store_index.erase(lxmf_store_index.begin() + oldest);
	return true;
}

// The transient id is the hash of the message *without* its stamp, while the
// stored blob keeps the stamp -- /get strips it again on the way out. This split
// is the single most consequential detail in this file: hash the wrong span and
// every id we advertise is one no client recognises, which presents as messages
// that sync but never arrive rather than as an error.
inline RNS::Bytes lxmf_transient_id(const RNS::Bytes& blob) {
	if (blob.size() <= LXMF_STAMP_SIZE) return RNS::Bytes();
	return RNS::Identity::full_hash(blob.left(blob.size() - LXMF_STAMP_SIZE));
}

// Store one message blob, stamp included. Returns false if it was rejected or
// already held.
inline bool lxmf_store_put(const RNS::Bytes& blob) {
	if (blob.size() <= LXMF_OVERHEAD + LXMF_STAMP_SIZE) {
		printf("[lxmf] rejecting %u-byte blob: below the %d-byte minimum message\n",
		       (unsigned)blob.size(), LXMF_OVERHEAD + LXMF_STAMP_SIZE);
		return false;
	}
	if (blob.size() > LXMF_PN_TRANSFER_LIMIT_BYTES) {
		// We advertised this limit in the announce, so a well-behaved client
		// will not send anything larger; enforce it anyway.
		printf("[lxmf] rejecting %u-byte blob: over the %d KB advertised limit\n",
		       (unsigned)blob.size(), LXMF_PN_TRANSFER_LIMIT_KB);
		return false;
	}

	RNS::Bytes transient_id = lxmf_transient_id(blob);
	if (lxmf_store_has(transient_id)) return false;   // already held; not an error

	while (lxmf_store_index.size() >= LXMF_PN_MAX_MESSAGES ||
	       lxmf_store_bytes() + blob.size() > (size_t)LXMF_PN_MAX_BYTES) {
		if (!lxmf_store_evict_oldest()) break;
	}

	// Eviction stops when there is nothing left to evict, which means the loop
	// above can exit with the cap still unsatisfied. Writing anyway would put
	// the store over a limit it is supposed to enforce, on a board where flash
	// is the scarce resource. The static_asserts make this unreachable for any
	// sane configuration; this is the backstop for one that is not.
	if (lxmf_store_index.size() >= LXMF_PN_MAX_MESSAGES ||
	    lxmf_store_bytes() + blob.size() > (size_t)LXMF_PN_MAX_BYTES) {
		printf("[lxmf] cannot fit %u bytes within the store cap (%u held, "
		       "%u bytes), rejecting\n",
		       (unsigned)blob.size(), (unsigned)lxmf_store_index.size(),
		       (unsigned)lxmf_store_bytes());
		return false;
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
// --- Ingest ----------------------------------------------------------------
//
// Both inbound paths carry the same payload: msgpack [timestamp, [blobs]].
// A client sends it as a plain link packet when it fits (LXMF caps that at 319
// bytes of content, so most short texts take this path) and as a Resource when
// it does not. Only the framing differs, so both funnel through here.
//
// The timestamp is the *sender's* clock. We deliberately do not adopt it: a node
// with no RTC that trusted any client's clock would let one misconfigured phone
// rewrite the age of every stored message. Entries are aged by our own receive
// time instead.

struct LXMFIngestResult {
	unsigned offered    = 0;
	unsigned stored     = 0;
	unsigned duplicate  = 0;
	unsigned rejected   = 0;
	bool     malformed  = false;
	// Accepted means "this message is now held here", which includes one we
	// already had. That distinction decides whether we acknowledge to the
	// sender, and a duplicate is emphatically not a failure to deliver.
	unsigned accepted() const { return stored + duplicate; }
};

inline LXMFIngestResult lxmf_ingest_container(const RNS::Bytes& data) {
	LXMFIngestResult r;
	MsgPack::Unpacker unpacker;
	unpacker.feed(data.data(), data.size());

	if (!unpacker.isArray() || unpacker.unpackArraySize() < 2) {
		r.malformed = true;
		return r;
	}
	// Element 0 is the sender's timebase; step past it whatever its numeric type.
	if      (unpacker.isFloat32()) { (void)unpacker.unpackFloat32(); }
	else if (unpacker.isFloat64()) { (void)unpacker.unpackFloat64(); }
	else if (unpacker.isUInt())    { (void)unpacker.unpackUInt64(); }
	else if (unpacker.isInt())     { (void)unpacker.unpackInt64(); }
	else                           { (void)unpacker.unpackNil(); }

	if (!unpacker.isArray()) {
		r.malformed = true;
		return r;
	}
	size_t count = unpacker.unpackArraySize();
	size_t cumulative = 0;
	for (size_t i = 0; i < count; i++) {
		if (!unpacker.isBin()) { r.malformed = true; break; }
		MsgPack::bin_t<uint8_t> blob_bin = unpacker.unpackBinary<uint8_t>();
		RNS::Bytes blob(blob_bin.data(), blob_bin.size());
		r.offered++;

		// The per-message limit is enforced in lxmf_store_put(); this is the
		// per-sync total. Without it a sender could stay under the message
		// limit and still fill the store in one request.
		cumulative += blob.size();
		if (cumulative > LXMF_PN_SYNC_LIMIT_BYTES) {
			printf("[lxmf] sync exceeded the %u byte limit at message %u, "
			       "ignoring the rest\n",
			       (unsigned)LXMF_PN_SYNC_LIMIT_BYTES, (unsigned)(i + 1));
			r.rejected += (unsigned)(count - i);
			break;
		}

		RNS::Bytes transient_id = lxmf_transient_id(blob);
		if (!transient_id) { r.rejected++; continue; }
		if (lxmf_store_has(transient_id)) { r.duplicate++; continue; }
		if (lxmf_store_put(blob)) r.stored++; else r.rejected++;
	}
	return r;
}

// --- Inbound message transfer ----------------------------------------------

// The packet path. This is the one that carries ordinary short messages from a
// phone, and proving the packet is what turns the sender's "sent" into
// "delivered" -- an unproven packet is exactly the silent non-delivery seen in
// the overnight test, so the acknowledgement matters as much as the storing.
inline void lxmf_propagation_packet(const RNS::Bytes& plaintext, const RNS::Packet& packet) {
	LXMFIngestResult r = lxmf_ingest_container(plaintext);
	if (r.malformed && r.offered == 0) {
		printf("[lxmf] malformed propagation packet (%u bytes), ignoring\n",
		       (unsigned)plaintext.size());
		return;
	}
	printf("[lxmf] client packet: %u offered, %u stored, %u duplicate, %u rejected\n",
	       r.offered, r.stored, r.duplicate, r.rejected);

	if (r.offered > 0 && r.accepted() == r.offered) {
		// const_cast mirrors the callback contract: RNS hands the packet in as
		// const, but proving is the documented response to a delivery.
		const_cast<RNS::Packet&>(packet).prove();
	}
	else {
		printf("[lxmf] not proving: %u of %u message(s) could not be stored\n",
		       r.offered - r.accepted(), r.offered);
	}
}

// The resource path, for messages too large for a single link packet.
inline void lxmf_resource_concluded(const RNS::Resource& resource) {
	if (resource.status() != RNS::Type::Resource::COMPLETE) {
		printf("[lxmf] inbound resource ended in status %u, discarding\n",
		       (unsigned)resource.status());
		return;
	}
	LXMFIngestResult r = lxmf_ingest_container(resource.data());
	printf("[lxmf] client resource (%u bytes): %u offered, %u stored, %u duplicate, %u rejected\n",
	       (unsigned)resource.data().size(), r.offered, r.stored, r.duplicate, r.rejected);
}

// An inbound Resource is the one thing a stranger can make this node allocate
// for, and even the advertised 8 KB sync limit must be checked before accepting.
// Resource::accept() does not preallocate -- parts accumulate as they arrive --
// so cancelling at the start of the transfer bounds the memory a sender can
// cost us to roughly one window rather than the whole advertised size.
//
// Note this is deliberately NOT done with ACCEPT_APP. That strategy exists for
// exactly this purpose in Python, but in this C++ port the resource callback
// returns void and Link.cpp accepts unconditionally afterwards regardless, so
// ACCEPT_APP would read as a refusal that never refuses. Cancelling on start is
// honest about what actually stops the transfer. Making the callback's verdict
// count is an upstream change to microReticulum; see docs/Messaging.md.
inline void lxmf_resource_started(const RNS::Resource& resource) {
	size_t advertised = resource.total_size();
	if (advertised > LXMF_PN_SYNC_LIMIT_BYTES) {
		printf("[lxmf] refusing %u byte resource, over the %u byte sync limit\n",
		       (unsigned)advertised, (unsigned)LXMF_PN_SYNC_LIMIT_BYTES);
		const_cast<RNS::Resource&>(resource).cancel();
	}
}

// Every inbound link to the propagation destination must be ready for both
// shapes before the client sends anything, so both are wired at establishment.
inline void lxmf_link_established(RNS::Link& link) {
	link.set_packet_callback(lxmf_propagation_packet);
	link.set_resource_strategy(RNS::Type::Link::ACCEPT_ALL);
	link.set_resource_started_callback(lxmf_resource_started);
	link.set_resource_concluded_callback(lxmf_resource_concluded);
}

// --- Request handlers ------------------------------------------------------

// /offer is the peer-to-peer sync path: another propagation node offering us its
// message backlog. Clients never call it -- they push messages straight onto the
// link -- so declining costs us nothing a phone depends on.
//
// We decline deliberately rather than for lack of implementation. Accepting
// means taking a Linux node's 500 MB backlog into a 512 KB flash store, where it
// would immediately evict the local residents' messages that are the entire
// point of this node. `false` is the protocol's "I want none of these", so the
// offering peer keeps them and stays free to retry.
inline RNS::Bytes lxmf_offer_request(
	const RNS::Bytes& path, const RNS::Bytes& data, const RNS::Bytes& request_id,
	const RNS::Bytes& link_id, const RNS::Identity& remote_identity, double requested_at
) {
	if (!remote_identity) return lxmf_pn_error(LXMF_ERROR_NO_IDENTITY);
	printf("[lxmf] /offer from a peer declined: this node does not accept peer sync\n");
	MsgPack::Packer packer;
	packer.serialize(false);
	return RNS::Bytes(packer.data(), packer.size());
}

// /get serves a client its own waiting messages. Request is [want, have] with an
// optional third element giving the client's transfer limit in kilobytes:
//
//   want == nil and have == nil  ->  reply with the list of ids we hold for them
//   have = [ids]                 ->  the client has these; delete them
//   want = [ids]                 ->  reply with those messages
//
// A message belongs to whoever owns the matching *delivery* destination, which
// is derived from the identity the client proved on this link. Deriving it here
// rather than trusting anything in the request is what stops one client
// downloading another's mail.
inline RNS::Bytes lxmf_message_get_request(
	const RNS::Bytes& path, const RNS::Bytes& data, const RNS::Bytes& request_id,
	const RNS::Bytes& link_id, const RNS::Identity& remote_identity, double requested_at
) {
	if (!remote_identity) return lxmf_pn_error(LXMF_ERROR_NO_IDENTITY);

	RNS::Bytes client = RNS::Destination::hash(remote_identity, LXMF_APP_NAME, "delivery");

	MsgPack::Unpacker unpacker;
	unpacker.feed(data.data(), data.size());
	if (!unpacker.isArray()) return lxmf_pn_error(LXMF_ERROR_INVALID_DATA);
	size_t fields = unpacker.unpackArraySize();
	if (fields < 2) return lxmf_pn_error(LXMF_ERROR_INVALID_DATA);

	// --- element 0: wanted ids (or nil) ---
	bool want_is_nil = unpacker.isNil();
	std::vector<RNS::Bytes> wanted;
	if (want_is_nil) { (void)unpacker.unpackNil(); }
	else if (unpacker.isArray()) {
		size_t n = unpacker.unpackArraySize();
		for (size_t i = 0; i < n && unpacker.isBin(); i++) {
			MsgPack::bin_t<uint8_t> id = unpacker.unpackBinary<uint8_t>();
			wanted.push_back(RNS::Bytes(id.data(), id.size()));
		}
	}
	else return lxmf_pn_error(LXMF_ERROR_INVALID_DATA);

	// --- element 1: ids the client already holds, to be purged ---
	bool have_is_nil = unpacker.isNil();
	std::vector<RNS::Bytes> have;
	if (have_is_nil) { (void)unpacker.unpackNil(); }
	else if (unpacker.isArray()) {
		size_t n = unpacker.unpackArraySize();
		for (size_t i = 0; i < n && unpacker.isBin(); i++) {
			MsgPack::bin_t<uint8_t> id = unpacker.unpackBinary<uint8_t>();
			have.push_back(RNS::Bytes(id.data(), id.size()));
		}
	}
	else return lxmf_pn_error(LXMF_ERROR_INVALID_DATA);

	// --- element 2 (optional): the client's transfer limit, in kilobytes ---
	// The client advertises what *it* can receive; it does not get to raise the
	// node's own advertised ceiling. Python LXMF currently sends its 1000 KB
	// default here even when the propagation announce says 8 KB. Replacing our
	// limit with that value silently defeats the bound and recreates the >8 KB
	// Resource stalls this ceiling was introduced to avoid. A smaller client
	// value is honoured; a larger one is clamped to the node limit.
	size_t client_limit = LXMF_PN_RESPONSE_LIMIT_BYTES;
	if (fields >= 3) {
		double kb = 0;
		if      (unpacker.isFloat32()) kb = unpacker.unpackFloat32();
		else if (unpacker.isFloat64()) kb = unpacker.unpackFloat64();
		else if (unpacker.isUInt())    kb = (double)unpacker.unpackUInt64();
		else if (unpacker.isInt())     kb = (double)unpacker.unpackInt64();
		if (kb > 0) {
			size_t requested = (size_t)(kb * 1000);
			if (requested < client_limit) client_limit = requested;
		}
	}

	// A listing request: both fields nil.
	if (want_is_nil && have_is_nil) {
		std::vector<const LXMFEntry*> mine;
		for (const auto& e : lxmf_store_index) {
			if (e.destination_hash == client) mine.push_back(&e);
		}
		// Python sorts smallest first so a constrained client gets the most
		// messages it can rather than stalling on one large one.
		std::sort(mine.begin(), mine.end(),
		          [](const LXMFEntry* a, const LXMFEntry* b) { return a->size < b->size; });

		printf("[lxmf] /get list for <%s>: %u message(s)\n",
		       client.toHex().c_str(), (unsigned)mine.size());

		MsgPack::Packer packer;
		packer.serialize(MsgPack::arr_size_t(mine.size()));
		for (const LXMFEntry* e : mine) {
			packer.serialize(MsgPack::bin_t<uint8_t>(
				e->transient_id.data(), e->transient_id.data() + e->transient_id.size()));
		}
		return RNS::Bytes(packer.data(), packer.size());
	}

	// Purge what the client confirms it already has. Ownership is checked here
	// too, so a client cannot delete someone else's mail.
	unsigned purged = 0;
	for (const RNS::Bytes& tid : have) {
		for (size_t i = 0; i < lxmf_store_index.size(); i++) {
			const LXMFEntry& e = lxmf_store_index[i];
			if (e.transient_id != tid || e.destination_hash != client) continue;
			if (!RNS::Utilities::OS::remove_file(
			        lxmf_entry_path(e.transient_id, e.received).c_str())) {
				printf("[lxmf] FAILED to purge %s; keeping it in the store index\n",
				       e.transient_id.toHex().substr(0, 16).c_str());
				break;
			}
			lxmf_store_index.erase(lxmf_store_index.begin() + i);
			purged++;
			break;
		}
	}
	if (purged) {
		printf("[lxmf] /get purged %u message(s) for <%s>, %u held\n",
		       purged, client.toHex().c_str(), (unsigned)lxmf_store_index.size());
	}

	// Serve the wanted messages, stamps stripped, honouring the client's limit.
	std::vector<RNS::Bytes> out;
	size_t cumulative = 24;             // highest reasonable structure overhead
	const size_t per_message_overhead = 16;
	for (const RNS::Bytes& tid : wanted) {
		for (const auto& e : lxmf_store_index) {
			if (e.transient_id != tid || e.destination_hash != client) continue;
			RNS::Bytes blob;
			std::string fp = lxmf_entry_path(e.transient_id, e.received);
			if (RNS::Utilities::OS::read_file(fp.c_str(), blob) <= LXMF_STAMP_SIZE) break;
			size_t next = cumulative + blob.size() + per_message_overhead;
			if (next > client_limit) break;     // client re-requests the rest
			out.push_back(blob.left(blob.size() - LXMF_STAMP_SIZE));
			cumulative = next;
			break;
		}
	}

	printf("[lxmf] /get serving %u of %u requested message(s) to <%s>\n",
	       (unsigned)out.size(), (unsigned)wanted.size(), client.toHex().c_str());

	MsgPack::Packer packer;
	packer.serialize(MsgPack::arr_size_t(out.size()));
	for (const RNS::Bytes& m : out) {
		packer.serialize(MsgPack::bin_t<uint8_t>(m.data(), m.data() + m.size()));
	}
	return RNS::Bytes(packer.data(), packer.size());
}

// Announce the propagation node periodically so clients can discover it.
inline void lxmf_propagation_announce_watch() {
	static uint32_t last = 0;
	static uint32_t jitter = 0;
	static bool armed = false;
	static bool first_done = false;
	if (!lxmf_propagation_destination) return;
	// Jitter is rolled when arming as well as after each announce, for the same
	// reason as the NomadNet announce: a block of nodes restored to mains power
	// all reach this point together and must not transmit in lockstep.
	if (!armed) {
		armed = true; last = millis();
		jitter = (uint32_t)random(LXMF_PN_ANNOUNCE_JITTER_MS);
		return;
	}
	// The first announce cannot wait for the full interval. Until a node
	// announces, clients cannot discover it *or* learn the stamp cost they must
	// pay to use it -- so a node that stays quiet for half an hour after boot is
	// simply not a propagation node yet. That is worst precisely when it matters
	// most: after a power restore, when every node in a building is silent at
	// once. Announce shortly after boot, once the network is definitely up, then
	// settle into the normal interval.
	uint32_t due = first_done ? LXMF_PN_ANNOUNCE_INTERVAL_MS : LXMF_PN_FIRST_ANNOUNCE_MS;
	due += jitter;
	if (millis() - last < due) return;
	last = millis();
	jitter = (uint32_t)random(LXMF_PN_ANNOUNCE_JITTER_MS);
	first_done = true;
	RNS::Bytes app_data = lxmf_pn_app_data();
	lxmf_propagation_destination.announce(app_data);
	printf("[lxmf] announced propagation node (%u byte app_data)\n",
	       (unsigned)app_data.size());
}
