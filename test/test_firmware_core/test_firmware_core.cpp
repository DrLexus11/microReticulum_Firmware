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
#include "PositionCodec.h"

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

// --- PositionCodec.h: the compact position report, v2 ------------------------
// Every hex string below is a case in tests/fixtures/position_v2.json, which
// tools/position_codec.py (the deck's decoder) is tested against too, and
// tests/test_position_fixture.py checks that each one appears here. One wire
// format, three readers, the same bytes.

static std::string hex_of(const uint8_t* data, size_t length) {
	static const char digits[] = "0123456789abcdef";
	std::string out;
	for (size_t i = 0; i < length; ++i) {
		out += digits[data[i] >> 4];
		out += digits[data[i] & 0x0f];
	}
	return out;
}

static size_t bytes_of(const char* hex, uint8_t* out, size_t capacity) {
	size_t n = 0;
	for (; hex[0] && hex[1] && n < capacity; hex += 2) {
		auto nibble = [](char c) { return (uint8_t)(c <= '9' ? c - '0' : c - 'a' + 10); };
		out[n++] = (uint8_t)(nibble(hex[0]) << 4 | nibble(hex[1]));
	}
	return n;
}

static std::string encoded(const NodePositionFix& fix) {
	uint8_t out[POSITION_WIRE_MAX_LEN];
	const size_t n = position_report_encode(fix, out, sizeof(out));
	return hex_of(out, n);
}

void test_position_minimal_report_bytes() {
	NodePositionFix fix;
	fix.valid = true;
	fix.sender_id = 0x0a0b0c0d;
	fix.lat_e7 = 123456789;
	fix.lon_e7 = -98765432;
	fix.fix_unix_ms = 1790000000ULL * 1000ULL;
	fix.accuracy_m = 12;
	TEST_ASSERT_EQUAL_STRING("02000a0b0c0d075bcd15fa1cf5886ab13b800c", encoded(fix).c_str());
}

void test_position_all_fields_saturate_and_normalise() {
	NodePositionFix fix;
	fix.valid = true;
	fix.sender_id = 0xfedcba98;
	fix.lat_e7 = -123456789;
	fix.lon_e7 = 98765432;
	fix.fix_unix_ms = 1790000123ULL * 1000ULL;
	fix.accuracy_m = 400;                       // over 254: saturates to 255
	fix.alt_known = true;
	fix.alt_m = -12;
	fix.course_known = true;
	fix.course_ddeg = 3600;                     // a full turn is due north, 0
	fix.speed_cms = 20000;                      // 400 half-m/s units: saturates
	fix.sats = 9;
	TEST_ASSERT_EQUAL_STRING("020ffedcba98f8a432eb05e30a786ab13bfbfffff400ff09", encoded(fix).c_str());
}

void test_position_course_and_speed_scale() {
	NodePositionFix fix;
	fix.valid = true;
	fix.sender_id = 1;
	fix.lat_e7 = 1;
	fix.lon_e7 = -1;
	fix.accuracy_m = 254;
	fix.course_known = true;
	fix.course_ddeg = 1795;                     // 2-degree units: 89
	fix.speed_cms = 149;                        // half-m/s units: 2
	TEST_ASSERT_EQUAL_STRING("02060000000100000001ffffffff00000000fe5902", encoded(fix).c_str());
}

void test_position_invalid_fix_or_short_buffer_encodes_nothing() {
	NodePositionFix fix;                        // valid = false
	uint8_t out[POSITION_WIRE_MAX_LEN];
	TEST_ASSERT_EQUAL_size_t(0, position_report_encode(fix, out, sizeof(out)));
	fix.valid = true;
	TEST_ASSERT_EQUAL_size_t(0, position_report_encode(fix, out, POSITION_WIRE_BASE_LEN - 1));
}

void test_position_decode_ignores_the_interval_byte() {
	uint8_t in[POSITION_WIRE_MAX_LEN];
	const size_t n = bytes_of("0218000000070000000a000000140000001e05040a", in, sizeof(in));
	NodePositionFix fix;
	TEST_ASSERT_TRUE(position_report_decode(in, n, fix));
	TEST_ASSERT_TRUE(fix.valid);
	TEST_ASSERT_EQUAL_HEX32(7, fix.sender_id);
	TEST_ASSERT_EQUAL_INT32(10, fix.lat_e7);
	TEST_ASSERT_EQUAL_INT32(20, fix.lon_e7);
	TEST_ASSERT_EQUAL_UINT64(30000ULL, fix.fix_unix_ms);
	TEST_ASSERT_EQUAL_UINT16(5, fix.accuracy_m);
	TEST_ASSERT_EQUAL_UINT8(4, fix.sats);
}

void test_position_decode_refuses_what_it_cannot_read() {
	const char* refused[] = {
		"020ffedcba98f8a432eb05e30a786ab13bfbffff",            // truncated before altitude
		"010ffedcba98f8a432eb05e30a786ab13bfbfffff400ff09",    // version 1
		"020ffedcba98f8a432eb05e30a786ab13bfb",                // shorter than the base
	};
	for (const char* hex : refused) {
		uint8_t in[POSITION_WIRE_MAX_LEN];
		const size_t n = bytes_of(hex, in, sizeof(in));
		NodePositionFix fix;
		TEST_ASSERT_FALSE_MESSAGE(position_report_decode(in, n, fix), hex);
	}
}

void test_position_round_trips_its_own_encoding() {
	uint8_t in[POSITION_WIRE_MAX_LEN];
	const size_t n = bytes_of("020ffedcba98f8a432eb05e30a786ab13bfbfffff400ff09", in, sizeof(in));
	NodePositionFix fix;
	TEST_ASSERT_TRUE(position_report_decode(in, n, fix));
	TEST_ASSERT_EQUAL_STRING("020ffedcba98f8a432eb05e30a786ab13bfbfffff400ff09", encoded(fix).c_str());
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
	RUN_TEST(test_position_minimal_report_bytes);
	RUN_TEST(test_position_all_fields_saturate_and_normalise);
	RUN_TEST(test_position_course_and_speed_scale);
	RUN_TEST(test_position_invalid_fix_or_short_buffer_encodes_nothing);
	RUN_TEST(test_position_decode_ignores_the_interval_byte);
	RUN_TEST(test_position_decode_refuses_what_it_cannot_read);
	RUN_TEST(test_position_round_trips_its_own_encoding);
	return UNITY_END();
}
