#include <unity.h>

#include "RRCProtocol.h"
#include "RRCState.h"

#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> from_hex(const char* hex) {
    std::vector<uint8_t> output;
    while (*hex) {
        unsigned int value = 0;
        TEST_ASSERT_EQUAL_INT(1, std::sscanf(hex, "%2x", &value));
        output.push_back(static_cast<uint8_t>(value));
        hex += 2;
    }
    return output;
}

RRC::IdentityHash identity(uint8_t start) {
    RRC::IdentityHash output{};
    for (size_t i = 0; i < output.size(); ++i) output[i] = start + i;
    return output;
}

RRC::IdentityHash patterned_identity(uint8_t start, int8_t step) {
    RRC::IdentityHash output{};
    for (size_t i = 0; i < output.size(); ++i) {
        output[i] = static_cast<uint8_t>(start + step * static_cast<int>(i));
    }
    return output;
}

RRC::MessageId message_id(uint8_t start) {
    RRC::MessageId output{};
    for (size_t i = 0; i < output.size(); ++i) output[i] = start + i;
    return output;
}

std::vector<uint8_t> encode(const RRC::Envelope& envelope) {
    std::array<uint8_t, RRC::MAX_ENVELOPE_BYTES> buffer{};
    const RRC::Result result = RRC::encode(envelope, buffer.data(), buffer.size());
    TEST_ASSERT_TRUE_MESSAGE(static_cast<bool>(result), RRC::error_string(result.error));
    return std::vector<uint8_t>(buffer.begin(), buffer.begin() + result.size);
}

RRC::Envelope message_envelope() {
    RRC::Envelope envelope;
    envelope.type = RRC::T_MSG;
    envelope.message_id = message_id(1);
    envelope.timestamp_ms = 1737849600000ULL;
    envelope.source = patterned_identity(0, 0x11);
    envelope.room = "#lobby";
    envelope.body = RRC::Body::text_value("Hello, world!");
    envelope.nickname = "alice";
    return envelope;
}

} // namespace

void test_msg_encoding_matches_nomadnet_golden_vector() {
    const std::vector<uint8_t> expected = from_hex(
        "a80001011402480102030405060708031b000001949fe878000450001122334455"
        "66778899aabbccddeeff0566236c6f626279066d48656c6c6f2c20776f726c64"
        "210765616c696365");
    const std::vector<uint8_t> actual = encode(message_envelope());
    TEST_ASSERT_EQUAL_size_t(expected.size(), actual.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.data(), actual.data(), expected.size());
}

void test_hello_encoding_matches_nomadnet_golden_vector() {
    RRC::HelloBody hello;
    hello.client_name = "NomadNet";
    hello.client_version = "1.2.8";
    hello.capabilities.action = true;
    RRC::Envelope envelope;
    envelope.type = RRC::T_HELLO;
    envelope.message_id = message_id(0x11);
    envelope.source = patterned_identity(0xff, -0x11);
    envelope.body = RRC::Body::hello_value(hello);
    envelope.nickname = "Lexus";

    const std::vector<uint8_t> expected = from_hex(
        "a7000101010248111213141516171803000450ffeeddccbbaa9988776655443322"
        "110006a300684e6f6d61644e65740165312e322e3802a200f401f507654c6578"
        "7573");
    const std::vector<uint8_t> actual = encode(envelope);
    TEST_ASSERT_EQUAL_size_t(expected.size(), actual.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.data(), actual.data(), expected.size());
}

void test_welcome_encoding_matches_reference_extension_maps() {
    RRC::WelcomeBody welcome;
    welcome.hub_name = "RAD-01 Hub";
    welcome.hub_version = "0.1.0";
    welcome.capabilities.action = true;
    welcome.limits = RRC::HubLimits{};
    RRC::Envelope envelope;
    envelope.type = RRC::T_WELCOME;
    envelope.message_id = message_id(0x21);
    envelope.source = patterned_identity(0, 0x11);
    envelope.body = RRC::Body::welcome_value(welcome);

    const std::vector<uint8_t> expected = from_hex(
        "a600010102024821222324252627280300045000112233445566778899aabbccdd"
        "eeff06a4006a5241442d3031204875620165302e312e3002a200f401f503a500"
        "182001184002190118030404183c");
    const std::vector<uint8_t> actual = encode(envelope);
    TEST_ASSERT_EQUAL_size_t(expected.size(), actual.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.data(), actual.data(), expected.size());
}

void test_decode_accepts_reordered_keys_and_ignores_unknown_key() {
    const std::vector<uint8_t> wire = from_hex(
        "a918636769676e6f7265640765616c69636506624869056523524f4f4d045000"
        "112233445566778899aabbccddeeff03010248010203040506070801140001");
    RRC::Envelope envelope;
    const RRC::Result result = RRC::decode(wire.data(), wire.size(), envelope);
    TEST_ASSERT_TRUE_MESSAGE(static_cast<bool>(result), RRC::error_string(result.error));
    TEST_ASSERT_EQUAL_UINT8(RRC::T_MSG, envelope.type);
    TEST_ASSERT_EQUAL_STRING("#ROOM", envelope.room->c_str());
    TEST_ASSERT_EQUAL_STRING("Hi", envelope.body.text.c_str());
    TEST_ASSERT_EQUAL_STRING("alice", envelope.nickname->c_str());
}

void test_decode_ignores_unknown_message_type_and_rejects_bad_version() {
    const std::vector<uint8_t> unknown = from_hex(
        "a60001011863024801020304050607080300045000112233445566778899aabbcc"
        "ddeeff0663666f6f");
    RRC::Envelope envelope;
    RRC::Result result = RRC::decode(unknown.data(), unknown.size(), envelope);
    TEST_ASSERT_TRUE_MESSAGE(static_cast<bool>(result),
                             RRC::error_string(result.error));
    TEST_ASSERT_EQUAL_UINT8(99, envelope.type);
    TEST_ASSERT_EQUAL_UINT8(RRC::BodyKind::None, envelope.body.kind);

    std::vector<uint8_t> bad_version = unknown;
    bad_version[2] = 2;
    result = RRC::decode(bad_version.data(), bad_version.size(), envelope);
    TEST_ASSERT_EQUAL_UINT8(RRC::Error::UnsupportedVersion, result.error);

    bad_version.pop_back();
    result = RRC::decode(bad_version.data(), bad_version.size(), envelope);
    TEST_ASSERT_FALSE(static_cast<bool>(result));
}

void test_decode_round_trip_preserves_welcome_limits() {
    RRC::WelcomeBody welcome;
    welcome.hub_name = "RAD-01 Hub";
    welcome.hub_version = "0.1.0";
    welcome.capabilities.action = true;
    welcome.limits = RRC::HubLimits{};
    RRC::Envelope original;
    original.type = RRC::T_WELCOME;
    original.message_id = message_id(9);
    original.source = identity(3);
    original.body = RRC::Body::welcome_value(welcome);
    const auto wire = encode(original);

    RRC::Envelope decoded;
    const RRC::Result result = RRC::decode(wire.data(), wire.size(), decoded);
    TEST_ASSERT_TRUE_MESSAGE(static_cast<bool>(result), RRC::error_string(result.error));
    TEST_ASSERT_EQUAL_UINT8(RRC::BodyKind::Welcome, decoded.body.kind);
    TEST_ASSERT_TRUE(decoded.body.welcome.capabilities.action);
    TEST_ASSERT_FALSE(decoded.body.welcome.capabilities.resource_envelope);
    TEST_ASSERT_TRUE(decoded.body.welcome.limits.has_value());
    TEST_ASSERT_EQUAL_UINT16(280, decoded.body.welcome.limits->max_msg_body_bytes);
    TEST_ASSERT_EQUAL_UINT16(60,
        decoded.body.welcome.limits->rate_limit_msgs_per_minute);
}

void test_validation_rejects_oversize_body_and_invalid_utf8() {
    RRC::Envelope envelope = message_envelope();
    envelope.body.text.assign(RRC::MAX_TEXT_BYTES + 1, 'x');
    TEST_ASSERT_EQUAL_UINT8(RRC::Error::ConstraintViolation,
        RRC::validate(envelope));
    envelope.body.text = std::string("\xc0\xaf", 2);
    TEST_ASSERT_EQUAL_UINT8(RRC::Error::InvalidUtf8, RRC::validate(envelope));

    RRC::HelloBody hello;
    hello.client_name = std::string(RRC::MAX_CLIENT_NAME_BYTES + 1, 'x');
    envelope.type = RRC::T_HELLO;
    envelope.room.reset();
    envelope.nickname.reset();
    envelope.body = RRC::Body::hello_value(hello);
    TEST_ASSERT_EQUAL_UINT8(RRC::Error::ConstraintViolation,
        RRC::validate(envelope));
}

void test_maximum_message_fits_default_link_mdu() {
    RRC::Envelope envelope = message_envelope();
    envelope.timestamp_ms = UINT64_MAX;
    envelope.room = std::string(RRC::MAX_ROOM_BYTES, 'r');
    envelope.body = RRC::Body::text_value(
        std::string(RRC::MAX_TEXT_BYTES, 'm'));
    envelope.nickname = std::string(RRC::MAX_NICK_BYTES, 'n');
    const std::vector<uint8_t> wire = encode(envelope);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(RRC::MAX_ENVELOPE_BYTES, wire.size());

    RRC::Envelope decoded;
    const RRC::Result result = RRC::decode(wire.data(), wire.size(), decoded);
    TEST_ASSERT_TRUE_MESSAGE(static_cast<bool>(result),
                             RRC::error_string(result.error));
    TEST_ASSERT_EQUAL_size_t(RRC::MAX_TEXT_BYTES, decoded.body.text.size());
    TEST_ASSERT_EQUAL_STRING_LEN(envelope.body.text.c_str(),
                                 decoded.body.text.c_str(),
                                 RRC::MAX_TEXT_BYTES);
}

void test_decoder_rejects_wrong_fixed_lengths_and_trailing_data() {
    const std::vector<uint8_t> short_id = from_hex(
        "a5000101140247010203040506070301045000112233445566778899aabbccddeeff");
    RRC::Envelope envelope;
    RRC::Result result = RRC::decode(short_id.data(), short_id.size(), envelope);
    TEST_ASSERT_EQUAL_UINT8(RRC::Error::WrongLength, result.error);

    std::vector<uint8_t> valid = encode(message_envelope());
    valid.push_back(0x00);
    result = RRC::decode(valid.data(), valid.size(), envelope);
    TEST_ASSERT_FALSE(static_cast<bool>(result));

    const std::vector<uint8_t> indefinite_map = from_hex(
        "bf00010114024801020304050607080301045000112233445566778899aabbccddeeffff");
    result = RRC::decode(indefinite_map.data(), indefinite_map.size(), envelope);
    TEST_ASSERT_FALSE(static_cast<bool>(result));
}

void test_authenticated_source_overwrites_spoof_and_normalizes_room() {
    RRC::Envelope envelope = message_envelope();
    envelope.source.fill(0xaa);
    envelope.room = "  #LOBBY  ";
    const RRC::IdentityHash authenticated = identity(0x40);
    TEST_ASSERT_EQUAL_UINT8(RRC::Error::None,
        RRC::stamp_forwarded(envelope, authenticated, std::string("Lexus")));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(authenticated.data(), envelope.source.data(),
                                  authenticated.size());
    TEST_ASSERT_EQUAL_STRING("#lobby", envelope.room->c_str());
    TEST_ASSERT_EQUAL_STRING("Lexus", envelope.nickname->c_str());
}

void test_state_requires_identity_then_hello() {
    RRC::HubState state;
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None, state.open(10, 100));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::NotIdentified,
        state.hello(10, std::string("alice"), 101));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
        state.identify(10, identity(1), 102));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::HelloRequired,
        state.join(10, "#room"));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
        state.hello(10, std::string("alice"), 103));
    TEST_ASSERT_TRUE(state.welcomed(10));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::AlreadyWelcomed,
        state.hello(10, std::nullopt, 104));
}

void test_room_membership_is_case_insensitive_and_closes_cleanly() {
    RRC::HubState state;
    for (uint64_t key : {10ULL, 20ULL}) {
        TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None, state.open(key, 0));
        TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
            state.identify(key, identity(static_cast<uint8_t>(key)), 1));
        TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
            state.hello(key, std::nullopt, 2));
    }
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None, state.join(10, " #FIELD "));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None, state.join(20, "#field"));
    TEST_ASSERT_EQUAL_size_t(1, state.room_count());
    TEST_ASSERT_EQUAL_size_t(2, state.members("#FIELD").count);
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
        state.can_send(10, "#field", 3));
    TEST_ASSERT_TRUE(state.close(10));
    TEST_ASSERT_EQUAL_size_t(1, state.members("#field").count);
    TEST_ASSERT_TRUE(state.close(20));
    TEST_ASSERT_EQUAL_size_t(0, state.room_count());
    TEST_ASSERT_EQUAL_size_t(0, state.session_count());
}

void test_state_enforces_session_room_and_membership_limits() {
    RRC::StateLimits limits;
    limits.max_sessions = 1;
    limits.max_rooms = 1;
    limits.max_rooms_per_session = 1;
    RRC::HubState state(limits);
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None, state.open(1, 0));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::LimitReached, state.open(2, 0));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
        state.identify(1, identity(1), 0));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
        state.hello(1, std::nullopt, 0));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None, state.join(1, "#one"));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::LimitReached, state.join(1, "#two"));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::NotJoined,
        state.can_send(1, "#other", 1));
}

void test_rate_limit_refills_without_unbounded_queue() {
    RRC::StateLimits limits;
    limits.messages_per_minute = 2;
    RRC::HubState state(limits);
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None, state.open(1, 0));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
        state.identify(1, identity(1), 0));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
        state.hello(1, std::nullopt, 0));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None, state.join(1, "#room"));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
        state.can_send(1, "#room", 0));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
        state.consume_rate(1, 0));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
        state.consume_rate(1, 0));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::RateLimited,
        state.consume_rate(1, 0));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
        state.consume_rate(1, 30000));
}

void test_incomplete_and_unresponsive_sessions_expire_and_release_rooms() {
    RRC::StateLimits limits;
    limits.identify_timeout_ms = 10;
    limits.hello_timeout_ms = 20;
    limits.pong_timeout_ms = 30;
    RRC::HubState state(limits);
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None, state.open(1, 100));
    RRC::ExpireCounts expired;
    TEST_ASSERT_EQUAL_size_t(1, state.expire(110, &expired));
    TEST_ASSERT_EQUAL_size_t(1, expired.unidentified);

    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None, state.open(2, 200));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
        state.identify(2, identity(2), 205));
    expired = {};
    TEST_ASSERT_EQUAL_size_t(1, state.expire(225, &expired));
    TEST_ASSERT_EQUAL_size_t(1, expired.hello);

    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None, state.open(3, 300));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
        state.identify(3, identity(3), 301));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
        state.hello(3, std::nullopt, 302));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None, state.join(3, "#room"));
    TEST_ASSERT_EQUAL_size_t(1, state.membership_count());
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
        state.mark_ping_sent(3, 310));
    TEST_ASSERT_TRUE(state.awaiting_pong(3));
    // A scheduler retry must preserve the original deadline. With a 30 ms
    // timeout, resetting this to 320 would incorrectly keep the session alive
    // at 340.
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
        state.mark_ping_sent(3, 320));
    expired = {};
    TEST_ASSERT_EQUAL_size_t(1, state.expire(340, &expired));
    TEST_ASSERT_EQUAL_size_t(1, expired.pong);
    TEST_ASSERT_EQUAL_size_t(1, expired.total());
    TEST_ASSERT_EQUAL_size_t(0, state.session_count());
    TEST_ASSERT_EQUAL_size_t(0, state.room_count());
    TEST_ASSERT_EQUAL_size_t(0, state.membership_count());

    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None, state.open(4, 400));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
        state.identify(4, identity(4), 401));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
        state.hello(4, std::nullopt, 402));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
        state.mark_ping_sent(4, 410));
    TEST_ASSERT_TRUE(state.awaiting_pong(4));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None, state.pong(4));
    TEST_ASSERT_FALSE(state.awaiting_pong(4));
    TEST_ASSERT_EQUAL_size_t(0, state.expire(500));
}

void test_repeated_reconnects_leave_no_stale_membership() {
    RRC::HubState state;
    for (uint64_t cycle = 0; cycle < 100; ++cycle) {
        TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None, state.open(7, cycle));
        TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
            state.identify(7, identity(static_cast<uint8_t>(cycle)), cycle));
        TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
            state.hello(7, std::string("field"), cycle));
        TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None,
            state.join(7, "#incident"));
        const RRC::RoomNames rooms = state.joined_rooms(7);
        TEST_ASSERT_EQUAL_size_t(1, rooms.count);
        TEST_ASSERT_EQUAL_STRING("#incident", rooms.values[0].c_str());
        TEST_ASSERT_TRUE(state.close(7));
        TEST_ASSERT_EQUAL_size_t(0, state.session_count());
        TEST_ASSERT_EQUAL_size_t(0, state.room_count());
        TEST_ASSERT_EQUAL_size_t(0, state.members("#incident").count);
    }
}

void test_state_reports_live_session_after_expiry() {
    RRC::StateLimits limits;
    limits.identify_timeout_ms = 10;
    RRC::HubState state(limits);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(RRC::StateError::None),
                          static_cast<int>(state.open(91, 100)));
    TEST_ASSERT_TRUE(state.has_session(91));
    TEST_ASSERT_EQUAL_UINT32(1, state.expire(110));
    TEST_ASSERT_FALSE(state.has_session(91));
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::NotFound,
        state.consume_rate(91, 111));
}

// Stock NomadNet sends `/list` as a MSG with no room immediately after WELCOME,
// and `/who <room>` room-scoped after JOIN. Requiring a room on every text type
// made the hub answer a healthy stock connection with "missing required field".
// Membership operations still require one, because they are meaningless without.
void test_roomless_command_message_is_accepted_and_membership_still_needs_room() {
    RRC::Envelope command;
    command.type = RRC::T_MSG;
    command.body = RRC::Body::text_value("/list");
    TEST_ASSERT_EQUAL(static_cast<int>(RRC::Error::None),
                      static_cast<int>(RRC::validate(command)));

    RRC::Envelope room_message;
    room_message.type = RRC::T_MSG;
    room_message.room = "#rad01";
    room_message.body = RRC::Body::text_value("hello");
    TEST_ASSERT_EQUAL(static_cast<int>(RRC::Error::None),
                      static_cast<int>(RRC::validate(room_message)));

    RRC::Envelope join;
    join.type = RRC::T_JOIN;
    TEST_ASSERT_EQUAL(static_cast<int>(RRC::Error::MissingField),
                      static_cast<int>(RRC::validate(join)));

    RRC::Envelope part;
    part.type = RRC::T_PART;
    TEST_ASSERT_EQUAL(static_cast<int>(RRC::Error::MissingField),
                      static_cast<int>(RRC::validate(part)));
}

// These exact strings are what stock NomadNet's _parse_room_list_notice and
// _parse_who_notice match on. A wrong prefix or separator produces no error
// anywhere -- the client's room list simply stays empty and its members stay
// unnamed, which is precisely how the hub differed from rrcd in the field.
void test_service_reply_formats_match_the_client_parser_contract() {
    TEST_ASSERT_EQUAL_STRING("No public rooms registered",
                             RRC::format_room_list(nullptr, 0, 280).c_str());

    const std::string rooms[] = {"#alpha", "#beta"};
    TEST_ASSERT_EQUAL_STRING("Registered public rooms\n#alpha\n#beta",
                             RRC::format_room_list(rooms, 2, 280).c_str());

    RRC::MemberEntry entries[2]{};
    for (size_t i = 0; i < entries[0].identity.size(); i++) {
        entries[0].identity[i] = static_cast<uint8_t>(i);
        entries[1].identity[i] = static_cast<uint8_t>(0xF0 + (i & 0x0F));
    }
    entries[0].nickname = std::string("Lexus");   // nicked -> 12 hex prefix
    // entries[1] stays un-nicked                  -> full 32 hex

    TEST_ASSERT_EQUAL_STRING(
        "members in #rad01: Lexus (000102030405), "
        "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
        RRC::format_member_list("#rad01", entries, 2, 280).c_str());

    TEST_ASSERT_EQUAL_STRING("members in #empty: (none)",
                             RRC::format_member_list("#empty", nullptr, 0, 280).c_str());
}

// The advertised body limit must bound the reply, or the hub builds a NOTICE it
// cannot encode and the client gets nothing at all.
void test_service_replies_stay_within_the_advertised_body_limit() {
    std::string rooms[8];
    for (size_t i = 0; i < 8; i++) rooms[i] = std::string("#") + std::string(40, 'a' + (char)i);
    const std::string listed = RRC::format_room_list(rooms, 8, 120);
    TEST_ASSERT_TRUE(listed.size() <= 120);
    TEST_ASSERT_EQUAL_STRING("Registered public rooms", listed.substr(0, 23).c_str());

    RRC::MemberEntry many[8]{};
    for (size_t m = 0; m < 8; m++) {
        for (size_t i = 0; i < many[m].identity.size(); i++) many[m].identity[i] = (uint8_t)(m * 16 + i);
        many[m].nickname = std::string(28, 'n');
    }
    const std::string members = RRC::format_member_list("#room", many, 8, 120);
    TEST_ASSERT_TRUE(members.size() <= 120);
}

// A second identify with the same identity must be a no-op. Reticulum can
// deliver the identified callback more than once, and the hub also adopts the
// Link's proven identity when a packet arrives first. Refusing the duplicate
// made the hub tear down a session that had just identified correctly -- stock
// clients showed "identified, sending HELLO" and were then disconnected.
void test_repeated_identify_is_idempotent_but_rejects_a_different_identity() {
    RRC::HubState state;
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None, state.open(7, 0));

    RRC::IdentityHash first{};
    for (size_t i = 0; i < first.size(); i++) first[i] = static_cast<uint8_t>(i);
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None, state.identify(7, first, 10));

    // Same identity again: accepted, and the session survives.
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::None, state.identify(7, first, 20));
    TEST_ASSERT_TRUE(state.has_session(7));
    TEST_ASSERT_TRUE(state.identity(7).has_value());
    TEST_ASSERT_TRUE(*state.identity(7) == first);

    // A different identity on the same session is still refused.
    RRC::IdentityHash other{};
    for (size_t i = 0; i < other.size(); i++) other[i] = static_cast<uint8_t>(0xA0 + i);
    TEST_ASSERT_EQUAL_UINT8(RRC::StateError::AlreadyIdentified,
                            state.identify(7, other, 30));
    TEST_ASSERT_TRUE(*state.identity(7) == first);
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_msg_encoding_matches_nomadnet_golden_vector);
    RUN_TEST(test_hello_encoding_matches_nomadnet_golden_vector);
    RUN_TEST(test_welcome_encoding_matches_reference_extension_maps);
    RUN_TEST(test_decode_accepts_reordered_keys_and_ignores_unknown_key);
    RUN_TEST(test_decode_ignores_unknown_message_type_and_rejects_bad_version);
    RUN_TEST(test_decode_round_trip_preserves_welcome_limits);
    RUN_TEST(test_validation_rejects_oversize_body_and_invalid_utf8);
    RUN_TEST(test_maximum_message_fits_default_link_mdu);
    RUN_TEST(test_decoder_rejects_wrong_fixed_lengths_and_trailing_data);
    RUN_TEST(test_authenticated_source_overwrites_spoof_and_normalizes_room);
    RUN_TEST(test_state_requires_identity_then_hello);
    RUN_TEST(test_room_membership_is_case_insensitive_and_closes_cleanly);
    RUN_TEST(test_state_enforces_session_room_and_membership_limits);
    RUN_TEST(test_rate_limit_refills_without_unbounded_queue);
    RUN_TEST(test_incomplete_and_unresponsive_sessions_expire_and_release_rooms);
    RUN_TEST(test_repeated_reconnects_leave_no_stale_membership);
    RUN_TEST(test_state_reports_live_session_after_expiry);
    RUN_TEST(test_roomless_command_message_is_accepted_and_membership_still_needs_room);
    RUN_TEST(test_service_reply_formats_match_the_client_parser_contract);
    RUN_TEST(test_service_replies_stay_within_the_advertised_body_limit);
    RUN_TEST(test_repeated_identify_is_idempotent_but_rejects_a_different_identity);
    return UNITY_END();
}
