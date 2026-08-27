#pragma once

#if defined(RRC_HUB)

#include <cstddef>
#include <cstdint>

#include <microReticulum.h>

extern bool rrc_hub_enabled;
extern char rrc_hub_name[64];
extern uint32_t rrc_hub_announce_interval_seconds;
extern uint8_t rrc_hub_max_sessions;
extern uint8_t rrc_hub_max_rooms_per_session;
extern uint16_t rrc_hub_max_body_bytes;
extern uint16_t rrc_hub_rate_per_minute;
extern uint32_t rrc_hub_ping_interval_seconds;
extern uint32_t rrc_hub_pong_timeout_seconds;

// Bounded, ephemeral RRC v1 group-chat hub. The destination uses the
// Reticulum transport identity, so its hash remains stable across reboots as
// long as the normal Reticulum identity store remains intact.
void rrc_hub_begin(const RNS::Identity& identity);
void rrc_hub_loop();

RNS::Bytes rrc_hub_destination_hash();
bool rrc_hub_running();
size_t rrc_hub_session_count();
size_t rrc_hub_identified_count();
size_t rrc_hub_room_count();
size_t rrc_hub_membership_count();
uint32_t rrc_hub_rx_count();
uint32_t rrc_hub_tx_count();
uint32_t rrc_hub_accepted_count();
uint32_t rrc_hub_forwarded_count();
uint32_t rrc_hub_rejected_count();
uint32_t rrc_hub_rate_limited_count();
uint32_t rrc_hub_malformed_count();
uint32_t rrc_hub_identify_timeout_count();
uint32_t rrc_hub_hello_timeout_count();
uint32_t rrc_hub_pong_timeout_count();

#endif // RRC_HUB
