// Wire format for the one-hop ESP-NOW Reticulum interface.
//
// ESP-NOW is only a link layer here. It discovers adjacent radios and carries
// complete Reticulum frames after fragmentation; it never forwards an ESP-NOW
// frame or makes a routing decision. Reticulum remains the only routing layer.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ESPNOW_PROTO_MAGIC_0        0x52  // R
#define ESPNOW_PROTO_MAGIC_1        0x4e  // N
#define ESPNOW_PROTO_VERSION        0x01

#define ESPNOW_FRAME_DISCOVERY      0x01
#define ESPNOW_FRAME_DATA           0x02
#define ESPNOW_FRAME_SOLICIT        0x03
#define ESPNOW_FRAME_RECOVERY_REPLY 0x04

// ESP-IDF 4.4, used by the current Arduino core, limits an ESP-NOW action
// frame's application data to 250 bytes. The ten-byte header leaves 240 bytes
// per fragment, so a maximum 564-byte Reticulum+IFAC frame needs three.
#define ESPNOW_WIRE_MTU             250
#define ESPNOW_HEADER_SIZE           10
#define ESPNOW_FRAGMENT_PAYLOAD     (ESPNOW_WIRE_MTU - ESPNOW_HEADER_SIZE)
#define ESPNOW_RNS_MTU              564
#define ESPNOW_MAX_FRAGMENTS        ((ESPNOW_RNS_MTU + ESPNOW_FRAGMENT_PAYLOAD - 1) / ESPNOW_FRAGMENT_PAYLOAD)

// Discovery is deliberately small and versioned independently by the common
// protocol header. These fields are advisory: they help an operator identify a
// nearby RAD's LoRa configuration, but MUST NOT trigger an automatic PHY change
// without an authenticated higher-layer exchange.
#define ESPNOW_DISCOVERY_SIZE        16
#define ESPNOW_DISCOVERY_INTERVAL_MS 10000
#define ESPNOW_PEER_TIMEOUT_MS       45000
#define ESPNOW_REASSEMBLY_TIMEOUT_MS 3000

#define ESPNOW_CAP_LORA              0x01
#define ESPNOW_CAP_TRANSPORT         0x02
#define ESPNOW_CAP_IFAC_PROOF        0x04
// "I can reach the mesh without going back out through ESP-NOW." An orphan
// that is itself parented over ESP-NOW is not a way out for anybody else, and
// a node that adopts one as its parent adds a hop that leads nowhere better.
// This is RPL's rank, reduced to the one bit that matters here.
#define ESPNOW_CAP_UPSTREAM          0x08

// Active recovery uses a nonce so a scanner accepts only replies to its
// current channel probe. The reply carries the normal advisory discovery plus
// a short proof derived from the existing backbone IFAC key. It is admission
// evidence, not a replacement for Reticulum's packet-level IFAC validation.
#define ESPNOW_SOLICIT_SIZE            4
#define ESPNOW_RECOVERY_PROOF_SIZE      8
#define ESPNOW_RECOVERY_REPLY_SIZE     (ESPNOW_SOLICIT_SIZE + ESPNOW_DISCOVERY_SIZE + ESPNOW_RECOVERY_PROOF_SIZE)

struct ESPNowFrameHeader {
	uint8_t  type;
	uint16_t packet_id;
	uint8_t  fragment_index;
	uint8_t  fragment_count;
	uint16_t total_length;
};

struct ESPNowDiscovery {
	uint32_t phy_hash;
	uint32_t frequency;
	uint32_t bandwidth;
	uint8_t  spreading_factor;
	uint8_t  coding_rate;
	uint8_t  wifi_channel;
	uint8_t  capabilities;
};

inline void espnow_put_u16(uint8_t* out, uint16_t value) {
	out[0] = (uint8_t)(value >> 8);
	out[1] = (uint8_t)value;
}

inline uint16_t espnow_get_u16(const uint8_t* in) {
	return (uint16_t)(((uint16_t)in[0] << 8) | in[1]);
}

inline void espnow_put_u32(uint8_t* out, uint32_t value) {
	out[0] = (uint8_t)(value >> 24);
	out[1] = (uint8_t)(value >> 16);
	out[2] = (uint8_t)(value >> 8);
	out[3] = (uint8_t)value;
}

inline uint32_t espnow_get_u32(const uint8_t* in) {
	return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
	       ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

inline size_t espnow_write_header(uint8_t* out, size_t capacity,
	                               const ESPNowFrameHeader& header) {
	if (out == nullptr || capacity < ESPNOW_HEADER_SIZE) return 0;
	out[0] = ESPNOW_PROTO_MAGIC_0;
	out[1] = ESPNOW_PROTO_MAGIC_1;
	out[2] = ESPNOW_PROTO_VERSION;
	out[3] = header.type;
	espnow_put_u16(out + 4, header.packet_id);
	out[6] = header.fragment_index;
	out[7] = header.fragment_count;
	espnow_put_u16(out + 8, header.total_length);
	return ESPNOW_HEADER_SIZE;
}

inline bool espnow_read_header(const uint8_t* data, size_t length,
	                            ESPNowFrameHeader& header) {
	if (data == nullptr || length < ESPNOW_HEADER_SIZE) return false;
	if (data[0] != ESPNOW_PROTO_MAGIC_0 || data[1] != ESPNOW_PROTO_MAGIC_1 ||
	    data[2] != ESPNOW_PROTO_VERSION) return false;
	header.type           = data[3];
	header.packet_id      = espnow_get_u16(data + 4);
	header.fragment_index = data[6];
	header.fragment_count = data[7];
	header.total_length   = espnow_get_u16(data + 8);
	return true;
}

inline size_t espnow_write_discovery(uint8_t* out, size_t capacity,
	                                  const ESPNowDiscovery& discovery) {
	if (out == nullptr || capacity < ESPNOW_DISCOVERY_SIZE) return 0;
	espnow_put_u32(out + 0, discovery.phy_hash);
	espnow_put_u32(out + 4, discovery.frequency);
	espnow_put_u32(out + 8, discovery.bandwidth);
	out[12] = discovery.spreading_factor;
	out[13] = discovery.coding_rate;
	out[14] = discovery.wifi_channel;
	out[15] = discovery.capabilities;
	return ESPNOW_DISCOVERY_SIZE;
}

inline bool espnow_read_discovery(const uint8_t* data, size_t length,
	                               ESPNowDiscovery& discovery) {
	if (data == nullptr || length != ESPNOW_DISCOVERY_SIZE) return false;
	discovery.phy_hash         = espnow_get_u32(data + 0);
	discovery.frequency        = espnow_get_u32(data + 4);
	discovery.bandwidth        = espnow_get_u32(data + 8);
	discovery.spreading_factor = data[12];
	discovery.coding_rate      = data[13];
	discovery.wifi_channel     = data[14];
	discovery.capabilities     = data[15];
	return true;
}

inline size_t espnow_write_solicit(uint8_t* out, size_t capacity,
	                                uint32_t nonce) {
	if (out == nullptr || capacity < ESPNOW_SOLICIT_SIZE) return 0;
	espnow_put_u32(out, nonce);
	return ESPNOW_SOLICIT_SIZE;
}

inline bool espnow_read_solicit(const uint8_t* data, size_t length,
	                             uint32_t& nonce) {
	if (data == nullptr || length != ESPNOW_SOLICIT_SIZE) return false;
	nonce = espnow_get_u32(data);
	return true;
}

inline size_t espnow_write_recovery_reply(uint8_t* out, size_t capacity,
	                                       uint32_t nonce,
	                                       const ESPNowDiscovery& discovery,
	                                       const uint8_t* proof) {
	if (out == nullptr || proof == nullptr || capacity < ESPNOW_RECOVERY_REPLY_SIZE) return 0;
	espnow_put_u32(out, nonce);
	espnow_write_discovery(out + ESPNOW_SOLICIT_SIZE, ESPNOW_DISCOVERY_SIZE, discovery);
	memcpy(out + ESPNOW_SOLICIT_SIZE + ESPNOW_DISCOVERY_SIZE,
	       proof, ESPNOW_RECOVERY_PROOF_SIZE);
	return ESPNOW_RECOVERY_REPLY_SIZE;
}

inline bool espnow_read_recovery_reply(const uint8_t* data, size_t length,
	                                    uint32_t& nonce,
	                                    ESPNowDiscovery& discovery,
	                                    const uint8_t*& proof) {
	if (data == nullptr || length != ESPNOW_RECOVERY_REPLY_SIZE) return false;
	nonce = espnow_get_u32(data);
	if (!espnow_read_discovery(data + ESPNOW_SOLICIT_SIZE,
	                           ESPNOW_DISCOVERY_SIZE, discovery)) return false;
	proof = data + ESPNOW_SOLICIT_SIZE + ESPNOW_DISCOVERY_SIZE;
	return true;
}
