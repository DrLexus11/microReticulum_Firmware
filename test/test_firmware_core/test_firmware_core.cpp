// Host unit tests for the firmware's pure modules (PR F step F1, R1 of
// docs/IoTPlatform.md). Only headers with no Arduino, radio or Reticulum
// dependency are compiled here, under AddressSanitizer and UBSan, so a codec
// change is caught on the bench PC before it reaches a board. Byte layouts are
// pinned exactly: these are wire formats, and other implementations (the deck's
// Python, Columba's Kotlin) must read the same bytes.

#include <unity.h>

#include "BootLog.h"
#include "BLEPeerProtocol.h"
#include "ESPNowProtocol.h"

#include <cstdint>
#include <cstring>
#include <string>

void setUp() {}
void tearDown() {}

// --- BootLog.h: the bootlog ring keeps its newest whole lines ----------------

static size_t tail_start(const std::string& log, size_t keep) {
	return bootlog_tail_start(log.data(), log.size(), keep);
}

void test_bootlog_that_fits_is_kept_whole() {
	TEST_ASSERT_EQUAL_size_t(0, tail_start("a\nb\n", 64));
}

void test_bootlog_trim_starts_at_a_line_and_keeps_the_newest() {
	const std::string log = "boot reason=POWERON\nboot reason=TASK_WDT prev=9796s\nboot reason=SW prev=0s\n";
	const size_t start = tail_start(log, 30);
	TEST_ASSERT_TRUE(start > 0);
	TEST_ASSERT_EQUAL_CHAR('\n', log[start - 1]);          // never half a line
	TEST_ASSERT_TRUE(log.size() - start <= 30);              // within the budget
	TEST_ASSERT_EQUAL_STRING("boot reason=SW prev=0s\n", log.c_str() + start);
}

void test_bootlog_line_longer_than_the_budget_keeps_nothing() {
	// No line boundary inside the newest `keep` bytes: a partial line is worse
	// than none, so the ring keeps nothing rather than a fragment.
	const std::string log = "short\n" + std::string(100, 'x') + "\n";
	TEST_ASSERT_EQUAL_size_t(log.size(), tail_start(log, 20));
}

// --- ESPNowProtocol.h ---------------------------------------------------------

void test_espnow_header_bytes_are_pinned() {
	uint8_t out[ESPNOW_HEADER_SIZE];
	const ESPNowFrameHeader header{ESPNOW_FRAME_DATA, 0x1234, 1, 3, 0x0234};
	TEST_ASSERT_EQUAL_size_t(ESPNOW_HEADER_SIZE, espnow_write_header(out, sizeof(out), header));
	const uint8_t expected[ESPNOW_HEADER_SIZE] = {0x52, 0x4e, 0x01, 0x02, 0x12, 0x34, 0x01, 0x03, 0x02, 0x34};
	TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, ESPNOW_HEADER_SIZE);

	ESPNowFrameHeader back{};
	TEST_ASSERT_TRUE(espnow_read_header(out, sizeof(out), back));
	TEST_ASSERT_EQUAL_UINT8(ESPNOW_FRAME_DATA, back.type);
	TEST_ASSERT_EQUAL_UINT16(0x1234, back.packet_id);
	TEST_ASSERT_EQUAL_UINT8(1, back.fragment_index);
	TEST_ASSERT_EQUAL_UINT8(3, back.fragment_count);
	TEST_ASSERT_EQUAL_UINT16(0x0234, back.total_length);
}

void test_espnow_header_rejects_foreign_or_short_frames() {
	uint8_t out[ESPNOW_HEADER_SIZE];
	espnow_write_header(out, sizeof(out), ESPNowFrameHeader{ESPNOW_FRAME_DATA, 1, 0, 1, 10});
	ESPNowFrameHeader back{};
	TEST_ASSERT_FALSE(espnow_read_header(out, ESPNOW_HEADER_SIZE - 1, back));
	out[0] = 0x00;
	TEST_ASSERT_FALSE(espnow_read_header(out, sizeof(out), back));
	out[0] = ESPNOW_PROTO_MAGIC_0;
	out[2] = ESPNOW_PROTO_VERSION + 1;
	TEST_ASSERT_FALSE(espnow_read_header(out, sizeof(out), back));
	TEST_ASSERT_EQUAL_size_t(0, espnow_write_header(out, ESPNOW_HEADER_SIZE - 1,
	                                                ESPNowFrameHeader{ESPNOW_FRAME_DATA, 1, 0, 1, 10}));
}

void test_espnow_discovery_round_trips_at_its_exact_size() {
	const ESPNowDiscovery discovery{0x33dbd294, 867200000, 250000, 7, 5, 1,
	                                ESPNOW_CAP_LORA | ESPNOW_CAP_TRANSPORT | ESPNOW_CAP_UPSTREAM};
	uint8_t out[ESPNOW_DISCOVERY_SIZE + 4];
	TEST_ASSERT_EQUAL_size_t(ESPNOW_DISCOVERY_SIZE, espnow_write_discovery(out, sizeof(out), discovery));
	ESPNowDiscovery back{};
	TEST_ASSERT_TRUE(espnow_read_discovery(out, ESPNOW_DISCOVERY_SIZE, back));
	TEST_ASSERT_EQUAL_UINT32(discovery.phy_hash, back.phy_hash);
	TEST_ASSERT_EQUAL_UINT32(discovery.frequency, back.frequency);
	TEST_ASSERT_EQUAL_UINT32(discovery.bandwidth, back.bandwidth);
	TEST_ASSERT_EQUAL_UINT8(7, back.spreading_factor);
	TEST_ASSERT_EQUAL_UINT8(5, back.coding_rate);
	TEST_ASSERT_EQUAL_UINT8(1, back.wifi_channel);
	TEST_ASSERT_EQUAL_UINT8(discovery.capabilities, back.capabilities);
	// The whole payload against a vector computed independently (Python's
	// struct.pack(">IIIBBBB", ...)), so an encoder and decoder that drift
	// together still fail: phy_hash, frequency, bandwidth big-endian, then
	// SF, CR, Wi-Fi channel and capabilities.
	const uint8_t expected[ESPNOW_DISCOVERY_SIZE] = {
		0x33, 0xdb, 0xd2, 0x94,  0x33, 0xb0, 0x6c, 0x00,  0x00, 0x03, 0xd0, 0x90,
		0x07, 0x05, 0x01, 0x0b};
	TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, ESPNOW_DISCOVERY_SIZE);
	// Any other length is a different frame, not a short discovery.
	TEST_ASSERT_FALSE(espnow_read_discovery(out, ESPNOW_DISCOVERY_SIZE + 1, back));
	TEST_ASSERT_FALSE(espnow_read_discovery(out, ESPNOW_DISCOVERY_SIZE - 1, back));
}

void test_espnow_recovery_reply_carries_nonce_discovery_and_proof() {
	const ESPNowDiscovery discovery{0x01020304, 868000000, 125000, 9, 5, 6, ESPNOW_CAP_IFAC_PROOF};
	const uint8_t proof[ESPNOW_RECOVERY_PROOF_SIZE] = {1, 2, 3, 4, 5, 6, 7, 8};
	uint8_t out[ESPNOW_RECOVERY_REPLY_SIZE];
	TEST_ASSERT_EQUAL_size_t(ESPNOW_RECOVERY_REPLY_SIZE,
	                         espnow_write_recovery_reply(out, sizeof(out), 0xdeadbeef, discovery, proof));
	// The bytes, independently computed: nonce, the discovery, the proof.
	const uint8_t expected[ESPNOW_RECOVERY_REPLY_SIZE] = {
		0xde, 0xad, 0xbe, 0xef,
		0x01, 0x02, 0x03, 0x04,  0x33, 0xbc, 0xa1, 0x00,  0x00, 0x01, 0xe8, 0x48,
		0x09, 0x05, 0x06, 0x04,
		1, 2, 3, 4, 5, 6, 7, 8};
	TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, ESPNOW_RECOVERY_REPLY_SIZE);
	// And through the parser ESPNowInterface uses.
	uint32_t nonce = 0;
	ESPNowDiscovery back{};
	const uint8_t* back_proof = nullptr;
	TEST_ASSERT_TRUE(espnow_read_recovery_reply(out, sizeof(out), nonce, back, back_proof));
	TEST_ASSERT_EQUAL_HEX32(0xdeadbeef, nonce);
	TEST_ASSERT_EQUAL_HEX32(0x01020304, back.phy_hash);
	TEST_ASSERT_EQUAL_UINT32(868000000, back.frequency);
	TEST_ASSERT_EQUAL_UINT32(125000, back.bandwidth);
	TEST_ASSERT_EQUAL_UINT8(9, back.spreading_factor);
	TEST_ASSERT_EQUAL_UINT8(5, back.coding_rate);
	TEST_ASSERT_EQUAL_UINT8(6, back.wifi_channel);
	TEST_ASSERT_EQUAL_UINT8(ESPNOW_CAP_IFAC_PROOF, back.capabilities);
	TEST_ASSERT_NOT_NULL(back_proof);
	TEST_ASSERT_EQUAL_UINT8_ARRAY(proof, back_proof, ESPNOW_RECOVERY_PROOF_SIZE);
	// Exactly its size: one byte short or long is another frame.
	TEST_ASSERT_FALSE(espnow_read_recovery_reply(out, sizeof(out) - 1, nonce, back, back_proof));
	TEST_ASSERT_EQUAL_size_t(0, espnow_write_recovery_reply(out, sizeof(out), 1, discovery, nullptr));
}

void test_espnow_rns_mtu_fits_its_fragment_budget() {
	TEST_ASSERT_EQUAL_INT(240, ESPNOW_FRAGMENT_PAYLOAD);
	TEST_ASSERT_EQUAL_INT(3, ESPNOW_MAX_FRAGMENTS);
	TEST_ASSERT_TRUE(ESPNOW_MAX_FRAGMENTS * ESPNOW_FRAGMENT_PAYLOAD >= ESPNOW_RNS_MTU);
}

// --- BLEPeerProtocol.h --------------------------------------------------------

void test_ble_usable_length_is_clamped_to_the_att_limits() {
	TEST_ASSERT_EQUAL_size_t(20, ble_peer_usable_value_length(23));    // minimum MTU
	TEST_ASSERT_EQUAL_size_t(20, ble_peer_usable_value_length(10));    // below it: floor
	TEST_ASSERT_EQUAL_size_t(97, ble_peer_usable_value_length(100));
	TEST_ASSERT_EQUAL_size_t(512, ble_peer_usable_value_length(517));  // attribute cap
}

void test_ble_payload_leaves_room_for_the_header() {
	TEST_ASSERT_EQUAL_size_t(15, ble_peer_payload_size(20));
	TEST_ASSERT_EQUAL_size_t(507, ble_peer_payload_size(512));
	TEST_ASSERT_EQUAL_size_t(1, ble_peer_payload_size(BLE_PEER_HEADER_SIZE));  // never zero
}

void test_ble_header_bytes_are_network_order() {
	uint8_t out[BLE_PEER_HEADER_SIZE];
	ble_peer_write_header(out, BLE_PEER_TYPE_CONTINUE, 0x0102, 0x0304);
	const uint8_t expected[BLE_PEER_HEADER_SIZE] = {0x02, 0x01, 0x02, 0x03, 0x04};
	TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, BLE_PEER_HEADER_SIZE);
}

static bool ble_read(uint8_t type, uint16_t seq, uint16_t total) {
	uint8_t out[BLE_PEER_HEADER_SIZE];
	ble_peer_write_header(out, type, seq, total);
	uint8_t t; uint16_t s, n;
	return ble_peer_read_header(out, sizeof(out), t, s, n);
}

void test_ble_header_accepts_what_reassembly_can_use() {
	TEST_ASSERT_TRUE(ble_read(BLE_PEER_TYPE_LONE, 0, 0));
	TEST_ASSERT_TRUE(ble_read(BLE_PEER_TYPE_LONE, 0, 1));       // either spelling of "whole"
	TEST_ASSERT_TRUE(ble_read(BLE_PEER_TYPE_START, 0, 3));
	TEST_ASSERT_TRUE(ble_read(BLE_PEER_TYPE_CONTINUE, 1, 3));
	TEST_ASSERT_TRUE(ble_read(BLE_PEER_TYPE_END, 2, 3));
}

void test_ble_header_refuses_what_would_assemble_a_short_packet() {
	TEST_ASSERT_FALSE(ble_read(BLE_PEER_TYPE_LONE, 1, 1));
	TEST_ASSERT_FALSE(ble_read(BLE_PEER_TYPE_LONE, 0, 2));
	TEST_ASSERT_FALSE(ble_read(BLE_PEER_TYPE_START, 1, 3));     // a START is fragment zero
	TEST_ASSERT_FALSE(ble_read(BLE_PEER_TYPE_CONTINUE, 3, 3));  // seq past the total
	TEST_ASSERT_FALSE(ble_read(BLE_PEER_TYPE_END, 0, 0));
	TEST_ASSERT_FALSE(ble_read(0x07, 0, 1));                    // unknown type
	uint8_t out[BLE_PEER_HEADER_SIZE] = {0};
	uint8_t t; uint16_t s, n;
	TEST_ASSERT_FALSE(ble_peer_read_header(out, BLE_PEER_HEADER_SIZE - 1, t, s, n));
}

int main() {
	UNITY_BEGIN();
	RUN_TEST(test_bootlog_that_fits_is_kept_whole);
	RUN_TEST(test_bootlog_trim_starts_at_a_line_and_keeps_the_newest);
	RUN_TEST(test_bootlog_line_longer_than_the_budget_keeps_nothing);
	RUN_TEST(test_espnow_header_bytes_are_pinned);
	RUN_TEST(test_espnow_header_rejects_foreign_or_short_frames);
	RUN_TEST(test_espnow_discovery_round_trips_at_its_exact_size);
	RUN_TEST(test_espnow_recovery_reply_carries_nonce_discovery_and_proof);
	RUN_TEST(test_espnow_rns_mtu_fits_its_fragment_budget);
	RUN_TEST(test_ble_usable_length_is_clamped_to_the_att_limits);
	RUN_TEST(test_ble_payload_leaves_room_for_the_header);
	RUN_TEST(test_ble_header_bytes_are_network_order);
	RUN_TEST(test_ble_header_accepts_what_reassembly_can_use);
	RUN_TEST(test_ble_header_refuses_what_would_assemble_a_short_packet);
	return UNITY_END();
}
