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
#include "TelemetryCodec.h"
#include "TelemetryBatchCodec.h"
#include "TelemetryKept.h"
#include "TelemetryDetailCodec.h"
#include "TelemetryNeighbours.h"
#include "ServiceRunner.h"
#include "TelemetryUplink.h"

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

// --- TelemetryCodec.h: the board health report, v1 ---------------------------
// Every hex string below is a case in tests/fixtures/telemetry_v1.json, which
// tools/telemetry_codec.py is tested against too; tests/test_telemetry_fixture.py
// checks that each one appears here.

static std::string telemetry_hex(const NodeTelemetry& t) {
	uint8_t out[TELEMETRY_WIRE_MAX_LEN];
	return hex_of(out, telemetry_encode(t, out, sizeof(out)));
}

void test_telemetry_ozd_report_bytes() {
	NodeTelemetry t;
	t.sender_id = 0x0a0b0c0d;
	t.uptime_s = 3600;
	t.reset = TELEMETRY_RESET_PANIC;
	t.boots = 1; t.crashes = 9; t.panics = 8;
	t.heap_bytes = 26220; t.largest_bytes = 19444;          // 25 and 18 KB
	t.if_present = TELEMETRY_IF_BLE | TELEMETRY_IF_WIFI | TELEMETRY_IF_ESPNOW;
	t.if_up = TELEMETRY_IF_BLE | TELEMETRY_IF_ESPNOW;
	t.ble_peers = 1; t.espnow_peers = 1; t.paths = 23; t.nodes = 5;
	t.relaying = true; t.relay_expected = true;
	TEST_ASSERT_EQUAL_STRING("01030a0b0c0d00000e1003000100090008001900120e0a010100170005", telemetry_hex(t).c_str());
}

void test_telemetry_saturates_and_carries_the_optional_fields() {
	NodeTelemetry t;
	t.sender_id = 0xfedcba98;
	t.uptime_s = 143512;
	t.reset = TELEMETRY_RESET_POWERON;
	t.boots = 1; t.crashes = 70000; t.panics = 0;          // crashes saturate
	t.heap_bytes = 243000; t.largest_bytes = 233460;
	t.psram_known = true; t.psram_bytes = 8230059;          // 8037 KB
	t.if_present = TELEMETRY_IF_LORA | TELEMETRY_IF_WIFI | TELEMETRY_IF_ESPNOW;
	t.if_up = t.if_present;
	t.ble_peers = 300; t.espnow_peers = 2;                  // peers saturate
	t.paths = 70000; t.nodes = 12;                          // paths saturate
	t.relaying = true; t.relay_expected = true; t.time_current = true;
	t.battery_known = true; t.battery_mv = 3912; t.battery_pct = 150;   // caps at 100
	TEST_ASSERT_EQUAL_STRING("011ffedcba9800023098010001ffff000000ed00e30d0dff02ffff000c1f650f4864",
	                         telemetry_hex(t).c_str());
}

void test_telemetry_all_zero_is_the_base_length() {
	NodeTelemetry t;
	TEST_ASSERT_EQUAL_STRING("0100000000000000000000000000000000000000000000000000000000", telemetry_hex(t).c_str());
	uint8_t out[TELEMETRY_WIRE_MAX_LEN];
	TEST_ASSERT_EQUAL_size_t(0, telemetry_encode(t, out, TELEMETRY_WIRE_BASE_LEN - 1));
}

void test_telemetry_decode_ignores_appended_bytes() {
	uint8_t in[TELEMETRY_WIRE_MAX_LEN + 8];
	const size_t n = bytes_of("01030a0b0c0d00000e1003000100090008001900120e0a010100170005aabbcc", in, sizeof(in));
	NodeTelemetry t;
	TEST_ASSERT_TRUE(telemetry_decode(in, n, t));
	TEST_ASSERT_EQUAL_HEX32(0x0a0b0c0d, t.sender_id);
	TEST_ASSERT_EQUAL_UINT32(3600, t.uptime_s);
	TEST_ASSERT_EQUAL_UINT8(TELEMETRY_RESET_PANIC, t.reset);
	TEST_ASSERT_EQUAL_UINT32(9, t.crashes);
	TEST_ASSERT_EQUAL_UINT32(25 * 1024, t.heap_bytes);      // whole KB back
	TEST_ASSERT_EQUAL_UINT8(TELEMETRY_IF_BLE | TELEMETRY_IF_ESPNOW, t.if_up);
	TEST_ASSERT_TRUE(t.relaying);
	TEST_ASSERT_FALSE(t.psram_known);
	TEST_ASSERT_FALSE(t.battery_known);
}

void test_telemetry_round_trips_the_full_report() {
	uint8_t in[TELEMETRY_WIRE_MAX_LEN];
	const char* hex = "011ffedcba9800023098010001ffff000000ed00e30d0dff02ffff000c1f650f4864";
	const size_t n = bytes_of(hex, in, sizeof(in));
	NodeTelemetry t;
	TEST_ASSERT_TRUE(telemetry_decode(in, n, t));
	TEST_ASSERT_TRUE(t.psram_known);
	TEST_ASSERT_EQUAL_UINT32(8037u * 1024u, t.psram_bytes);
	TEST_ASSERT_TRUE(t.battery_known);
	TEST_ASSERT_EQUAL_UINT16(3912, t.battery_mv);
	TEST_ASSERT_EQUAL_UINT8(100, t.battery_pct);
	TEST_ASSERT_EQUAL_STRING(hex, telemetry_hex(t).c_str());
}

void test_telemetry_decode_refuses_what_it_cannot_read() {
	const char* refused[] = {
		"011ffedcba9800023098010001ffff000000ed00e30d0dff02ffff000c1f650f",   // truncated before battery
		"02030a0b0c0d00000e1003000100090008001900120e0a010100170005",         // version 2
		"01030a0b0c0d00000e1003000100090008001900120e0a0101001700",           // shorter than the base
	};
	for (const char* hex : refused) {
		uint8_t in[TELEMETRY_WIRE_MAX_LEN];
		const size_t n = bytes_of(hex, in, sizeof(in));
		NodeTelemetry t;
		TEST_ASSERT_FALSE_MESSAGE(telemetry_decode(in, n, t), hex);
	}
}

// --- IService.h / ServiceRunner.h: the service contract (R3) -----------------

static uint32_t fake_now = 0;
static uint32_t fake_clock() { return fake_now; }

static char poll_log[64];
static size_t poll_log_len = 0;
static size_t hook_log[16];
static size_t hook_polls_done[16];
static size_t hook_count = 0;
// Records, with each index, how many polls had run when the hook fired: the
// hook must come before its service's poll, so a poll that hangs has already
// left its watchdog breadcrumb.
static void record_hook(size_t index) {
	if (hook_count < 16) { hook_polls_done[hook_count] = poll_log_len; hook_log[hook_count++] = index; }
}

struct FakeService : IService {
	char tag;
	uint32_t cost_ms;
	uint32_t budget;
	bool init_ok;
	int stopped_at = -1;
	static int stop_sequence;
	FakeService(char tag_, uint32_t cost, uint32_t budget_ = 20, bool ok = true)
		: tag(tag_), cost_ms(cost), budget(budget_), init_ok(ok) {}
	const char* name() const override { return "fake"; }
	bool init(const AppContext&) override { return init_ok; }
	bool start() override { return true; }
	void poll(uint32_t) override {
		if (poll_log_len < sizeof(poll_log) - 1) poll_log[poll_log_len++] = tag;
		fake_now += cost_ms;
	}
	Health health() const override { return Health{ServiceState::Healthy, "ok"}; }
	void telemetry(NodeTelemetry& report) const override { report.paths += 1; report.if_up |= (uint8_t)(1u << (tag - 'a')); }
	void stop() override { stopped_at = stop_sequence++; }
	uint32_t budget_ms() const override { return budget; }
};
int FakeService::stop_sequence = 0;

static void reset_runner_logs() {
	fake_now = 0; poll_log_len = 0; hook_count = 0;
	memset(poll_log, 0, sizeof(poll_log));
	FakeService::stop_sequence = 0;
}

void test_runner_polls_in_declared_order_with_the_hook_first() {
	reset_runner_logs();
	FakeService a('a', 1), b('b', 1), c('c', 1);
	ServiceRunner runner(fake_clock, record_hook);
	TEST_ASSERT_TRUE(runner.add(&a) && runner.add(&b) && runner.add(&c));
	TEST_ASSERT_EQUAL_size_t(3, runner.start_all(AppContext{}));
	runner.poll_all();
	runner.poll_all();
	TEST_ASSERT_EQUAL_STRING("abcabc", poll_log);
	TEST_ASSERT_EQUAL_size_t(6, hook_count);
	for (size_t i = 0; i < hook_count; ++i) {
		TEST_ASSERT_EQUAL_size_t(i % 3, hook_log[i]);
		TEST_ASSERT_EQUAL_size_t(i, hook_polls_done[i]);        // before its poll, after the last
	}
}

void test_runner_measures_each_poll_against_its_budget() {
	reset_runner_logs();
	FakeService quick('a', 5), slow('b', 30), allowed('c', 30, 50);
	ServiceRunner runner(fake_clock);
	runner.add(&quick); runner.add(&slow); runner.add(&allowed);
	runner.start_all(AppContext{});
	runner.poll_all();
	slow.cost_ms = 10;
	runner.poll_all();
	TEST_ASSERT_EQUAL_UINT32(2, runner.timing(0)->polls);
	TEST_ASSERT_EQUAL_UINT32(0, runner.timing(0)->overruns);
	TEST_ASSERT_EQUAL_UINT32(1, runner.timing(1)->overruns);    // 30 ms over 20, then 10 within
	TEST_ASSERT_EQUAL_UINT32(30, runner.timing(1)->worst_ms);
	TEST_ASSERT_EQUAL_UINT32(10, runner.timing(1)->last_ms);
	TEST_ASSERT_EQUAL_UINT32(0, runner.timing(2)->overruns);    // its own 50 ms budget
}

void test_runner_timing_survives_the_clock_wrapping() {
	reset_runner_logs();
	fake_now = 0xFFFFFFF0u;
	FakeService a('a', 32);
	ServiceRunner runner(fake_clock);
	runner.add(&a);
	runner.start_all(AppContext{});
	runner.poll_all();
	TEST_ASSERT_EQUAL_UINT32(32, runner.timing(0)->last_ms);
}

void test_runner_leaves_out_a_service_that_did_not_start() {
	reset_runner_logs();
	FakeService a('a', 1), broken('b', 1, 20, false), c('c', 1);
	ServiceRunner runner(fake_clock);
	runner.add(&a); runner.add(&broken); runner.add(&c);
	TEST_ASSERT_EQUAL_size_t(2, runner.start_all(AppContext{}));
	runner.poll_all();
	TEST_ASSERT_EQUAL_STRING("ac", poll_log);
	TEST_ASSERT_FALSE(runner.running(1));
	TEST_ASSERT_EQUAL(ServiceState::Failed, runner.health(1).state);
	TEST_ASSERT_EQUAL(ServiceState::Healthy, runner.health(0).state);
	TEST_ASSERT_EQUAL(ServiceState::Failed, runner.health(99).state);
	TEST_ASSERT_EQUAL(ServiceStage::InitFailed, runner.stage(1));
	TEST_ASSERT_NULL(runner.timing(99));                        // no borrowed figures
}

struct ExplainingService : FakeService {
	const char* why;
	ExplainingService(const char* why_) : FakeService('x', 0, 20, false), why(why_) {}
	Health health() const override { return Health{ServiceState::Starting, why}; }
};

void test_runner_keeps_the_reason_a_failed_service_gives() {
	reset_runner_logs();
	ExplainingService radio("radio not responding"), silent("");
	ServiceRunner runner(fake_clock);
	runner.add(&radio); runner.add(&silent);
	runner.start_all(AppContext{});
	TEST_ASSERT_EQUAL(ServiceState::Failed, runner.health(0).state);       // forced Failed
	TEST_ASSERT_EQUAL_STRING("radio not responding", runner.health(0).reason);
	TEST_ASSERT_EQUAL_STRING("init failed", runner.health(1).reason);      // the stage, when silent
}

void test_runner_collects_telemetry_from_running_services_only() {
	reset_runner_logs();
	FakeService a('a', 1), broken('b', 1, 20, false), c('c', 1);
	ServiceRunner runner(fake_clock);
	runner.add(&a); runner.add(&broken); runner.add(&c);
	runner.start_all(AppContext{});
	NodeTelemetry report;
	runner.collect(report);
	TEST_ASSERT_EQUAL_UINT32(2, report.paths);
	TEST_ASSERT_EQUAL_UINT8(0x01 | 0x04, report.if_up);        // a and c, not b
}

void test_runner_stops_in_reverse_order() {
	reset_runner_logs();
	FakeService a('a', 1), b('b', 1);
	ServiceRunner runner(fake_clock);
	runner.add(&a); runner.add(&b);
	runner.start_all(AppContext{});
	runner.stop_all();
	TEST_ASSERT_EQUAL_INT(0, b.stopped_at);
	TEST_ASSERT_EQUAL_INT(1, a.stopped_at);
	TEST_ASSERT_FALSE(runner.running(0));
}

// Every state has a name a person can read in a [svc] line or on the services
// page, and "unmeasured" is its own state: never reported as healthy.
// ---- telemetry uplink (F4a) ----

static void gateway_hash(uint8_t* out, uint8_t tag) {
	for (int i = 0; i < TELEMETRY_HASH_LEN; ++i) out[i] = (uint8_t)(tag + i);
}

// Hops by the first byte of the hash: a test stand-in for the path table.
static uint8_t fake_hops_table[256];
static uint8_t fake_hops(const uint8_t* hash) { return fake_hops_table[hash[0]]; }

static void fake_hops_reset() {
	for (int i = 0; i < 256; ++i) fake_hops_table[i] = TELEMETRY_HOPS_UNKNOWN;
}

void test_gateway_nearest_with_a_path_wins() {
	fake_hops_reset();
	TelemetryGateways gateways;
	uint8_t a[16], b[16], c[16];
	gateway_hash(a, 0x10); gateway_hash(b, 0x20); gateway_hash(c, 0x30);
	gateways.heard(a, 16, 1000);
	gateways.heard(b, 16, 1000);
	gateways.heard(c, 16, 1000);
	fake_hops_table[0x10] = 4;
	fake_hops_table[0x20] = 2;
	// 0x30: heard, but no path -- never chosen, however near it might be.
	const TelemetryGateways::Entry* best = gateways.best(2000, fake_hops);
	TEST_ASSERT_NOT_NULL(best);
	TEST_ASSERT_EQUAL_UINT8(0x20, best->hash[0]);
}

void test_gateway_tie_goes_to_the_most_recently_heard() {
	fake_hops_reset();
	TelemetryGateways gateways;
	uint8_t a[16], b[16];
	gateway_hash(a, 0x10); gateway_hash(b, 0x20);
	gateways.heard(a, 16, 1000);
	gateways.heard(b, 16, 5000);
	fake_hops_table[0x10] = 3;
	fake_hops_table[0x20] = 3;
	TEST_ASSERT_EQUAL_UINT8(0x20, gateways.best(6000, fake_hops)->hash[0]);
	gateways.heard(a, 16, 7000);   // heard again: now the fresher of two equals
	TEST_ASSERT_EQUAL_UINT8(0x10, gateways.best(8000, fake_hops)->hash[0]);
}

void test_gateway_none_reachable_is_null_and_newest_is_asked() {
	fake_hops_reset();
	TelemetryGateways gateways;
	TEST_ASSERT_NULL(gateways.best(0, fake_hops));
	TEST_ASSERT_NULL(gateways.newest(0));
	uint8_t a[16], b[16];
	gateway_hash(a, 0x10); gateway_hash(b, 0x20);
	gateways.heard(a, 16, 1000);
	gateways.heard(b, 16, 2000);
	TEST_ASSERT_NULL(gateways.best(3000, fake_hops));
	TEST_ASSERT_EQUAL_UINT8(0x20, gateways.newest(3000)->hash[0]);
}

void test_gateway_unheard_for_the_expiry_is_forgotten() {
	fake_hops_reset();
	TelemetryGateways gateways;
	uint8_t a[16];
	gateway_hash(a, 0x10);
	fake_hops_table[0x10] = 1;
	gateways.heard(a, 16, 1000);
	TEST_ASSERT_NOT_NULL(gateways.best(1000 + TELEMETRY_GATEWAY_EXPIRY_MS - 1, fake_hops));
	TEST_ASSERT_NULL(gateways.best(1000 + TELEMETRY_GATEWAY_EXPIRY_MS, fake_hops));
	TEST_ASSERT_EQUAL_UINT32(0, gateways.count(1000 + TELEMETRY_GATEWAY_EXPIRY_MS));
}

// A path outlives its gateway by days: a board keeping reports asks for one
// heard recently, and is told none when the last announce is too old.
void test_gateway_live_needs_a_recent_announce() {
	fake_hops_reset();
	TelemetryGateways gateways;
	uint8_t a[16];
	gateway_hash(a, 0x10);
	fake_hops_table[0x10] = 2;   // the path is still there
	gateways.heard(a, 16, 1000);
	const uint32_t live = 25u * 60u * 1000u;
	TEST_ASSERT_NOT_NULL(gateways.best(1000 + live - 1, fake_hops, live));
	TEST_ASSERT_NULL(gateways.best(1000 + live, fake_hops, live));
	TEST_ASSERT_NOT_NULL(gateways.best(1000 + live, fake_hops));          // without the limit: still chosen
	TEST_ASSERT_EQUAL_UINT8(0x10, gateways.newest(1000 + live)->hash[0]); // and still addressable
	gateways.heard(a, 16, 1000 + live);                                     // heard again
	TEST_ASSERT_NOT_NULL(gateways.best(1000 + live, fake_hops, live));
}

void test_gateway_table_full_drops_the_oldest_and_refuses_bad_hashes() {
	fake_hops_reset();
	TelemetryGateways gateways;
	uint8_t h[16];
	for (int i = 0; i < TELEMETRY_GATEWAY_CAPACITY; ++i) {
		gateway_hash(h, (uint8_t)(0x10 * (i + 1)));
		gateways.heard(h, 16, 1000 + (uint32_t)i);
	}
	gateway_hash(h, 0xF0);
	gateways.heard(h, 16, 9000);   // evicts 0x10, heard first
	TEST_ASSERT_EQUAL_UINT32(TELEMETRY_GATEWAY_CAPACITY, gateways.count(9000));
	fake_hops_table[0x10] = 1;
	TEST_ASSERT_NULL(gateways.best(9000, fake_hops));
	TEST_ASSERT_FALSE(gateways.heard(h, 15, 9000));
	TEST_ASSERT_FALSE(gateways.heard(nullptr, 16, 9000));
}

void test_telemetry_schedule_first_retry_and_interval() {
	TelemetrySchedule schedule(1000);
	TEST_ASSERT_FALSE(schedule.due(1000 + TELEMETRY_FIRST_MS - 1));
	TEST_ASSERT_TRUE(schedule.due(1000 + TELEMETRY_FIRST_MS));
	const uint32_t t = 1000 + TELEMETRY_FIRST_MS;
	schedule.unreachable(t);
	TEST_ASSERT_FALSE(schedule.due(t + TELEMETRY_RETRY_MS - 1));
	TEST_ASSERT_TRUE(schedule.due(t + TELEMETRY_RETRY_MS));
	schedule.sent(t + TELEMETRY_RETRY_MS);
	TEST_ASSERT_TRUE(schedule.ever_sent());
	TEST_ASSERT_FALSE(schedule.due(t + TELEMETRY_RETRY_MS + TELEMETRY_INTERVAL_MS - 1));
	TEST_ASSERT_TRUE(schedule.due(t + TELEMETRY_RETRY_MS + TELEMETRY_INTERVAL_MS));
}

void test_telemetry_schedule_survives_the_millis_wrap() {
	TelemetrySchedule schedule(0xFFFFFFFFu - 10000u);
	// The first report falls after the 49.7-day wrap; due() must still agree.
	TEST_ASSERT_FALSE(schedule.due(0xFFFFFFFFu - 5000u));
	TEST_ASSERT_TRUE(schedule.due((uint32_t)(0xFFFFFFFFu - 10000u + TELEMETRY_FIRST_MS)));
}

void test_every_service_state_has_a_name() {
	TEST_ASSERT_EQUAL_STRING("starting",   service_state_name(ServiceState::Starting));
	TEST_ASSERT_EQUAL_STRING("healthy",    service_state_name(ServiceState::Healthy));
	TEST_ASSERT_EQUAL_STRING("degraded",   service_state_name(ServiceState::Degraded));
	TEST_ASSERT_EQUAL_STRING("failed",     service_state_name(ServiceState::Failed));
	TEST_ASSERT_EQUAL_STRING("disabled",   service_state_name(ServiceState::Disabled));
	TEST_ASSERT_EQUAL_STRING("unmeasured", service_state_name(ServiceState::Unmeasured));
	TEST_ASSERT_NOT_EQUAL((int)ServiceState::Healthy, (int)ServiceState::Unmeasured);
}

void test_runner_refuses_past_its_capacity() {
	reset_runner_logs();
	FakeService a('a', 0);
	ServiceRunner runner(fake_clock);
	for (int i = 0; i < SERVICE_RUNNER_CAPACITY; ++i) TEST_ASSERT_TRUE(runner.add(&a));
	TEST_ASSERT_FALSE(runner.add(&a));
	TEST_ASSERT_FALSE(runner.add(nullptr));
	TEST_ASSERT_EQUAL_size_t(SERVICE_RUNNER_CAPACITY, runner.count());
}

// --- TelemetryDetailCodec.h: the board detail report, 0x21 -------------------
// Every hex string below is a case in tests/fixtures/telemetry_detail_v1.json,
// which tools/telemetry_detail_codec.py is tested against too;
// tests/test_telemetry_detail_fixture.py checks that each one appears here.

static std::string detail_hex(const NodeDetail& d) {
	uint8_t out[TELEMETRY_DETAIL_WIRE_MAX_LEN];
	return hex_of(out, telemetry_detail_encode(d, out, sizeof(out)));
}

static NodeDetail detail_full() {
	NodeDetail d;
	d.sender_id = 0x0a0b0c0d;
	d.uptime_s = 3600;
	const uint8_t hash[4] = {0xa1, 0xb2, 0xc3, 0xd4};
	memcpy(d.fw_hash, hash, 4);
	d.fw_version = 0x0156;
	strcpy(d.env, "impr-rad01-rev1");
	d.if_count = 4;
	d.interfaces[0] = {DETAIL_IF_LORA, true, 48000, 12000};
	d.interfaces[1] = {DETAIL_IF_ESPNOW, false, 0, 0};
	d.interfaces[2] = {DETAIL_IF_TCP_SERVER, true, 5000000, 2000000};
	d.interfaces[3] = {DETAIL_IF_BLE_PEER, true, 70000, 5};
	d.radio_known = true; d.rssi = -97; d.snr_q = -22; d.noise = -110;
	d.utilisation_pct = 12; d.airtime_pct = 3;
	d.propagation_known = true; d.store_messages = 8; d.store_bytes = 3300;   // 3 KB
	d.pn_peers = 2; d.sync_ok = 5; d.sync_fail = 12; d.last_sync_s = 7260;   // 121 min
	d.nb_count = 3;
	d.neighbours[0] = {0x0e0f1011, DETAIL_IF_LORA, -88, 120};
	d.neighbours[1] = {0x12131415, DETAIL_IF_TCP_SERVER, DETAIL_RSSI_UNKNOWN, 30};
	d.neighbours[2] = {0x16171819, DETAIL_IF_ESPNOW, -70, 70000};    // minutes saturate
	return d;
}

void test_detail_full_report_bytes() {
	TEST_ASSERT_EQUAL_STRING("21030a0b0c0d00000e10a1b2c3d401560f696d70722d72616430312d726576310401010000bb8000002ee0030000000000000000000401004c4b40001e8480020100011170000000059fea920c0300080003020005000c0079030e0f101101a802121314150480001617181903baff", detail_hex(detail_full()).c_str());
}

void test_detail_minimal_report_bytes() {
	NodeDetail d;
	d.sender_id = 0x01020304;
	d.uptime_s = 5;
	TEST_ASSERT_EQUAL_STRING("21000102030400000005000000000000000000", detail_hex(d).c_str());
}

void test_detail_never_synced_and_saturated() {
	NodeDetail d;
	d.sender_id = 0xfedcba98; d.uptime_s = 86400;
	const uint8_t hash[4] = {0x00, 0xff, 0x00, 0xff};
	memcpy(d.fw_hash, hash, 4);
	d.fw_version = 0x0200;
	strcpy(d.env, "ozd");
	d.propagation_known = true; d.store_messages = 70000; d.store_bytes = 80u * 1024u * 1024u;
	d.sync_fail = 70000; d.last_sync_s = DETAIL_NEVER;
	TEST_ASSERT_EQUAL_STRING("2102fedcba980001518000ff00ff0200036f7a6400ffffffff000000ffffffff00", detail_hex(d).c_str());
}

void test_detail_last_sync_saturates() {
	NodeDetail d;
	d.sender_id = 0xfedcba99; d.uptime_s = 1;
	d.propagation_known = true; d.last_sync_s = 4000000;            // 66666 min -> 0xfffe
	TEST_ASSERT_EQUAL_STRING("2102fedcba99000000010000000000000000000000000000000000fffe00", detail_hex(d).c_str());
}

void test_detail_cuts_neighbours_to_fit_and_flags_it() {
	static NodeDetail d;
	d = NodeDetail{};
	d.sender_id = 0x0a0b0c0e; d.uptime_s = 60;
	memset(d.env, 'x', DETAIL_ENV_MAX);
	d.env[DETAIL_ENV_MAX] = 0;                                       // a 40-character name, cut
	d.nb_count = DETAIL_MAX_NEIGHBOURS;                              // 60 asked, 48 kept, 47 fit
	for (uint8_t i = 0; i < DETAIL_MAX_NEIGHBOURS; ++i) d.neighbours[i] = {i, DETAIL_IF_LORA, -100, 60};
	const std::string hex = detail_hex(d);
	TEST_ASSERT_EQUAL_size_t(TELEMETRY_DETAIL_WIRE_MAX_LEN * 2, hex.size());
	TEST_ASSERT_EQUAL_STRING("21040a0b0c0e0000003c000000000000207878787878787878787878787878787878787878787878787878787878787878002f00000000019c0100000001019c0100000002019c0100000003019c0100000004019c0100000005019c0100000006019c0100000007019c0100000008019c0100000009019c010000000a019c010000000b019c010000000c019c010000000d019c010000000e019c010000000f019c0100000010019c0100000011019c0100000012019c0100000013019c0100000014019c0100000015019c0100000016019c0100000017019c0100000018019c0100000019019c010000001a019c010000001b019c010000001c019c010000001d019c010000001e019c010000001f019c0100000020019c0100000021019c0100000022019c0100000023019c0100000024019c0100000025019c0100000026019c0100000027019c0100000028019c0100000029019c010000002a019c010000002b019c010000002c019c010000002d019c010000002e019c01", hex.c_str());
}

void test_detail_round_trips_the_full_report() {
	uint8_t in[TELEMETRY_DETAIL_WIRE_MAX_LEN];
	const size_t n = bytes_of("21030a0b0c0d00000e10a1b2c3d401560f696d70722d72616430312d726576310401010000bb8000002ee0030000000000000000000401004c4b40001e8480020100011170000000059fea920c0300080003020005000c0079030e0f101101a802121314150480001617181903baff", in, sizeof(in));
	static NodeDetail d;
	TEST_ASSERT_TRUE(telemetry_detail_decode(in, n, d));
	TEST_ASSERT_EQUAL_STRING("impr-rad01-rev1", d.env);
	TEST_ASSERT_EQUAL_UINT8(4, d.if_count);
	TEST_ASSERT_EQUAL_UINT32(5000000, d.interfaces[2].rx_bytes);
	TEST_ASSERT_EQUAL_UINT32(70000, d.interfaces[3].rx_bytes);
	TEST_ASSERT_EQUAL_INT8(-97, d.rssi);
	TEST_ASSERT_EQUAL_UINT32(3u * 1024u, d.store_bytes);
	TEST_ASSERT_EQUAL_UINT32(121u * 60u, d.last_sync_s);
	TEST_ASSERT_EQUAL_UINT8(3, d.nb_count);
	TEST_ASSERT_EQUAL_INT8(DETAIL_RSSI_UNKNOWN, d.neighbours[1].rssi);
	TEST_ASSERT_EQUAL_UINT32(255u * 60u, d.neighbours[2].heard_s);
	TEST_ASSERT_FALSE(d.neighbours_truncated);
}

void test_detail_kinds_follow_the_firmware_interface_names() {
	TEST_ASSERT_EQUAL_UINT8(DETAIL_IF_LORA, detail_kind_of("LoRaInterface"));
	TEST_ASSERT_EQUAL_UINT8(DETAIL_IF_BLE_PEER, detail_kind_of("BLEPeerInterface"));
	TEST_ASSERT_EQUAL_UINT8(DETAIL_IF_ESPNOW, detail_kind_of("ESPNowInterface"));
	TEST_ASSERT_EQUAL_UINT8(DETAIL_IF_TCP_SERVER, detail_kind_of("TCPServerInterface"));
	TEST_ASSERT_EQUAL_UINT8(DETAIL_IF_UDP, detail_kind_of("UDPInterface"));
	TEST_ASSERT_EQUAL_UINT8(DETAIL_IF_OTHER, detail_kind_of("Mystery"));
	TEST_ASSERT_EQUAL_UINT8(DETAIL_IF_OTHER, detail_kind_of(nullptr));
}

void test_announces_name_the_neighbour_that_sent_them() {
	uint8_t raw[2 + 16 + 1 + 64 + 10] = {0};
	size_t at = 99;
	// Heard from its originator: HEADER_1, hops 0 -- the public key follows the
	// destination hash and the context byte.
	raw[0] = 0x01; raw[1] = 0;
	TEST_ASSERT_TRUE(announce_neighbour(raw, sizeof(raw), at) == AnnounceSource::PublicKey);
	TEST_ASSERT_EQUAL_UINT32(19, at);
	// The same announce relayed under HEADER_1 says nothing about who sent it.
	raw[1] = 2;
	TEST_ASSERT_TRUE(announce_neighbour(raw, sizeof(raw), at) == AnnounceSource::None);
	// Rebroadcast: HEADER_2, the rebroadcaster's transport id comes first.
	raw[0] = 0x41; raw[1] = 3;
	TEST_ASSERT_TRUE(announce_neighbour(raw, sizeof(raw), at) == AnnounceSource::TransportId);
	TEST_ASSERT_EQUAL_UINT32(2, at);
	// Data packets name nobody; a short announce is refused, not read past.
	raw[0] = 0x40;
	TEST_ASSERT_TRUE(announce_neighbour(raw, sizeof(raw), at) == AnnounceSource::None);
	raw[0] = 0x01; raw[1] = 0;
	TEST_ASSERT_TRUE(announce_neighbour(raw, 2 + 16 + 1 + 63, at) == AnnounceSource::None);
	raw[0] = 0x41;
	TEST_ASSERT_TRUE(announce_neighbour(raw, 2 + 32, at) == AnnounceSource::None);
	const uint8_t hash[4] = {0xba, 0x03, 0xaa, 0x75};
	TEST_ASSERT_EQUAL_HEX32(0xba03aa75u, neighbour_id_of(hash));
}

void test_neighbour_table_keeps_the_freshest_and_expires_the_rest() {
	static NeighbourTable table;
	static NodeDetail d;
	table = NeighbourTable{};
	table.heard(1, DETAIL_IF_LORA, -90, 1000);
	table.heard(2, DETAIL_IF_TCP_SERVER, DETAIL_RSSI_UNKNOWN, 5000);
	table.heard(1, DETAIL_IF_LORA, DETAIL_RSSI_UNKNOWN, 9000);   // RSSI kept on the same carrier
	table.fill(d, 10000);
	TEST_ASSERT_EQUAL_UINT8(2, d.nb_count);
	TEST_ASSERT_EQUAL_UINT32(1, d.neighbours[0].id);
	TEST_ASSERT_EQUAL_UINT32(1, d.neighbours[0].heard_s);
	TEST_ASSERT_EQUAL_INT8(-90, d.neighbours[0].rssi);
	TEST_ASSERT_EQUAL_UINT32(5, d.neighbours[1].heard_s);
	// Heard over another carrier: that carrier's RSSI, or unknown.
	table.heard(1, DETAIL_IF_ESPNOW, DETAIL_RSSI_UNKNOWN, 11000);
	table.fill(d, 11000);
	TEST_ASSERT_EQUAL_UINT8(DETAIL_IF_ESPNOW, d.neighbours[0].kind);
	TEST_ASSERT_EQUAL_INT8(DETAIL_RSSI_UNKNOWN, d.neighbours[0].rssi);
	// Silent past the expiry: dropped.
	table.fill(d, 5000 + NEIGHBOUR_EXPIRY_MS + 1);
	TEST_ASSERT_EQUAL_UINT8(1, d.nb_count);
	TEST_ASSERT_EQUAL_UINT32(1, d.neighbours[0].id);
	// Full: the stalest gives way, the table never grows.
	table = NeighbourTable{};
	for (uint32_t i = 0; i < NEIGHBOUR_TABLE_SIZE; ++i) table.heard(100 + i, DETAIL_IF_LORA, -80, 1000 + i);
	table.heard(999, DETAIL_IF_LORA, -70, 2000);
	TEST_ASSERT_EQUAL_UINT32(NEIGHBOUR_TABLE_SIZE, table.size());
	table.fill(d, 2000);
	TEST_ASSERT_EQUAL_UINT32(999, d.neighbours[0].id);
	for (uint8_t i = 0; i < d.nb_count; ++i) TEST_ASSERT_NOT_EQUAL(100, d.neighbours[i].id);
}

void test_detail_decode_refuses_what_it_cannot_read() {
	const char* refused[] = {
		"01030a0b0c0d00000e1003000100090008001900120e0a010100170005",
		"21030a0b0c0d00000e10a1b2c3d401560f696d70722d72616430312d726576310401010000bb8000002ee0030000000000000000000401004c4b40001e8480020100011170000000059fea920c0300080003020005000c0079030e0f101101a802121314150480001617181903",
		"21000a0b0c0d00000e10a1b2c3d40156216161616161616161616161616161616161616161616161616161616161616161610000",
		"21000a0b0c0d00000e10a1b2c3d40156000d0101000000000000000001010000000000000000010100000000000000000101000000000000000001010000000000000000010100000000000000000101000000000000000001010000000000000000010100000000000000000101000000000000000001010000000000000000010100000000000000000101000000000000000000",
		"21000a0b0c0d00000e10a1b2c3d401",
	};
	for (const char* hex : refused) {
		uint8_t in[TELEMETRY_DETAIL_WIRE_MAX_LEN];
		const size_t n = bytes_of(hex, in, sizeof(in));
		static NodeDetail d;
		TEST_ASSERT_FALSE_MESSAGE(telemetry_detail_decode(in, n, d), hex);
	}
}

// The telemetry batch (TelemetryBatchCodec.h), against every case in
// tests/fixtures/telemetry_batch_v1.json; the Python suite checks that each
// hex below is the fixture's.
struct BatchCaseEntry { uint8_t time_kind; uint32_t time; const char* report_hex; };
struct BatchEncodeCase { const char* name; uint32_t sender_id; uint32_t composed_unix; bool truncated;
                         size_t out_len; size_t n; BatchCaseEntry entries[3]; const char* hex; };
static const BatchEncodeCase BATCH_ENCODE_CASES[] = {
	{"absolute_health_and_detail", 0x0a0b0c0du, 1791100000u, false, 2048, 2, {{1, 1791096400u, "01030a0b0c0d00000e1003000100090008001900120e0a010100170005"}, {1, 1791098200u, "21030a0b0c0d00000e10a1b2c3d401560f696d70722d72616430312d726576310401010000bb8000002ee0030000000000000000000401004c4b40001e8480020100011170000000059fea920c0300080003020005000c0079030e0f101101a802121314150480001617181903baff"}},
	 "31000a0b0c0d6ac2046002016ac1f650001d01030a0b0c0d00000e1003000100090008001900120e0a010100170005016ac1fd58006f21030a0b0c0d00000e10a1b2c3d401560f696d70722d72616430312d726576310401010000bb8000002ee0030000000000000000000401004c4b40001e8480020100011170000000059fea920c0300080003020005000c0079030e0f101101a802121314150480001617181903baff"},
	{"relative_clock_not_set", 0xfedcba98u, 0u, false, 2048, 3, {{0, 3600u, "011ffedcba9800023098010001ffff000000ed00e30d0dff02ffff000c1f650f4864"}, {0, 1800u, "2102fedcba980001518000ff00ff0200036f7a6400ffffffff000000ffffffff00"}, {0, 0u, "011ffedcba9800023098010001ffff000000ed00e30d0dff02ffff000c1f650f4864"}},
	 "3100fedcba9800000000030000000e100022011ffedcba9800023098010001ffff000000ed00e30d0dff02ffff000c1f650f4864000000070800212102fedcba980001518000ff00ff0200036f7a6400ffffffff000000ffffffff0000000000000022011ffedcba9800023098010001ffff000000ed00e30d0dff02ffff000c1f650f4864"},
	{"oldest_dropped_to_fit", 0x0a0b0c0du, 1791100000u, false, 83, 3, {{1, 1791099000u, "01030a0b0c0d00000e1003000100090008001900120e0a010100170005"}, {1, 1791099300u, "01030a0b0c0d00000e1003000100090008001900120e0a010100170005"}, {1, 1791099600u, "01030a0b0c0d00000e1003000100090008001900120e0a010100170005"}},
	 "31010a0b0c0d6ac2046002016ac201a4001d01030a0b0c0d00000e1003000100090008001900120e0a010100170005016ac202d0001d01030a0b0c0d00000e1003000100090008001900120e0a010100170005"},
	{"board_already_dropped_some", 0xfedcba98u, 1791100000u, true, 2048, 1, {{1, 1791099600u, "011ffedcba9800023098010001ffff000000ed00e30d0dff02ffff000c1f650f4864"}},
	 "3101fedcba986ac2046001016ac202d00022011ffedcba9800023098010001ffff000000ed00e30d0dff02ffff000c1f650f4864"},
	{"empty", 0x0a0b0c0du, 1791100000u, false, 2048, 0, {{0, 0u, ""}},
	 "31000a0b0c0d6ac2046000"},
};

void test_batch_encodes_the_fixture() {
	for (const BatchEncodeCase& c : BATCH_ENCODE_CASES) {
		static uint8_t reports[3][TELEMETRY_BATCH_ENTRY_MAX];
		BatchEntry entries[3];
		for (size_t i = 0; i < c.n; ++i) {
			entries[i].time_kind = c.entries[i].time_kind;
			entries[i].time = c.entries[i].time;
			entries[i].len = (uint16_t)bytes_of(c.entries[i].report_hex, reports[i], sizeof(reports[i]));
			entries[i].report = reports[i];
		}
		static uint8_t out[TELEMETRY_BATCH_MAX_LEN];
		const size_t n = telemetry_batch_encode(c.sender_id, c.composed_unix, c.truncated, entries, c.n,
		                                        out, c.out_len);
		TEST_ASSERT_EQUAL_STRING_MESSAGE(c.hex, hex_of(out, n).c_str(), c.name);
	}
}

void test_batch_decodes_the_fixture_and_refuses_the_rest() {
	static uint8_t in[TELEMETRY_BATCH_MAX_LEN + 8];
	// Accepted: header, then every entry, back to the same bytes.
	for (const BatchEncodeCase& c : BATCH_ENCODE_CASES) {
		const size_t len = bytes_of(c.hex, in, sizeof(in));
		BatchHeader h;
		TEST_ASSERT_TRUE_MESSAGE(telemetry_batch_header(in, len, h), c.name);
		TEST_ASSERT_EQUAL_UINT32(c.sender_id, h.sender_id);
		TEST_ASSERT_EQUAL_UINT32(c.composed_unix, h.composed_unix);
		size_t at = TELEMETRY_BATCH_HEADER_LEN;
		const size_t first = c.n - h.count;   // the oldest left out to fit
		for (uint8_t i = 0; i < h.count; ++i) {
			BatchEntry e;
			telemetry_batch_next(in, at, e);
			TEST_ASSERT_EQUAL_UINT32(c.entries[first + i].time, e.time);
			TEST_ASSERT_EQUAL_STRING(c.entries[first + i].report_hex, hex_of(e.report, e.len).c_str());
		}
		TEST_ASSERT_EQUAL_size_t(len, at);
	}
	const char* refused[] = {
		"21000a0b0c0d6ac2046002016ac1f650001d01030a0b0c0d00000e1003000100090008001900120e0a010100170005016ac1fd58006f21030a0b0c0d00000e10a1b2c3d401560f696d70722d72616430312d726576310401010000bb8000002ee0030000000000000000000401004c4b40001e8480020100011170000000059fea920c0300080003020005000c0079030e0f101101a802121314150480001617181903baff",  // wrong_kind
		"31000a0b0c0d6ac2046003016ac1f650001d01030a0b0c0d00000e1003000100090008001900120e0a010100170005016ac1fd58006f21030a0b0c0d00000e10a1b2c3d401560f696d70722d72616430312d726576310401010000bb8000002ee0030000000000000000000401004c4b40001e8480020100011170000000059fea920c0300080003020005000c0079030e0f101101a802121314150480001617181903baff",  // count_says_more
		"31000a0b0c0d6ac2046002016ac1f650001d01030a0b0c0d00000e1003000100090008001900120e0a010100170005016ac1fd58006f21030a0b0c0d00000e10a1b2c3d401560f696d70722d72616430312d726576310401010000bb8000002ee0030000000000000000000401004c4b40001e8480020100011170000000059fea920c0300080003020005000c0079030e0f101101a802121314150480001617181903baff00",  // trailing_byte
		"31000a0b0c0d6ac2046002016ac1f650001d01030a0b0c0d00000e1003000100090008001900120e0a010100170005016ac1fd58006f21030a0b0c0d00000e10a1b2c3d401560f696d70722d72616430312d726576310401010000bb8000002ee0030000000000000000000401004c4b40001e8480020100011170000000059fea920c0300080003020005000c0079030e0f101101a802121314150480001617181903ba",  // entry_cut_short
		"31000a0b0c0d6ac2046002026ac1f650001d01030a0b0c0d00000e1003000100090008001900120e0a010100170005016ac1fd58006f21030a0b0c0d00000e10a1b2c3d401560f696d70722d72616430312d726576310401010000bb8000002ee0030000000000000000000401004c4b40001e8480020100011170000000059fea920c0300080003020005000c0079030e0f101101a802121314150480001617181903baff",  // bad_time_kind
		"31000a0b0c0d6ac0d2600100000000000000",  // empty_entry
		"31000a0b0c0d6ac0d260010100000000017d000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",  // entry_over_max
		"31000a0b0c0d6ac20460",  // header_cut_short
	};
	for (const char* hex : refused) {
		const size_t len = bytes_of(hex, in, sizeof(in));
		BatchHeader h;
		TEST_ASSERT_FALSE(telemetry_batch_header(in, len, h));
	}
}

void test_kept_reports_compose_into_one_batch_with_their_times() {
	static TelemetryKept kept;
	kept.clear();
	uint8_t report[30];
	for (size_t i = 0; i < sizeof(report); ++i) report[i] = (uint8_t)i;
	report[0] = 0x01;
	TEST_ASSERT_TRUE(kept.keep(report, sizeof(report), 1000, 1791100000));   // clock set
	TEST_ASSERT_TRUE(kept.keep(report, sizeof(report), 61000, 0));           // clock not set
	BatchEntry entries[TELEMETRY_KEPT_MAX_ENTRIES];
	const size_t n = kept.entries(entries, TELEMETRY_KEPT_MAX_ENTRIES, 121000);
	TEST_ASSERT_EQUAL_size_t(2, n);
	TEST_ASSERT_EQUAL_UINT8(BATCH_TIME_ABSOLUTE, entries[0].time_kind);
	TEST_ASSERT_EQUAL_UINT32(1791100000, entries[0].time);
	TEST_ASSERT_EQUAL_UINT8(BATCH_TIME_RELATIVE, entries[1].time_kind);
	TEST_ASSERT_EQUAL_UINT32(60, entries[1].time);                          // taken 60 s before composing
	static uint8_t out[TELEMETRY_BATCH_MAX_LEN];
	const size_t len = telemetry_batch_encode(0x0a0b0c0d, 1791100120, kept.dropped(), entries, n, out, sizeof(out));
	TEST_ASSERT_EQUAL_size_t(kept.encoded_len(), len);
	BatchHeader h;
	TEST_ASSERT_TRUE(telemetry_batch_header(out, len, h));
	TEST_ASSERT_EQUAL_UINT8(2, h.count);
	TEST_ASSERT_EQUAL_UINT8(0, h.flags);
}

void test_kept_reports_always_fit_one_batch_and_the_oldest_give_way() {
	static TelemetryKept kept;
	kept.clear();
	static uint8_t big[TELEMETRY_BATCH_ENTRY_MAX];
	memset(big, 0x21, sizeof(big));
	for (uint32_t i = 0; i < 20; ++i) {
		big[1] = (uint8_t)i;   // tell them apart
		TEST_ASSERT_TRUE(kept.keep(big, sizeof(big), 1000 * i, 1791100000 + i));
		TEST_ASSERT_TRUE(kept.encoded_len() <= TELEMETRY_BATCH_MAX_LEN);
	}
	TEST_ASSERT_TRUE(kept.dropped());
	BatchEntry entries[TELEMETRY_KEPT_MAX_ENTRIES];
	const size_t n = kept.entries(entries, TELEMETRY_KEPT_MAX_ENTRIES, 30000);
	TEST_ASSERT_EQUAL_UINT8(19, entries[n - 1].report[1]);                 // the newest kept
	TEST_ASSERT_EQUAL_UINT32(1791100000 + 20 - n, entries[0].time);         // the oldest that fit
	static uint8_t out[TELEMETRY_BATCH_MAX_LEN];
	const size_t len = telemetry_batch_encode(1, 1791100030, kept.dropped(), entries, n, out, sizeof(out));
	BatchHeader h;
	TEST_ASSERT_TRUE(telemetry_batch_header(out, len, h));
	TEST_ASSERT_EQUAL_size_t(n, h.count);                                   // nothing more cut by the encoder
	TEST_ASSERT_EQUAL_UINT8(BATCH_FLAG_TRUNCATED, h.flags);
	// Small reports: the entry count is the limit, not the bytes.
	kept.clear();
	uint8_t small[4] = {0x01, 2, 3, 4};
	for (uint32_t i = 0; i < TELEMETRY_KEPT_MAX_ENTRIES + 5; ++i) kept.keep(small, sizeof(small), i, 0);
	TEST_ASSERT_EQUAL_size_t(TELEMETRY_KEPT_MAX_ENTRIES, kept.count());
	TEST_ASSERT_FALSE(kept.keep(small, 0, 0, 0));
	kept.clear();
	TEST_ASSERT_TRUE(kept.empty());
	TEST_ASSERT_FALSE(kept.dropped());
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
	RUN_TEST(test_telemetry_ozd_report_bytes);
	RUN_TEST(test_telemetry_saturates_and_carries_the_optional_fields);
	RUN_TEST(test_telemetry_all_zero_is_the_base_length);
	RUN_TEST(test_telemetry_decode_ignores_appended_bytes);
	RUN_TEST(test_telemetry_round_trips_the_full_report);
	RUN_TEST(test_telemetry_decode_refuses_what_it_cannot_read);
	RUN_TEST(test_runner_polls_in_declared_order_with_the_hook_first);
	RUN_TEST(test_runner_measures_each_poll_against_its_budget);
	RUN_TEST(test_runner_timing_survives_the_clock_wrapping);
	RUN_TEST(test_runner_leaves_out_a_service_that_did_not_start);
	RUN_TEST(test_runner_keeps_the_reason_a_failed_service_gives);
	RUN_TEST(test_runner_collects_telemetry_from_running_services_only);
	RUN_TEST(test_runner_stops_in_reverse_order);
	RUN_TEST(test_runner_refuses_past_its_capacity);
	RUN_TEST(test_every_service_state_has_a_name);
	RUN_TEST(test_gateway_nearest_with_a_path_wins);
	RUN_TEST(test_gateway_tie_goes_to_the_most_recently_heard);
	RUN_TEST(test_gateway_none_reachable_is_null_and_newest_is_asked);
	RUN_TEST(test_gateway_unheard_for_the_expiry_is_forgotten);
	RUN_TEST(test_gateway_table_full_drops_the_oldest_and_refuses_bad_hashes);
	RUN_TEST(test_telemetry_schedule_first_retry_and_interval);
	RUN_TEST(test_telemetry_schedule_survives_the_millis_wrap);
	RUN_TEST(test_detail_full_report_bytes);
	RUN_TEST(test_detail_minimal_report_bytes);
	RUN_TEST(test_detail_never_synced_and_saturated);
	RUN_TEST(test_detail_last_sync_saturates);
	RUN_TEST(test_detail_cuts_neighbours_to_fit_and_flags_it);
	RUN_TEST(test_detail_round_trips_the_full_report);
	RUN_TEST(test_detail_decode_refuses_what_it_cannot_read);
	RUN_TEST(test_detail_kinds_follow_the_firmware_interface_names);
	RUN_TEST(test_announces_name_the_neighbour_that_sent_them);
	RUN_TEST(test_neighbour_table_keeps_the_freshest_and_expires_the_rest);
	RUN_TEST(test_batch_encodes_the_fixture);
	RUN_TEST(test_batch_decodes_the_fixture_and_refuses_the_rest);
	RUN_TEST(test_kept_reports_compose_into_one_batch_with_their_times);
	RUN_TEST(test_kept_reports_always_fit_one_batch_and_the_oldest_give_way);
	RUN_TEST(test_gateway_live_needs_a_recent_announce);
	return UNITY_END();
}
