// Reticulum BLE peer protocol v2.2 — wire format.
//
// WHY THIS EXISTS, AND WHY IT IS NOT THE OTHER BLE THING
//
// This node is a peer, not a modem. The RNode-over-BLE path already in this
// firmware (BLESerial, Nordic UART, KISS) hands a host control of the radio:
// the host's Reticulum drives the modem, the node's own stack stops seeing
// that traffic, and the node's destinations become unreachable to the very
// device attached to it. For a standalone mesh node that is backwards -- it
// takes the node *out* of the network in order to serve one client.
//
// This is the opposite arrangement, and the one the product wants: a phone
// connects over BLE and joins the mesh *through* the node, exactly as it would
// over TCP. The node keeps its identity, its radio and its own stack; BLE is
// simply another interface into it. The motivating case is that SoftAP costs
// the phone its Wi-Fi, and BLE does not.
//
// THE PROTOCOL is Reticulum BLE v2.2, as implemented by ble-reticulum
// (torlando-tech/ble-reticulum @ 07d9413, "0.2.2") and by Columba's Android
// port. Both sides must agree byte for byte, so the constants below are
// transcribed from BLEFragmentation.py rather than inferred:
//
//   service     37145b00-442d-4a94-917f-8f42c5da28e3   advertised + scanned for
//   rx          ...28e5   WRITE / WRITE_NO_RESPONSE    peer writes to us
//   tx          ...28e4   READ / NOTIFY                we notify the peer
//   identity    ...28e6   READ, 16 bytes               our transport identity
//
// The identity characteristic is what makes this a peer protocol rather than a
// point-to-point link: Android rotates its BLE MAC roughly every 15 minutes, so
// peers are tracked by Reticulum identity instead of address.

#pragma once

#include <cstddef>
#include <cstdint>

#define BLE_PEER_SERVICE_UUID   "37145b00-442d-4a94-917f-8f42c5da28e3"
#define BLE_PEER_TX_UUID        "37145b00-442d-4a94-917f-8f42c5da28e4"  // notify to peer
#define BLE_PEER_RX_UUID        "37145b00-442d-4a94-917f-8f42c5da28e5"  // peer writes here
#define BLE_PEER_IDENTITY_UUID  "37145b00-442d-4a94-917f-8f42c5da28e6"  // read, 16 bytes

// Fragment header: struct.pack("!BHH", type, seq, total) -- big-endian,
// uint8 type then two uint16s. Five bytes, on every fragment.
#define BLE_PEER_HEADER_SIZE 5

// TYPE_LONE is declared by the client and never sent by it. Measured on the
// wire, every single-fragment packet from Columba arrives as 01 0000 0001 --
// START with total==1 -- and FRAGMENT_TYPE_LONE is unused anywhere in its
// Kotlin. Its reassembler drops types it does not recognise, so emitting LONE
// makes the whole outbound direction disappear while inbound keeps working:
// announces reach the mesh and nothing ever comes back.
//
// We accept it defensively on receive. We never send it. See send_outgoing()
// and tests/test_ble_peer_protocol.py.
#define BLE_PEER_TYPE_LONE     0x00
#define BLE_PEER_TYPE_START    0x01
#define BLE_PEER_TYPE_CONTINUE 0x02
#define BLE_PEER_TYPE_END      0x03

// A single-fragment packet is START with total==1. Multi-fragment packets run
// START, CONTINUE..., END. Fragment zero is START even when it is also the
// last one, so reassembly must treat START-with-total-1 as complete on its own.
// This is exactly the kind of detail that costs a day when inferred from
// behaviour instead of read from the source.

// The identity handshake is a bare 16-byte write, sent before any fragment and
// carrying no header at all. A receiver distinguishes it by length: sixteen
// bytes from a peer whose identity is not yet known is a handshake, never a
// fragment. Any fragment is at least BLE_PEER_HEADER_SIZE + 1 = 6 bytes, so the
// two cannot be confused in the other direction.
#define BLE_PEER_IDENTITY_SIZE 16

// Usable payload per fragment, from the negotiated ATT MTU. Three bytes of ATT
// header come off before ours does.
#define BLE_PEER_ATT_HEADER 3
#define BLE_PEER_MIN_MTU    23
#define BLE_PEER_MAX_MTU    517
#define BLE_PEER_MAX_ATTR   512

// Maximum simultaneous peer links carried by one BLE mesh interface. Columba
// defaults to seven, and the ESP32 controller supports up to nine. Targets can
// lower this at build time when their controller or heap budget is smaller.
#ifndef BLE_PEER_MAX_CONNECTIONS
#define BLE_PEER_MAX_CONNECTIONS 7
#endif

// Android BLE links drop after 20-30 seconds of silence, so both reference
// implementations keepalive at 15. A node that does not will look like it
// keeps losing peers.
#define BLE_PEER_KEEPALIVE_MS 15000

// Packets reassembled in a BLE callback and waiting for the main loop to hand
// them to Reticulum. Deep enough to absorb a burst, shallow enough that a
// stalled main loop cannot exhaust memory. Eight was not enough: an announce
// flood delivered thirty packets in twenty seconds and five were refused for
// want of a slot, which the counters showed as inbound and dropped at once.
#define BLE_PEER_RX_QUEUE_DEPTH 32

// The keepalive is a single 0x00 byte, observed on the wire from Columba every
// fifteen seconds. It is not a fragment and must not be parsed as one: a
// fragment is at least BLE_PEER_HEADER_SIZE + 1 bytes, so length alone tells
// them apart. Before this was recognised the heartbeat was counted as a
// discarded fragment, and the node reported steady packet loss on a link that
// was in perfect health.
#define BLE_PEER_KEEPALIVE_BYTE 0x00
#define BLE_PEER_KEEPALIVE_SIZE 1

// How often to re-arm advertising while no peer is connected.
#define BLE_PEER_ADVERTISE_REARM_MS 30000

// Nominal usable throughput of a BLE link, in bits per second. Connection
// interval and MTU put real-world Android BLE in the low hundreds of kbps;
// this is deliberately conservative. RNS only needs it to be finite and
// roughly right -- see the constructor for what a zero here does to link
// establishment.
#define BLE_PEER_BITRATE 100000

// Scan in short bursts rather than continuously: this board also runs a LoRa
// radio and a Reticulum stack, and a permanent scan starves both. The reference
// scans on an interval with an idle backoff for the same reason.
#define BLE_PEER_SCAN_INTERVAL_MS 20000
#define BLE_PEER_SCAN_SECONDS     4

// A central-role attempt must never stop the Reticulum loop for NimBLE's
// default 30-second connect timeout. NimBLE connections are asynchronous on
// the OZD backend, and failed attempts leave a quiet advertising window so a
// Columba central can connect in the opposite direction instead of both peers
// continuously colliding as centrals.
#define BLE_PEER_CONNECT_TIMEOUT_MS 10000
#define BLE_PEER_CONNECT_RETRY_MS   10000
#define BLE_PEER_CONNECT_BLACKLIST_MS 10000
#define BLE_PEER_CONNECT_BLACKLIST_SIZE BLE_PEER_MAX_CONNECTIONS

// Give reciprocal centrals (especially Android) a stable advertising window
// before this node leaves advertising to initiate its own connection.
#define BLE_PEER_PERIPHERAL_WINDOW_MS 15000
#define BLE_PEER_ADVERTISE_SETTLE_MS  500

// Do not dial an advertiser too weak to complete a connection.
//
// A central attempt costs BLE_PEER_CONNECT_TIMEOUT_MS of radio time whether or
// not it succeeds, and this chip has one 2.4 GHz radio shared with Wi-Fi and
// ESP-NOW. Measured on the Ozdisan fixture: an advertiser at -95 dBm was
// discovered, dialled, and failed with reason 13 ten seconds later, repeatedly.
// The RSSI floor avoids spending that radio time and energy on a peer that can
// be heard but not reached. It is not the fixture's watchdog fix; a controlled
// A/B later isolated those resets to the compact device-metrics registry.
//
// -85 dBm is chosen to be permissive: a phone in the same room reads -40 to
// -70, and a RAD across a building still connects at -85. It excludes the case
// this is for, which is an advertiser at the edge of detection that can be
// heard but not reached.
#define BLE_PEER_CONNECT_MIN_RSSI (-85)

// How often to present a fresh BLE address while no peer is connected.
//
// THIS IS A WORKAROUND FOR A CLIENT BUG, NOT PART OF THE PROTOCOL.
//
// Columba's BleScanner keeps a map of every address it has ever seen and fires
// onDeviceDiscovered only when the address is absent from it:
//
//     if (existingDevice == null) { devices[address] = d; onDeviceDiscovered?.invoke(d) }
//     else { /* update RSSI only, no callback */ }
//
// The callback is what reaches the Python driver, and the Python driver is what
// calls connect(). So a client learns about a given BLE address exactly once,
// for the lifetime of that map, and restarting the interface does not clear it.
// A node with a fixed address that was first seen during a moment it could not
// be connected to is then invisible to that client for ever, no matter how
// correctly it advertises afterwards -- which is exactly what OZD-ARD-01 did:
// discoverable at -61 dBm, advertising the right service UUID, and ignored.
//
// Presenting a new address while we have no peer makes us a new key in that map
// on the next scan, so the client re-evaluates us. This is safe here because
// the protocol already refuses to treat a BLE address as identity: peers are
// tracked by the 16-byte Reticulum identity, precisely because Android rotates
// its own address roughly every fifteen minutes.
//
// The proper fix belongs in the client, by evicting stale entries or firing the
// callback on rediscovery. Remove this the day that lands.
#define BLE_PEER_ADDRESS_ROTATE_MS 45000


inline size_t ble_peer_usable_value_length(uint16_t raw_att_mtu) {
	const int usable = (int)raw_att_mtu - BLE_PEER_ATT_HEADER;
	const int floor  = BLE_PEER_MIN_MTU - BLE_PEER_ATT_HEADER;
	if (usable < floor) return (size_t)floor;
	if (usable > BLE_PEER_MAX_ATTR) return (size_t)BLE_PEER_MAX_ATTR;
	return (size_t)usable;
}

inline size_t ble_peer_payload_size(size_t usable_value_length) {
	return (usable_value_length > BLE_PEER_HEADER_SIZE)
	     ? (usable_value_length - BLE_PEER_HEADER_SIZE) : 1;
}

// Write a fragment header into `out` (at least BLE_PEER_HEADER_SIZE bytes).
inline void ble_peer_write_header(uint8_t* out, uint8_t type, uint16_t seq,
                                  uint16_t total) {
	out[0] = type;
	out[1] = (uint8_t)(seq >> 8);      // network byte order, as "!BHH"
	out[2] = (uint8_t)(seq & 0xFF);
	out[3] = (uint8_t)(total >> 8);
	out[4] = (uint8_t)(total & 0xFF);
}

inline bool ble_peer_read_header(const uint8_t* in, size_t length, uint8_t& type,
                                 uint16_t& seq, uint16_t& total) {
	if (length < BLE_PEER_HEADER_SIZE) return false;
	type  = in[0];
	seq   = (uint16_t)((uint16_t)in[1] << 8 | in[2]);
	total = (uint16_t)((uint16_t)in[3] << 8 | in[4]);
	if (type != BLE_PEER_TYPE_LONE && type != BLE_PEER_TYPE_START &&
	    type != BLE_PEER_TYPE_CONTINUE && type != BLE_PEER_TYPE_END) return false;
	// A lone fragment is the whole packet, so it is always sequence 0. Its
	// total is accepted as either 0 or 1 -- the field carries no information
	// when there is nothing to reassemble, and rejecting one spelling of "no
	// continuation" would drop real traffic over a cosmetic disagreement.
	if (type == BLE_PEER_TYPE_LONE) return (seq == 0 && total <= 1);
	if (total == 0 || seq >= total) return false;
	return true;
}
