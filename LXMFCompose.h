// Composing LXMF messages on the node, for the RRC-to-LXMF bridge.
//
// WHY THIS EXISTS
//
// LXMFPropagation.h deliberately never parses a message: a propagation node
// stores opaque blobs and that is what makes it fit on a microcontroller. The
// bridge breaks that symmetry in one direction only. To let a responder who was
// out of range receive what a bridged room said while they were gone, the hub
// has to *produce* an LXMF message rather than relay someone else's, which
// means packing, signing and encrypting one here.
//
// This is the only place in the firmware that constructs LXMF. Keep it that
// way: everything else should continue to treat messages as opaque.
//
// BIT-COMPATIBILITY IS THE WHOLE RISK, and it is worse here than on the store
// path. A malformed stored blob fails visibly at sync; a malformed *composed*
// message syncs perfectly, decrypts to garbage or fails a signature check
// inside someone else's client, and is discarded there with no error reaching
// us. tests/test_lxmf_protocol.py asserts this layout against the Python
// reference so a divergence fails on a workstation instead of in a stairwell.
//
// THE LAYOUT, from LXMF/LXMessage.py pack() and its PROPAGATED branch:
//
//   payload      = [ timestamp, title, content, fields ]
//   hashed_part  = dest.hash + source.hash + msgpack(payload)
//   hash         = full_hash(hashed_part)
//   signed_part  = hashed_part + hash
//   signature    = source.sign(signed_part)
//   packed       = dest.hash + source.hash + signature + msgpack(payload)
//   lxmf_data    = packed[:16] + dest.encrypt(packed[16:])
//   transient_id = full_hash(lxmf_data)
//   blob         = lxmf_data + propagation_stamp
//
// Two details that are easy to get wrong and silent when wrong:
//
//   - the payload is [timestamp, TITLE, CONTENT, fields]. Title precedes
//     content. Swapping them produces a message that decrypts and validates
//     cleanly and displays with the body in the subject line.
//   - `dest.hash` and `source.hash` are *delivery destination* hashes
//     (identity + "lxmf"/"delivery"), not identity hashes. They differ, and
//     using an identity hash yields a message no client will ever match to a
//     conversation.

#pragma once

#include <microReticulum.h>
#include <MsgPack.h>

#include "LXMFPropagation.h"

// MsgPack's packFloat64() quietly degrades to FLOAT32 on toolchains without
// libstdc++11, which would change the timestamp's wire type and make every
// composed message unreadable to a reference client. Fail the build instead.
#if defined(ARX_HAVE_LIBSTDCPLUSPLUS) && (ARX_HAVE_LIBSTDCPLUSPLUS < 201103L)
#error "LXMF timestamps must pack as FLOAT64; this toolchain would emit FLOAT32"
#endif

#define LXMF_DELIVERY_ASPECT "delivery"

// Ed25519, as Identity::sign() produces. Named here because a short signature
// is one of the ways a composed message goes silently wrong.
#define LXMF_SIGNATURE_LEN (RNS::Type::Identity::SIGLENGTH / 8)

// The propagation stamp is a proof-of-work admission gate between a sender and
// a propagation node, and it is stripped again before the message is served to
// its recipient -- Python does this at LXMRouter.py:1549, and so does
// lxmf_message_get_request() here. A message the hub composes is inserted
// straight into this node's own store and never crosses that gate, so no work
// needs proving and a zero stamp costs the recipient nothing: what leaves this
// node on /get is byte-identical either way.
//
// This stops being true if we ever accept peer sync (FeatureRoadmap item 4a):
// a peer validates the stamp against its own advertised cost before storing,
// and would reject these. Generate a real stamp before enabling peering, not
// after wondering why a peer holds none of our room traffic.
#ifndef LXMF_BRIDGE_STAMP_VALUE
#define LXMF_BRIDGE_STAMP_VALUE 0x00
#endif

// Pack the LXMF payload array. Kept separate so the test can compare exactly
// this against msgpack.packb() without the surrounding crypto.
// `fields` is raw msgpack for the fields map, spliced in whole. Passing it
// pre-packed keeps this function free of any knowledge of what a caller wants
// to attach, and lets the expensive part -- building the map -- happen
// wherever the data already lives rather than here.
inline RNS::Bytes lxmf_pack_payload(double timestamp, const RNS::Bytes& title,
                                    const RNS::Bytes& content,
                                    const RNS::Bytes& fields = RNS::Bytes()) {
	// An empty Bytes has a null data pointer, and an empty title is the normal
	// case for room traffic, so never hand that pointer to packBinary().
	static const uint8_t empty = 0;
	const uint8_t* title_data = title.size() ? title.data() : &empty;
	const uint8_t* content_data = content.size() ? content.data() : &empty;

	MsgPack::Packer packer;
	packer.serialize(MsgPack::arr_size_t(4));
	packer.serialize(timestamp);                     // 0: seconds since epoch, float64
	packer.packBinary(title_data, title.size());     // 1: title
	packer.packBinary(content_data, content.size()); // 2: content
	if (!fields) packer.serialize(MsgPack::map_size_t(0));  // 3: fields (none)
	RNS::Bytes packed(packer.data(), packer.size());
	if (fields) packed << fields;                    // 3: fields, pre-packed
	return packed;
}

// Build the signed, unencrypted message. Exposed for testing; the bridge wants
// lxmf_compose_propagated() below.
inline RNS::Bytes lxmf_compose_packed(const RNS::Identity& source_identity,
                                      const RNS::Bytes& source_destination_hash,
                                      const RNS::Bytes& destination_hash,
                                      double timestamp, const RNS::Bytes& title,
                                      const RNS::Bytes& content,
                                      const RNS::Bytes& fields = RNS::Bytes()) {
	if (!source_identity ||
	    source_destination_hash.size() != LXMF_DESTINATION_LEN ||
	    destination_hash.size() != LXMF_DESTINATION_LEN) {
		return RNS::Bytes();
	}

	const RNS::Bytes payload = lxmf_pack_payload(timestamp, title, content, fields);

	RNS::Bytes hashed_part;
	hashed_part << destination_hash << source_destination_hash << payload;
	const RNS::Bytes hash = RNS::Identity::full_hash(hashed_part);

	RNS::Bytes signed_part;
	signed_part << hashed_part << hash;
	const RNS::Bytes signature = source_identity.sign(signed_part);
	if (signature.size() != LXMF_SIGNATURE_LEN) return RNS::Bytes();

	RNS::Bytes packed;
	packed << destination_hash << source_destination_hash << signature << payload;
	return packed;
}

// Build the blob the propagation store holds: encrypted to the recipient, with
// the trailing stamp the store's transient-id arithmetic expects.
//
// `recipient` needs only a public key, which is exactly what a Link hands over
// when the remote end identifies -- so the hub can address a member it has met
// without ever having seen their announce.
inline RNS::Bytes lxmf_compose_propagated(const RNS::Identity& source_identity,
                                          const RNS::Bytes& source_destination_hash,
                                          RNS::Destination& recipient_destination,
                                          double timestamp, const RNS::Bytes& title,
                                          const RNS::Bytes& content,
                                          const RNS::Bytes& fields = RNS::Bytes()) {
	const RNS::Bytes packed = lxmf_compose_packed(
		source_identity, source_destination_hash, recipient_destination.hash(),
		timestamp, title, content, fields);
	if (!packed) return RNS::Bytes();

	// Everything after the destination hash is encrypted to the recipient; the
	// hash stays in clear because that is how the store and every node between
	// here and them knows who it is for.
	const RNS::Bytes ciphertext =
		recipient_destination.encrypt(packed.mid(LXMF_DESTINATION_LEN));
	if (!ciphertext) return RNS::Bytes();

	RNS::Bytes blob;
	blob << packed.left(LXMF_DESTINATION_LEN) << ciphertext;
	for (size_t i = 0; i < LXMF_STAMP_SIZE; i++) {
		blob.append((uint8_t)LXMF_BRIDGE_STAMP_VALUE);
	}
	return blob;
}
