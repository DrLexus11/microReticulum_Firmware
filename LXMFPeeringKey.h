// Peering-key proof of work for LXMF propagation-node sync.
//
// A propagation node will not accept an /offer without a valid peering key, and
// answers ERROR_INVALID_KEY (0xf3) without one. From LXMRouter.offer_request:
//
//   peering_id = self.identity.hash + remote_identity.hash   // peer's, then ours
//   valid      = LXStamper.validate_peering_key(peering_id, key, peering_cost)
//
// and from LXStamper:
//
//   workblock = concat over n in 0..24 of
//               HKDF(length=256, derive_from=peering_id,
//                    salt=full_hash(peering_id + msgpack(n)))
//   valid     = int(SHA256(workblock || key)) <= (1 << (256 - cost))
//
// So the key is 32 bytes whose hash, appended to a 6400-byte workblock derived
// from the two identities, has at least `cost` leading zero bits.
//
// TWO THINGS MAKE THIS AFFORDABLE ON AN ESP32.
//
// The workblock is fixed for a given pair of identities, so the key can be
// computed once per peer and cached; it does not depend on the messages being
// offered.
//
// And the workblock is exactly 100 SHA-256 blocks of 64 bytes, so the hash
// midstate after it can be computed once and cloned for each attempt. Without
// that, 2^18 attempts would each hash 6432 bytes -- about 1.7 GB, minutes of
// solid work. With it, each attempt costs a single block.
//
// The search runs in bounded chunks driven from the main loop rather than in one
// blocking call. Spinning for seconds inside loop() is exactly how this project
// tripped "Stack canary watchpoint triggered (loopTask)" and the task watchdog.

#pragma once

#include "LXMFPropagation.h"

#if defined(HAS_RNS) && defined(LXMF_PROPAGATION_NODE)

#include <mbedtls/sha256.h>
// hkdf is not surfaced by the umbrella header.
#include <microReticulum/Cryptography/HKDF.h>

// LXStamper.WORKBLOCK_EXPAND_ROUNDS_PEERING. Far cheaper than the 3000 rounds
// used for message stamps -- peering is negotiated once, not per message.
#define LXMF_PEERING_EXPAND_ROUNDS 25
#define LXMF_PEERING_HKDF_BYTES    256
#define LXMF_PEERING_WORKBLOCK_LEN (LXMF_PEERING_EXPAND_ROUNDS * LXMF_PEERING_HKDF_BYTES)
#define LXMF_PEERING_KEY_SIZE      32

// Attempts per main-loop call. Each is one SHA-256 block from a cloned
// midstate, so a chunk is well under a millisecond and the loop keeps turning.
#ifndef LXMF_PEERING_ROUNDS_PER_STEP
#define LXMF_PEERING_ROUNDS_PER_STEP 4096
#endif

// Give up rather than search forever. 2^18 is the expected count at cost 18;
// this allows a wide margin over that and still terminates.
#ifndef LXMF_PEERING_MAX_ROUNDS
#define LXMF_PEERING_MAX_ROUNDS 4000000
#endif

struct LXMFPeeringKeyJob {
	bool       active = false;
	bool       failed = false;
	uint8_t    cost   = 0;
	uint32_t   round  = 0;
	uint8_t*   workblock = nullptr;
	mbedtls_sha256_context base;
	RNS::Bytes peer;          // which peer this job is for
	RNS::Bytes key;           // result, when found
};

inline LXMFPeeringKeyJob& lxmf_peering_job() {
	static LXMFPeeringKeyJob job;
	return job;
}

inline void lxmf_peering_job_release() {
	LXMFPeeringKeyJob& j = lxmf_peering_job();
	if (j.workblock != nullptr) {
		mbedtls_sha256_free(&j.base);
		free(j.workblock);
		j.workblock = nullptr;
	}
	j.active = false;
	j.round  = 0;
}

// Build the 6400-byte workblock and the SHA-256 midstate over it.
inline bool lxmf_peering_job_begin(const RNS::Bytes& peer_dest_hash,
                                   const RNS::Bytes& peering_id, uint8_t cost) {
	lxmf_peering_job_release();
	LXMFPeeringKeyJob& j = lxmf_peering_job();

	j.workblock = (uint8_t*)malloc(LXMF_PEERING_WORKBLOCK_LEN);
	if (j.workblock == nullptr) {
		printf("[lxmf-key] cannot allocate %d bytes for the workblock\n",
		       LXMF_PEERING_WORKBLOCK_LEN);
		return false;
	}

	for (uint8_t n = 0; n < LXMF_PEERING_EXPAND_ROUNDS; n++) {
		// salt = full_hash(peering_id + msgpack(n)). msgpack encodes 0..127 as
		// a single byte equal to the value, so appending n is the encoding.
		RNS::Bytes material(peering_id);
		material.append(&n, 1);
		const RNS::Bytes salt = RNS::Identity::full_hash(material);
		const RNS::Bytes block =
			RNS::Cryptography::hkdf(LXMF_PEERING_HKDF_BYTES, peering_id, salt);
		if (block.size() != LXMF_PEERING_HKDF_BYTES) {
			printf("[lxmf-key] hkdf returned %u bytes, expected %d\n",
			       (unsigned)block.size(), LXMF_PEERING_HKDF_BYTES);
			lxmf_peering_job_release();
			return false;
		}
		memcpy(j.workblock + (size_t)n * LXMF_PEERING_HKDF_BYTES,
		       block.data(), LXMF_PEERING_HKDF_BYTES);
	}

	mbedtls_sha256_init(&j.base);
	// The _ret variants are the ones that report failure; the plain names are
	// deprecated wrappers returning void in this mbedtls.
	if (mbedtls_sha256_starts_ret(&j.base, 0) != 0 ||
	    mbedtls_sha256_update_ret(&j.base, j.workblock, LXMF_PEERING_WORKBLOCK_LEN) != 0) {
		printf("[lxmf-key] sha256 midstate failed\n");
		lxmf_peering_job_release();
		return false;
	}

	j.peer   = peer_dest_hash;
	j.cost   = cost;
	j.round  = 0;
	j.failed = false;
	j.key    = RNS::Bytes();
	j.active = true;
	printf("[lxmf-key] searching for a cost-%u peering key for <%s>\n",
	       (unsigned)cost, peer_dest_hash.toHex().substr(0, 16).c_str());
	return true;
}

// Valid when the digest has at least `cost` leading zero bits. LXMF's test is
// int(digest) <= (1 << (256 - cost)); leading zeros give int(digest) <
// 2^(256-cost), which satisfies it.
inline bool lxmf_peering_digest_ok(const uint8_t* digest, uint8_t cost) {
	uint8_t full = cost / 8;
	uint8_t rem  = cost % 8;
	for (uint8_t i = 0; i < full; i++) if (digest[i] != 0) return false;
	if (rem != 0 && (digest[full] >> (8 - rem)) != 0) return false;
	return true;
}

// One bounded chunk. Returns true when the job has concluded, either way.
inline bool lxmf_peering_job_step() {
	LXMFPeeringKeyJob& j = lxmf_peering_job();
	if (!j.active) return true;

	uint8_t stamp[LXMF_PEERING_KEY_SIZE];
	uint8_t digest[32];
	memset(stamp, 0, sizeof(stamp));

	for (uint32_t i = 0; i < LXMF_PEERING_ROUNDS_PER_STEP; i++) {
		const uint32_t r = j.round++;
		if (r >= LXMF_PEERING_MAX_ROUNDS) {
			printf("[lxmf-key] gave up after %lu rounds\n", (unsigned long)r);
			j.failed = true;
			lxmf_peering_job_release();
			return true;
		}
		// The stamp is any 32 bytes; a counter is as good as random here and is
		// reproducible, which makes a failure repeatable.
		memcpy(stamp, &r, sizeof(r));

		mbedtls_sha256_context ctx;
		mbedtls_sha256_init(&ctx);
		mbedtls_sha256_clone(&ctx, &j.base);
		mbedtls_sha256_update_ret(&ctx, stamp, sizeof(stamp));
		mbedtls_sha256_finish_ret(&ctx, digest);
		mbedtls_sha256_free(&ctx);

		if (lxmf_peering_digest_ok(digest, j.cost)) {
			j.key = RNS::Bytes(stamp, sizeof(stamp));
			printf("[lxmf-key] found a key in %lu rounds\n", (unsigned long)(r + 1));
			// Keep the result; release only the workblock and midstate.
			mbedtls_sha256_free(&j.base);
			free(j.workblock);
			j.workblock = nullptr;
			j.active = false;
			return true;
		}
	}
	return false;   // still searching
}

#endif // HAS_RNS && LXMF_PROPAGATION_NODE
