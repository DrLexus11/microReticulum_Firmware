// One-hop Reticulum interface over ESP-NOW.
//
// ESP-NOW performs neighbour discovery and moves frames between radios on the
// current WiFi channel. It does not forward ESP-NOW frames or build routes;
// complete reassembled frames are handed to Reticulum, which remains the only
// routing and end-to-end security layer.

#pragma once

#include "Boards.h"

#if HAS_WIFI == true && MCU_VARIANT == MCU_ESP32

#include <microReticulum.h>
#include <microReticulum/Cryptography/HKDF.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <exception>
#include <new>
#include <string.h>

#include "ESPNowProtocol.h"

class ESPNowInterface;

inline ESPNowInterface* espnow_active_interface = nullptr;
inline void espnow_receive_trampoline(const uint8_t* mac, const uint8_t* data, int length);
inline void espnow_send_trampoline(const uint8_t* mac, esp_now_send_status_t status);

class ESPNowInterface : public RNS::InterfaceImpl {
public:
	enum RecoveryState : uint8_t {
		RECOVERY_STRICT = 0,
		RECOVERY_REQUESTED = 1,
		RECOVERY_SCANNING = 2,
		RECOVERY_PINNED = 3,
		RECOVERY_FAILED = 4,
	};

	ESPNowInterface(const char* name) : RNS::InterfaceImpl(name) {
		_IN = true;
		_OUT = true;
		_HW_MTU = ESPNOW_RNS_MTU;
		// Conservative usable throughput, below Espressif's measured open-air
		// figure. RNS uses this for link proof timing and must never see zero.
		_bitrate = 100000;
		memset(_peers, 0, sizeof(_peers));
		memset(_reassembly, 0, sizeof(_reassembly));
		memset(_tx_queue, 0, sizeof(_tx_queue));
		memset(_rx_queue, 0, sizeof(_rx_queue));
	}

	ESPNowInterface() : ESPNowInterface("ESPNowInterface") {}

	virtual ~ESPNowInterface() {
		stop();
		_name = "deleted";
	}

	bool start() override {
		wifi_interface_t desired;
		if (!active_wifi_interface(desired)) return false;
		if (_started && desired == _wifi_interface) return true;
		if (_started) stop();
		if (espnow_active_interface != nullptr && espnow_active_interface != this) {
			printf("[espnow] refusing to start: another interface owns ESP-NOW\n");
			return false;
		}

		esp_err_t err = esp_now_init();
		if (err != ESP_OK) {
			printf("[espnow] init failed: %d\n", (int)err);
			return false;
		}

		espnow_active_interface = this;
		if (esp_now_register_recv_cb(espnow_receive_trampoline) != ESP_OK ||
		    esp_now_register_send_cb(espnow_send_trampoline) != ESP_OK) {
			printf("[espnow] callback registration failed\n");
			stop();
			return false;
		}

		esp_now_peer_info_t broadcast = {};
		memset(broadcast.peer_addr, 0xff, sizeof(broadcast.peer_addr));
		broadcast.channel = 0;  // follow the interface's current WiFi channel
		broadcast.ifidx = desired;
		broadcast.encrypt = false;
		err = esp_now_add_peer(&broadcast);
		if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
			printf("[espnow] could not add broadcast peer: %d\n", (int)err);
			stop();
			return false;
		}

		_wifi_interface = desired;
		esp_wifi_get_mac(_wifi_interface, _own_mac);
		refresh_channel();
		_started = true;
		_online = true;
		_next_discovery_at = millis() + (esp_random() % 2000u);
		printf("[espnow] interface up on WiFi %s channel %u, mtu=%u bitrate=%lu\n",
		       _wifi_interface == WIFI_IF_STA ? "STA" : "AP", (unsigned)_channel,
		       (unsigned)_HW_MTU, (unsigned long)_bitrate);
		return true;
	}

	void stop() override {
		if (!_started && espnow_active_interface != this) return;
		if (espnow_active_interface == this) {
			esp_now_unregister_recv_cb();
			esp_now_unregister_send_cb();
			esp_now_deinit();
			espnow_active_interface = nullptr;
		}
		_started = false;
		_online = false;
		_send_waiting = false;
		_send_done = false;
		_send_kind = SEND_NONE;
		clear_rx_queue();
		clear_reassembly();
		reset_recovery();
		_recovery_reply_pending = false;
	}

	void detach() override { stop(); }

	void loop() override {
		const uint32_t now = millis();
		wifi_interface_t desired;
		if (!active_wifi_interface(desired)) {
			if (_started) stop();
			return;
		}
		if (_started && desired != _wifi_interface) stop();
		if (!_started) {
			if ((uint32_t)(now - _last_start_try) < START_RETRY_MS) return;
			_last_start_try = now;
			if (!start()) return;
		}

		if ((uint32_t)(now - _last_channel_check) >= CHANNEL_CHECK_MS) {
			_last_channel_check = now;
			refresh_channel();
		}

		drain_inbound();
		expire_state(now);
		service_send_completion(now);
		if (!_started || _send_waiting || _send_done) return;

		if (_recovery_state == RECOVERY_REQUESTED) start_recovery_scan(now);
		if (_recovery_state == RECOVERY_SCANNING) {
			if (_recovery_reply_pending && time_reached(now, _recovery_reply_at)) {
				send_recovery_reply(now);
			}
			else {
				service_recovery_scan(now);
			}
			return;
		}
		if (_recovery_state == RECOVERY_PINNED &&
		    (uint32_t)(now - _recovery_peer_last_seen) > ESPNOW_PEER_TIMEOUT_MS) {
			_recovery_state = RECOVERY_FAILED;
			_recovery_failures++;
			return;
		}

		if (_recovery_reply_pending && time_reached(now, _recovery_reply_at)) {
			send_recovery_reply(now);
			return;
		}

		if (_tx_tail != _tx_head) {
			send_current_fragment(now);
		}
		else if (time_reached(now, _next_discovery_at)) {
			send_discovery(now);
		}
	}

	bool started() const { return _started; }
	uint8_t channel() const { return _channel; }
	uint32_t peer_count() const {
		uint32_t count = 0;
		const uint32_t now = millis();
		for (uint8_t i = 0; i < MAX_PEERS; ++i) {
			if (_peers[i].used && (uint32_t)(now - _peers[i].last_seen) <= ESPNOW_PEER_TIMEOUT_MS) ++count;
		}
		return count;
	}
	uint32_t packets_in() const { return _packets_in; }
	uint32_t packets_out() const { return _packets_out; }
	uint32_t discoveries_in() const { return _discoveries_in; }
	uint32_t rx_dropped() const { return _rx_dropped + _rx_callback_drops; }
	uint32_t tx_dropped() const { return _tx_dropped; }
	uint32_t send_failures() const { return _send_failures; }
	uint32_t reassembly_timeouts() const { return _reassembly_timeouts; }
	uint32_t last_peer_phy_hash() const { return _last_peer_phy_hash; }
	RecoveryState recovery_state() const { return _recovery_state; }
	const char* recovery_state_name() const {
		switch (_recovery_state) {
			case RECOVERY_REQUESTED: return "requested";
			case RECOVERY_SCANNING:  return "scanning";
			case RECOVERY_PINNED:    return "pinned";
			case RECOVERY_FAILED:    return "failed";
			default:                 return "strict";
		}
	}
	bool recovery_active() const {
		return _recovery_state == RECOVERY_REQUESTED ||
		       _recovery_state == RECOVERY_SCANNING ||
		       _recovery_state == RECOVERY_PINNED;
	}
	bool recovery_pinned() const { return _recovery_state == RECOVERY_PINNED; }
	bool recovery_failed() const { return _recovery_state == RECOVERY_FAILED; }
	uint8_t recovery_channel() const { return _recovery_selected_channel; }
	uint32_t recovery_pinned_since() const { return _recovery_pinned_since; }
	uint32_t recovery_scans() const { return _recovery_scans; }
	uint32_t recovery_successes() const { return _recovery_successes; }
	uint32_t recovery_failures() const { return _recovery_failures; }
	uint32_t recovery_proof_failures() const { return _recovery_proof_failures; }
	uint32_t recovery_channel_errors() const { return _recovery_channel_errors; }
	uint32_t accepted_packets_in() const { return _accepted_packets_in; }
	uint32_t accepted_from_selected() const { return _accepted_from_selected; }
	const char* recovery_peer_mac() const { return _recovery_peer_text; }

	bool request_recovery_scan(uint32_t budget_ms, uint8_t rendezvous_channel) {
		// A connected station owns the radio channel. Recovery is forbidden in
		// that state even if a caller is wrong; normal WiFi must never flap for a
		// speculative peer search.
		if (WiFi.getMode() != WIFI_MODE_STA ||
		    WiFi.status() == WL_CONNECTED) return false;
		// A node with an erased/no SSID configuration reaches fallback
		// immediately. Do not make that first recovery opportunity depend on the
		// interface's normal delayed start retry having elapsed.
		if (!_started && !start()) return false;
		if (recovery_active()) return true;
		if (budget_ms < 1000) budget_ms = 1000;
		if (budget_ms > 60000) budget_ms = 60000;
		_recovery_budget_ms = budget_ms;
		_recovery_rendezvous = rendezvous_channel;
		_recovery_state = RECOVERY_REQUESTED;
		return true;
	}

	void reset_recovery() {
		_recovery_state = RECOVERY_STRICT;
		_recovery_selected_channel = 0;
		_recovery_peer_text[0] = 0;
		memset(_recovery_peer_mac, 0, sizeof(_recovery_peer_mac));
	}

	// WiFi-task callbacks: copy or record only. Reticulum parsing, logging and
	// reassembly all happen later in loop() on the Arduino loop task.
	void receive_from_callback(const uint8_t* mac, const uint8_t* data, int length) {
		if (mac == nullptr || data == nullptr || length <= 0 || length > ESPNOW_WIRE_MTU) {
			_rx_callback_drops++;
			return;
		}
		portENTER_CRITICAL(&_rx_mux);
		const uint8_t next = (uint8_t)((_rx_head + 1u) % RX_QUEUE_DEPTH);
		if (next == _rx_tail) {
			_rx_callback_drops++;
		}
		else {
			RxWireFrame& slot = _rx_queue[_rx_head];
			memcpy(slot.mac, mac, 6);
			slot.length = (uint16_t)length;
			memcpy(slot.data, data, (size_t)length);
			_rx_head = next;
		}
		portEXIT_CRITICAL(&_rx_mux);
	}

	void send_from_callback(esp_now_send_status_t status) {
		portENTER_CRITICAL(&_send_mux);
		_send_ok = (status == ESP_NOW_SEND_SUCCESS);
		_send_done = true;
		portEXIT_CRITICAL(&_send_mux);
	}

protected:
	bool send_outgoing(const RNS::Bytes& data) override {
		if (!_started || data.size() == 0 || data.size() > ESPNOW_RNS_MTU) {
			_tx_dropped++;
			return false;
		}
		const uint8_t next = (uint8_t)((_tx_head + 1u) % TX_QUEUE_DEPTH);
		if (next == _tx_tail) {
			_tx_dropped++;
			return false;
		}
		TxPacket& packet = _tx_queue[_tx_head];
		packet.length = (uint16_t)data.size();
		packet.packet_id = ++_next_packet_id;
		memcpy(packet.data, data.data(), data.size());
		_tx_head = next;
		handle_outgoing(data);
		return true;
	}

private:
	static constexpr uint8_t MAX_PEERS = 12;
	static constexpr uint8_t REASSEMBLY_SLOTS = 6;
	static constexpr uint8_t RX_QUEUE_DEPTH = 9;  // ring capacity is depth - 1
	static constexpr uint8_t TX_QUEUE_DEPTH = 5;
	static constexpr uint8_t RX_DRAIN_PER_LOOP = 4;
	static constexpr uint8_t SEND_RETRIES = 1;
	static constexpr uint32_t SEND_TIMEOUT_MS = 1000;
	static constexpr uint32_t START_RETRY_MS = 5000;
	static constexpr uint32_t CHANNEL_CHECK_MS = 1000;

	struct RxWireFrame {
		uint8_t mac[6];
		uint16_t length;
		uint8_t data[ESPNOW_WIRE_MTU];
	};

	struct TxPacket {
		uint16_t packet_id;
		uint16_t length;
		uint8_t data[ESPNOW_RNS_MTU];
	};

	struct Reassembly {
		bool used;
		uint8_t mac[6];
		uint16_t packet_id;
		uint16_t total_length;
		uint16_t received;
		uint8_t fragment_count;
		uint8_t next_fragment;
		uint32_t updated;
		uint8_t data[ESPNOW_RNS_MTU];
	};

	struct Peer {
		bool used;
		uint8_t mac[6];
		uint32_t last_seen;
		ESPNowDiscovery discovery;
	};

	enum SendKind : uint8_t {
		SEND_NONE,
		SEND_DATA,
		SEND_DISCOVERY,
		SEND_SOLICIT,
		SEND_RECOVERY_REPLY,
	};

	static bool time_reached(uint32_t now, uint32_t deadline) {
		return (int32_t)(now - deadline) >= 0;
	}

	bool active_wifi_interface(wifi_interface_t& interface) const {
		const wifi_mode_t mode = WiFi.getMode();
		if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
			interface = WIFI_IF_STA;
			return true;
		}
		if (mode == WIFI_MODE_AP) {
			interface = WIFI_IF_AP;
			return true;
		}
		return false;
	}

	void refresh_channel() {
		uint8_t primary = 0;
		wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
		if (esp_wifi_get_channel(&primary, &secondary) == ESP_OK) _channel = primary;
	}

	void clear_rx_queue() {
		portENTER_CRITICAL(&_rx_mux);
		_rx_head = _rx_tail = 0;
		portEXIT_CRITICAL(&_rx_mux);
	}

	void clear_reassembly() {
		for (uint8_t i = 0; i < REASSEMBLY_SLOTS; ++i) _reassembly[i].used = false;
	}

	bool pop_inbound(RxWireFrame& frame) {
		bool present = false;
		portENTER_CRITICAL(&_rx_mux);
		if (_rx_tail != _rx_head) {
			frame = _rx_queue[_rx_tail];
			_rx_tail = (uint8_t)((_rx_tail + 1u) % RX_QUEUE_DEPTH);
			present = true;
		}
		portEXIT_CRITICAL(&_rx_mux);
		return present;
	}

	void drain_inbound() {
		RxWireFrame frame;
		for (uint8_t n = 0; n < RX_DRAIN_PER_LOOP && pop_inbound(frame); ++n) {
			if (memcmp(frame.mac, _own_mac, 6) == 0) continue;
			ESPNowFrameHeader header;
			if (!espnow_read_header(frame.data, frame.length, header)) {
				_rx_dropped++;
				continue;
			}
			const uint8_t* payload = frame.data + ESPNOW_HEADER_SIZE;
			const size_t payload_length = frame.length - ESPNOW_HEADER_SIZE;
			if (header.type == ESPNOW_FRAME_DISCOVERY) {
				handle_discovery(frame.mac, header, payload, payload_length);
			}
			else if (header.type == ESPNOW_FRAME_DATA) {
				touch_peer(frame.mac, nullptr);
				handle_fragment(frame.mac, header, payload, payload_length);
			}
			else if (header.type == ESPNOW_FRAME_SOLICIT) {
				handle_solicit(header, payload, payload_length);
			}
			else if (header.type == ESPNOW_FRAME_RECOVERY_REPLY) {
				handle_recovery_reply(frame.mac, header, payload, payload_length);
			}
			else {
				_rx_dropped++;
			}
		}
	}

	void handle_discovery(const uint8_t* mac, const ESPNowFrameHeader& header,
	                      const uint8_t* payload, size_t payload_length) {
		if (header.fragment_index != 0 || header.fragment_count != 1 ||
		    header.total_length != ESPNOW_DISCOVERY_SIZE) {
			_rx_dropped++;
			return;
		}
		ESPNowDiscovery discovery;
		if (!espnow_read_discovery(payload, payload_length, discovery)) {
			_rx_dropped++;
			return;
		}
		touch_peer(mac, &discovery);
		if (_recovery_state == RECOVERY_PINNED &&
		    memcmp(mac, _recovery_peer_mac, 6) == 0) {
			_recovery_peer_last_seen = millis();
		}
		_last_peer_phy_hash = discovery.phy_hash;
		_discoveries_in++;
	}

	void handle_solicit(const ESPNowFrameHeader& header,
	                    const uint8_t* payload, size_t payload_length) {
		if (header.fragment_index != 0 || header.fragment_count != 1 ||
		    header.total_length != ESPNOW_SOLICIT_SIZE) {
			_rx_dropped++;
			return;
		}
		uint32_t nonce = 0;
		if (!espnow_read_solicit(payload, payload_length, nonce)) {
			_rx_dropped++;
			return;
		}
		// Keep the first outstanding nonce. A burst of scanners (or malformed
		// traffic) must not postpone the reply forever by continually replacing
		// its deadline.
		if (_recovery_reply_pending) return;
		_recovery_reply_nonce = nonce;
		_recovery_reply_pending = true;
		// Several fixed peers may hear one solicitation. Jitter avoids making
		// all of their replies collide at the scanner.
		_recovery_reply_at = millis() + 20u + (esp_random() % 81u);
	}

	void handle_recovery_reply(const uint8_t* mac,
	                           const ESPNowFrameHeader& header,
	                           const uint8_t* payload, size_t payload_length) {
		if (_recovery_state != RECOVERY_SCANNING) return;
		if (header.fragment_index != 0 || header.fragment_count != 1 ||
		    header.total_length != ESPNOW_RECOVERY_REPLY_SIZE) {
			_rx_dropped++;
			return;
		}
		uint32_t nonce = 0;
		ESPNowDiscovery discovery;
		const uint8_t* proof = nullptr;
		if (!espnow_read_recovery_reply(payload, payload_length, nonce,
		                                 discovery, proof)) {
			_rx_dropped++;
			return;
		}
		if (nonce != _recovery_nonce) return;

		const bool local_has_ifac = _ifac_key.size() > 0;
		const bool remote_has_proof =
		    (discovery.capabilities & ESPNOW_CAP_IFAC_PROOF) != 0;
		if (_ifac_required && !local_has_ifac) {
			_recovery_proof_failures++;
			return;
		}
		if (local_has_ifac != remote_has_proof ||
		    (local_has_ifac && !verify_recovery_proof(payload, proof))) {
			_recovery_proof_failures++;
			return;
		}

		touch_peer(mac, &discovery);
		memcpy(_recovery_peer_mac, mac, 6);
		snprintf(_recovery_peer_text, sizeof(_recovery_peer_text),
		         "%02x:%02x:%02x:%02x:%02x:%02x",
		         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		_recovery_selected_channel = _channel;
		_recovery_peer_last_seen = millis();
		_recovery_pinned_since = millis();
		_recovery_state = RECOVERY_PINNED;
		_recovery_successes++;
		_last_peer_phy_hash = discovery.phy_hash;
		_next_discovery_at = millis();
		printf("[espnow] recovery peer %s proven on channel %u\n",
		       _recovery_peer_text, (unsigned)_channel);
	}

	Peer* touch_peer(const uint8_t* mac, const ESPNowDiscovery* discovery) {
		Peer* empty = nullptr;
		Peer* oldest = &_peers[0];
		for (uint8_t i = 0; i < MAX_PEERS; ++i) {
			Peer& peer = _peers[i];
			if (peer.used && memcmp(peer.mac, mac, 6) == 0) {
				peer.last_seen = millis();
				if (discovery != nullptr) peer.discovery = *discovery;
				return &peer;
			}
			if (!peer.used && empty == nullptr) empty = &peer;
			if (peer.last_seen < oldest->last_seen) oldest = &peer;
		}
		Peer* peer = empty != nullptr ? empty : oldest;
		memset(peer, 0, sizeof(*peer));
		peer->used = true;
		memcpy(peer->mac, mac, 6);
		peer->last_seen = millis();
		if (discovery != nullptr) peer->discovery = *discovery;
		return peer;
	}

	Reassembly* reassembly_for(const uint8_t* mac, uint32_t now) {
		Reassembly* empty = nullptr;
		Reassembly* oldest = &_reassembly[0];
		for (uint8_t i = 0; i < REASSEMBLY_SLOTS; ++i) {
			Reassembly& slot = _reassembly[i];
			if (slot.used && memcmp(slot.mac, mac, 6) == 0) return &slot;
			if (!slot.used && empty == nullptr) empty = &slot;
			if (slot.updated < oldest->updated) oldest = &slot;
		}
		Reassembly* slot = empty != nullptr ? empty : oldest;
		if (slot->used) _rx_dropped++;
		memset(slot, 0, sizeof(*slot));
		slot->used = true;
		memcpy(slot->mac, mac, 6);
		slot->updated = now;
		return slot;
	}

	void handle_fragment(const uint8_t* mac, const ESPNowFrameHeader& header,
	                     const uint8_t* payload, size_t payload_length) {
		if (header.fragment_count == 0 || header.fragment_count > ESPNOW_MAX_FRAGMENTS ||
		    header.fragment_index >= header.fragment_count || header.total_length == 0 ||
		    header.total_length > ESPNOW_RNS_MTU) {
			_rx_dropped++;
			return;
		}
		const uint8_t expected_count = (uint8_t)((header.total_length + ESPNOW_FRAGMENT_PAYLOAD - 1) /
		                                          ESPNOW_FRAGMENT_PAYLOAD);
		const size_t offset = (size_t)header.fragment_index * ESPNOW_FRAGMENT_PAYLOAD;
		if (header.fragment_count != expected_count || offset >= header.total_length) {
			_rx_dropped++;
			return;
		}
		const size_t remaining = header.total_length - offset;
		const size_t expected_length = remaining > ESPNOW_FRAGMENT_PAYLOAD
		                             ? ESPNOW_FRAGMENT_PAYLOAD : remaining;
		if (payload_length != expected_length) {
			_rx_dropped++;
			return;
		}

		const uint32_t now = millis();
		Reassembly* slot = reassembly_for(mac, now);
		if (header.fragment_index == 0) {
			slot->packet_id = header.packet_id;
			slot->total_length = header.total_length;
			slot->fragment_count = header.fragment_count;
			slot->next_fragment = 0;
			slot->received = 0;
		}
		else if (slot->packet_id != header.packet_id ||
		         slot->total_length != header.total_length ||
		         slot->fragment_count != header.fragment_count ||
		         slot->next_fragment != header.fragment_index) {
			slot->used = false;
			_rx_dropped++;
			return;
		}
		if (slot->next_fragment != header.fragment_index) {
			slot->used = false;
			_rx_dropped++;
			return;
		}

		memcpy(slot->data + offset, payload, payload_length);
		slot->received = (uint16_t)(slot->received + payload_length);
		slot->next_fragment++;
		slot->updated = now;
		if (slot->next_fragment == slot->fragment_count) {
			if (slot->received == slot->total_length) {
				RNS::Bytes packet(slot->data, slot->total_length);
				slot->used = false;
				_packets_in++;
				try {
					const uint32_t accepted_before = RNS::Transport::packets_received();
					handle_incoming(packet);
					if (RNS::Transport::packets_received() != accepted_before) {
						_accepted_packets_in++;
						if (_recovery_state == RECOVERY_PINNED &&
						    memcmp(mac, _recovery_peer_mac, 6) == 0) {
							_accepted_from_selected++;
							_recovery_peer_last_seen = millis();
						}
					}
				}
				catch (const std::bad_alloc&) {
					_rx_dropped++;
					ERROR("ESPNowInterface::handle_incoming: bad_alloc");
				}
				catch (const std::exception& e) {
					_rx_dropped++;
					ERRORF("ESPNowInterface::handle_incoming: %s", e.what());
				}
			}
			else {
				slot->used = false;
				_rx_dropped++;
			}
		}
	}

	void expire_state(uint32_t now) {
		if ((uint32_t)(now - _last_expiry) < 1000) return;
		_last_expiry = now;
		for (uint8_t i = 0; i < MAX_PEERS; ++i) {
			if (_peers[i].used && (uint32_t)(now - _peers[i].last_seen) > ESPNOW_PEER_TIMEOUT_MS) {
				_peers[i].used = false;
			}
		}
		for (uint8_t i = 0; i < REASSEMBLY_SLOTS; ++i) {
			if (_reassembly[i].used &&
			    (uint32_t)(now - _reassembly[i].updated) > ESPNOW_REASSEMBLY_TIMEOUT_MS) {
				_reassembly[i].used = false;
				_reassembly_timeouts++;
			}
		}
	}

	void service_send_completion(uint32_t now) {
		bool done = false;
		bool ok = false;
		portENTER_CRITICAL(&_send_mux);
		if (_send_done) {
			done = true;
			ok = _send_ok;
			_send_done = false;
		}
		portEXIT_CRITICAL(&_send_mux);

		if (!done && _send_waiting && (uint32_t)(now - _send_started) > SEND_TIMEOUT_MS) {
			_send_failures++;
			_send_waiting = false;
			_send_kind = SEND_NONE;
			stop();
			return;
		}
		if (!done) return;
		_send_waiting = false;
		if (_send_kind == SEND_DISCOVERY || _send_kind == SEND_SOLICIT ||
		    _send_kind == SEND_RECOVERY_REPLY) {
			if (!ok) _send_failures++;
			_send_kind = SEND_NONE;
			_send_retry = 0;
			return;
		}
		if (_send_kind != SEND_DATA) return;

		if (!ok && _send_retry < SEND_RETRIES) {
			_send_failures++;
			_send_retry++;
			_send_kind = SEND_NONE;
			return;
		}
		if (!ok) {
			_send_failures++;
			_tx_packet_failed = true;
		}
		_send_retry = 0;
		_send_kind = SEND_NONE;
		_tx_fragment++;
		TxPacket& packet = _tx_queue[_tx_tail];
		const uint8_t count = fragment_count(packet.length);
		if (_tx_fragment >= count) {
			if (_tx_packet_failed) _tx_dropped++;
			else _packets_out++;
			_tx_tail = (uint8_t)((_tx_tail + 1u) % TX_QUEUE_DEPTH);
			_tx_fragment = 0;
			_tx_packet_failed = false;
		}
	}

	static uint8_t fragment_count(uint16_t length) {
		return (uint8_t)((length + ESPNOW_FRAGMENT_PAYLOAD - 1) / ESPNOW_FRAGMENT_PAYLOAD);
	}

	void start_recovery_scan(uint32_t now) {
		// Stop the unsuccessful association attempt before setting channels. This
		// does not erase credentials; Remote.h will call WiFi.begin() again when
		// the station retry window arrives.
		WiFi.disconnect(false, false);

		wifi_country_t country = {};
		_scan_first_channel = 1;
		_scan_last_channel = 11;
		if (esp_wifi_get_country(&country) == ESP_OK && country.nchan > 0) {
			_scan_first_channel = country.schan < 1 ? 1 : country.schan;
			uint16_t last = (uint16_t)country.schan + country.nchan - 1u;
			_scan_last_channel = last > 13 ? 13 : (uint8_t)last;
		}
		if (_scan_first_channel > _scan_last_channel) {
			_scan_first_channel = 1;
			_scan_last_channel = 11;
		}
		if (_recovery_rendezvous < _scan_first_channel ||
		    _recovery_rendezvous > _scan_last_channel) {
			_recovery_rendezvous = _scan_first_channel;
		}

		_recovery_state = RECOVERY_SCANNING;
		_recovery_scan_deadline = now + _recovery_budget_ms;
		_recovery_next_hop = now;
		_scan_channel = 0;
		_scan_first_hop = true;
		_recovery_scans++;
		printf("[espnow] recovery scan started (%lums, channels %u-%u, rendezvous %u)\n",
		       (unsigned long)_recovery_budget_ms,
		       (unsigned)_scan_first_channel, (unsigned)_scan_last_channel,
		       (unsigned)_recovery_rendezvous);
	}

	void service_recovery_scan(uint32_t now) {
		if (time_reached(now, _recovery_scan_deadline)) {
			_recovery_state = RECOVERY_FAILED;
			_recovery_failures++;
			printf("[espnow] recovery scan found no proven peer\n");
			return;
		}
		if (!time_reached(now, _recovery_next_hop)) return;

		uint8_t next_channel;
		if (_scan_first_hop) {
			next_channel = _recovery_rendezvous;
			_scan_first_hop = false;
		}
		else {
			next_channel = _scan_channel >= _scan_last_channel
			             ? _scan_first_channel : (uint8_t)(_scan_channel + 1u);
		}
		_scan_channel = next_channel;
		if (esp_wifi_set_channel(next_channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
			_recovery_channel_errors++;
			_recovery_next_hop = now + 50;
			return;
		}
		refresh_channel();
		_recovery_nonce = esp_random();
		if (_recovery_nonce == 0) _recovery_nonce = 1;
		send_solicit(now);
		// Spend longer on the fleet rendezvous channel so two orphans entering
		// recovery a little apart can still meet. Other channels are active
		// probes and need only enough time for a jittered response.
		_recovery_next_hop = now +
		    (next_channel == _recovery_rendezvous ? 900u : 260u);
	}

	void send_current_fragment(uint32_t now) {
		TxPacket& packet = _tx_queue[_tx_tail];
		const uint8_t count = fragment_count(packet.length);
		if (_tx_fragment >= count) {
			_tx_dropped++;
			_tx_tail = (uint8_t)((_tx_tail + 1u) % TX_QUEUE_DEPTH);
			_tx_fragment = 0;
			return;
		}
		const size_t offset = (size_t)_tx_fragment * ESPNOW_FRAGMENT_PAYLOAD;
		const size_t remaining = packet.length - offset;
		const size_t payload_length = remaining > ESPNOW_FRAGMENT_PAYLOAD
		                            ? ESPNOW_FRAGMENT_PAYLOAD : remaining;
		uint8_t wire[ESPNOW_WIRE_MTU];
		ESPNowFrameHeader header = {
			ESPNOW_FRAME_DATA, packet.packet_id, _tx_fragment, count, packet.length
		};
		espnow_write_header(wire, sizeof(wire), header);
		memcpy(wire + ESPNOW_HEADER_SIZE, packet.data + offset, payload_length);
		begin_send(SEND_DATA, wire, ESPNOW_HEADER_SIZE + payload_length, now);
	}

	ESPNowDiscovery local_discovery() const {
		ESPNowDiscovery discovery = {};
#if defined(LORA_TRANSPORT)
		extern uint32_t lora_phy_hash();
		extern uint32_t lora_freq;
		extern uint32_t lora_bw;
		extern int lora_sf;
		extern int lora_cr;
		discovery.phy_hash = lora_phy_hash();
		discovery.frequency = lora_freq;
		discovery.bandwidth = lora_bw;
		discovery.spreading_factor = (uint8_t)lora_sf;
		discovery.coding_rate = (uint8_t)lora_cr;
		discovery.capabilities |= ESPNOW_CAP_LORA;
#endif
		if (RNS::Reticulum::transport_enabled()) discovery.capabilities |= ESPNOW_CAP_TRANSPORT;
		if (_ifac_key.size() > 0) discovery.capabilities |= ESPNOW_CAP_IFAC_PROOF;
		discovery.wifi_channel = _channel;
		return discovery;
	}

	void send_discovery(uint32_t now) {
		uint8_t wire[ESPNOW_HEADER_SIZE + ESPNOW_DISCOVERY_SIZE];
		ESPNowFrameHeader header = {
			ESPNOW_FRAME_DISCOVERY, 0, 0, 1, ESPNOW_DISCOVERY_SIZE
		};
		espnow_write_header(wire, sizeof(wire), header);
		ESPNowDiscovery discovery = local_discovery();
		espnow_write_discovery(wire + ESPNOW_HEADER_SIZE, ESPNOW_DISCOVERY_SIZE, discovery);
		_next_discovery_at = now + ESPNOW_DISCOVERY_INTERVAL_MS + (esp_random() % 2000u);
		begin_send(SEND_DISCOVERY, wire, sizeof(wire), now);
	}

	void send_solicit(uint32_t now) {
		uint8_t wire[ESPNOW_HEADER_SIZE + ESPNOW_SOLICIT_SIZE];
		ESPNowFrameHeader header = {
			ESPNOW_FRAME_SOLICIT, 0, 0, 1, ESPNOW_SOLICIT_SIZE
		};
		espnow_write_header(wire, sizeof(wire), header);
		espnow_write_solicit(wire + ESPNOW_HEADER_SIZE,
		                      ESPNOW_SOLICIT_SIZE, _recovery_nonce);
		begin_send(SEND_SOLICIT, wire, sizeof(wire), now);
	}

	void make_recovery_proof(const uint8_t* material, uint8_t* proof) const {
		memset(proof, 0, ESPNOW_RECOVERY_PROOF_SIZE);
		if (_ifac_key.size() == 0) return;
		const RNS::Bytes salt(material, ESPNOW_SOLICIT_SIZE + ESPNOW_DISCOVERY_SIZE);
		const RNS::Bytes derived = RNS::Cryptography::hkdf(
		    ESPNOW_RECOVERY_PROOF_SIZE, _ifac_key, salt);
		memcpy(proof, derived.data(), ESPNOW_RECOVERY_PROOF_SIZE);
	}

	bool verify_recovery_proof(const uint8_t* material, const uint8_t* proof) const {
		uint8_t expected[ESPNOW_RECOVERY_PROOF_SIZE];
		make_recovery_proof(material, expected);
		uint8_t difference = 0;
		for (uint8_t i = 0; i < ESPNOW_RECOVERY_PROOF_SIZE; ++i) {
			difference |= expected[i] ^ proof[i];
		}
		return difference == 0;
	}

	void send_recovery_reply(uint32_t now) {
		uint8_t wire[ESPNOW_HEADER_SIZE + ESPNOW_RECOVERY_REPLY_SIZE];
		ESPNowFrameHeader header = {
			ESPNOW_FRAME_RECOVERY_REPLY, 0, 0, 1, ESPNOW_RECOVERY_REPLY_SIZE
		};
		espnow_write_header(wire, sizeof(wire), header);
		ESPNowDiscovery discovery = local_discovery();
		uint8_t material[ESPNOW_SOLICIT_SIZE + ESPNOW_DISCOVERY_SIZE];
		espnow_put_u32(material, _recovery_reply_nonce);
		espnow_write_discovery(material + ESPNOW_SOLICIT_SIZE,
		                         ESPNOW_DISCOVERY_SIZE, discovery);
		uint8_t proof[ESPNOW_RECOVERY_PROOF_SIZE];
		make_recovery_proof(material, proof);
		espnow_write_recovery_reply(wire + ESPNOW_HEADER_SIZE,
		                            ESPNOW_RECOVERY_REPLY_SIZE,
		                            _recovery_reply_nonce, discovery, proof);
		_recovery_reply_pending = false;
		begin_send(SEND_RECOVERY_REPLY, wire, sizeof(wire), now);
	}

	void begin_send(SendKind kind, const uint8_t* data, size_t length, uint32_t now) {
		static const uint8_t broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
		_send_kind = kind;
		_send_waiting = true;
		_send_started = now;
		const esp_err_t err = esp_now_send(broadcast_mac, data, length);
		if (err != ESP_OK) {
			_send_waiting = false;
			portENTER_CRITICAL(&_send_mux);
			_send_ok = false;
			_send_done = true;
			portEXIT_CRITICAL(&_send_mux);
		}
	}

	bool _started = false;
	wifi_interface_t _wifi_interface = WIFI_IF_STA;
	uint8_t _channel = 0;
	uint8_t _own_mac[6] = {0};
	uint32_t _last_start_try = 0;
	uint32_t _last_channel_check = 0;
	uint32_t _next_discovery_at = 0;
	uint32_t _last_expiry = 0;

	RecoveryState _recovery_state = RECOVERY_STRICT;
	uint32_t _recovery_budget_ms = 0;
	uint32_t _recovery_scan_deadline = 0;
	uint32_t _recovery_next_hop = 0;
	uint32_t _recovery_nonce = 0;
	uint32_t _recovery_pinned_since = 0;
	uint32_t _recovery_peer_last_seen = 0;
	uint32_t _recovery_reply_nonce = 0;
	uint32_t _recovery_reply_at = 0;
	uint8_t _recovery_rendezvous = 1;
	uint8_t _recovery_selected_channel = 0;
	uint8_t _scan_first_channel = 1;
	uint8_t _scan_last_channel = 11;
	uint8_t _scan_channel = 0;
	bool _scan_first_hop = true;
	bool _recovery_reply_pending = false;
	uint8_t _recovery_peer_mac[6] = {0};
	char _recovery_peer_text[18] = {0};

	Peer _peers[MAX_PEERS];
	Reassembly _reassembly[REASSEMBLY_SLOTS];
	RxWireFrame _rx_queue[RX_QUEUE_DEPTH];
	volatile uint8_t _rx_head = 0;
	volatile uint8_t _rx_tail = 0;
	portMUX_TYPE _rx_mux = portMUX_INITIALIZER_UNLOCKED;

	TxPacket _tx_queue[TX_QUEUE_DEPTH];
	uint8_t _tx_head = 0;
	uint8_t _tx_tail = 0;
	uint8_t _tx_fragment = 0;
	uint8_t _send_retry = 0;
	uint16_t _next_packet_id = 0;
	bool _tx_packet_failed = false;
	volatile bool _send_waiting = false;
	volatile bool _send_done = false;
	volatile bool _send_ok = false;
	SendKind _send_kind = SEND_NONE;
	uint32_t _send_started = 0;
	portMUX_TYPE _send_mux = portMUX_INITIALIZER_UNLOCKED;

	volatile uint32_t _rx_callback_drops = 0;
	uint32_t _packets_in = 0;
	uint32_t _packets_out = 0;
	uint32_t _discoveries_in = 0;
	uint32_t _rx_dropped = 0;
	uint32_t _tx_dropped = 0;
	uint32_t _send_failures = 0;
	uint32_t _reassembly_timeouts = 0;
	uint32_t _last_peer_phy_hash = 0;
	uint32_t _accepted_packets_in = 0;
	uint32_t _accepted_from_selected = 0;
	uint32_t _recovery_scans = 0;
	uint32_t _recovery_successes = 0;
	uint32_t _recovery_failures = 0;
	uint32_t _recovery_proof_failures = 0;
	uint32_t _recovery_channel_errors = 0;
};

inline void espnow_receive_trampoline(const uint8_t* mac, const uint8_t* data, int length) {
	if (espnow_active_interface != nullptr) {
		espnow_active_interface->receive_from_callback(mac, data, length);
	}
}

inline void espnow_send_trampoline(const uint8_t*, esp_now_send_status_t status) {
	if (espnow_active_interface != nullptr) {
		espnow_active_interface->send_from_callback(status);
	}
}

#endif  // HAS_WIFI && MCU_VARIANT == MCU_ESP32
