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
#include "ServiceRunner.h"

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

void test_runner_refuses_past_its_capacity() {
	reset_runner_logs();
	FakeService a('a', 0);
	ServiceRunner runner(fake_clock);
	for (int i = 0; i < SERVICE_RUNNER_CAPACITY; ++i) TEST_ASSERT_TRUE(runner.add(&a));
	TEST_ASSERT_FALSE(runner.add(&a));
	TEST_ASSERT_FALSE(runner.add(nullptr));
	TEST_ASSERT_EQUAL_size_t(SERVICE_RUNNER_CAPACITY, runner.count());
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
	return UNITY_END();
}
