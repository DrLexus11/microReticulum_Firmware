// NimBLE backend for the Reticulum BLE peer interface.
//
// One BLEPeerInterface is a multi-access Reticulum medium. Every connected
// GATT peer receives outbound packets; inbound packets from every peer enter
// Reticulum on this interface. Connection-local identity and reassembly state
// must therefore remain keyed by the BLE connection, never stored globally.

#pragma once

#include <array>
#include <esp_mac.h>
#include <climits>

static_assert(BLE_PEER_MAX_CONNECTIONS >= 1,
              "BLE_PEER_MAX_CONNECTIONS must be at least one");
static_assert(BLE_PEER_MAX_CONNECTIONS <= CONFIG_BT_NIMBLE_MAX_CONNECTIONS,
              "NimBLE controller capacity is below BLE peer capacity");

class BLEPeerInterface;

inline BLEPeerInterface* ble_peer_nimble_active = nullptr;
inline void ble_peer_nimble_notify_trampoline(
    NimBLERemoteCharacteristic* characteristic, uint8_t* data, size_t length,
    bool is_notify);

class BLEPeerInterface : public RNS::InterfaceImpl,
                         public NimBLECharacteristicCallbacks,
                         public NimBLEScanCallbacks,
                         public NimBLEClientCallbacks,
                         public NimBLEServerCallbacks {
public:
	BLEPeerInterface(const char* name) : RNS::InterfaceImpl(name) {
		_IN = true;
		_OUT = true;
		_HW_MTU = 500;
		_bitrate = BLE_PEER_BITRATE;
	}

	BLEPeerInterface() : BLEPeerInterface("BLEPeerInterface") {}
	virtual ~BLEPeerInterface() { _name = "deleted"; }

	bool begin(const RNS::Bytes& identity_hash) {
		if (_begin_attempted) return false;
		_begin_attempted = true;
		if (identity_hash.size() != BLE_PEER_IDENTITY_SIZE) {
			printf("[blepeer] refusing to start: identity is %u bytes, expected %u\n",
			       (unsigned)identity_hash.size(), (unsigned)BLE_PEER_IDENTITY_SIZE);
			return false;
		}
		extern char bt_devname[11];
		if (!NimBLEDevice::isInitialized() &&
		    !NimBLEDevice::init(std::string(bt_devname))) {
			printf("[blepeer] NimBLE initialisation failed\n");
			return false;
		}

		NimBLEDevice::setMTU(BLE_PEER_MAX_ATTR);
		_server = NimBLEDevice::createServer();
		if (_server == nullptr) return false;
		_server->setCallbacks(this, false);
		_server->advertiseOnDisconnect(false);

		_service = _server->createService(BLE_PEER_SERVICE_UUID);
		if (_service == nullptr) return false;
		_rx = _service->createCharacteristic(
			BLE_PEER_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
		_tx = _service->createCharacteristic(
			BLE_PEER_TX_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
		_identity = _service->createCharacteristic(
			BLE_PEER_IDENTITY_UUID, NIMBLE_PROPERTY::READ);
		if (_rx == nullptr || _tx == nullptr || _identity == nullptr) return false;
		_rx->setCallbacks(this);
		_tx->setCallbacks(this);   // diagnostic: observe CCCD subscription
		_identity->setValue((uint8_t*)identity_hash.data(), identity_hash.size());

		NimBLEAdvertisementData advertisement;
		advertisement.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
		advertisement.setCompleteServices(NimBLEUUID(BLE_PEER_SERVICE_UUID));
		NimBLEAdvertisementData scan_response;
		scan_response.setName(std::string(bt_devname));

		NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
		if (advertising == nullptr) return false;
		advertising->setAdvertisementData(advertisement);
		advertising->setScanResponseData(scan_response);
		advertising->enableScanResponse(true);

		_identity_hash = identity_hash;
		_started = true;
		ble_peer_nimble_active = this;
		_last_scan = millis();
		_next_connect_attempt = millis() + BLE_PEER_PERIPHERAL_WINDOW_MS;
		adopt_stable_high_address();
		restart_advertising();
		printf("[blepeer] multi-peer service up, identity <%s>, capacity=%u\n",
		       identity_hash.toHex().c_str(), (unsigned)BLE_PEER_MAX_CONNECTIONS);
		return true;
	}

	bool started() const { return _started; }
	size_t reassembling() const {
		size_t total = 0;
		for (const PeerState& peer : _peers) total += peer.inbound.size();
		return total;
	}
	uint32_t packets_in() const { return _packets_in; }
	uint32_t packets_out() const { return _packets_out; }
	uint32_t fragments_dropped() const { return _dropped; }
	uint32_t keepalives_in() const { return _keepalives_in; }
	uint32_t last_in_size() const { return _last_in_size; }
	const char* last_in_hex() const { return _last_in_hex; }
	uint32_t last_out_size() const { return _last_out_size; }
	uint32_t last_mtu() const { return _last_mtu; }
	uint32_t identity_writes() const { return _identity_writes; }
	const char* last_frag_hdr() const { return _last_frag_hdr; }
	uint32_t frag_lone() const { return _frag_lone; }
	uint32_t frag_start() const { return _frag_start; }
	uint8_t connected_peers() const { return ready_count(); }
	uint8_t peer_capacity() const { return BLE_PEER_MAX_CONNECTIONS; }

	void loop() {
		if (!_started || _server == nullptr) return;

		process_peer_events();
		report_rejected();
		drain_inbound();

		if (_overflow_handle != BLE_HS_CONN_HANDLE_NONE) {
			const uint16_t handle = _overflow_handle;
			_overflow_handle = BLE_HS_CONN_HANDLE_NONE;
			printf("[blepeer] rejecting connection handle=%u: capacity %u reached\n",
			       (unsigned)handle, (unsigned)BLE_PEER_MAX_CONNECTIONS);
			_server->disconnect(handle);
		}

		if (_advertise_pending &&
		    (int32_t)(millis() - _advertise_not_before) >= 0) {
			_advertise_pending = false;
			restart_advertising();
		}

		consider_candidate();
		if (_connect_pending) {
			_connect_pending = false;
			connect_to_candidate();
		}
		scan_tick();

		reap_silent_peers();

		const bool connected = ready_count() > 0;
		if (connected && (millis() - _last_keepalive) >= BLE_PEER_KEEPALIVE_MS) {
			_last_keepalive = millis();
			const uint8_t beat = BLE_PEER_KEEPALIVE_BYTE;
			notify_raw(&beat, BLE_PEER_KEEPALIVE_SIZE);
		}

		// Some controllers stop legacy advertising after every accepted link.
		// Re-arm it while capacity remains, including while other peers are live.
		if (has_capacity() &&
		    (millis() - _last_advertise) >= BLE_PEER_ADVERTISE_REARM_MS) {
			restart_advertising();
		}

	}

	// Diagnostic. Columba completes its handshake and then closes the link one
	// second later, and nothing here tracks whether the client ever enabled
	// notifications -- notify_raw() pushes the moment an identity arrives. This
	// says whether the CCCD was written, and when, relative to that push.
	void onSubscribe(NimBLECharacteristic* characteristic,
	                 NimBLEConnInfo& conn_info, uint16_t sub_value) override {
#if defined(BLE_PEER_DIAG)
		printf("[blepeer] CCCD write handle=%u value=0x%04x (%s)\n",
		       (unsigned)conn_info.getConnHandle(), (unsigned)sub_value,
		       characteristic == _tx ? "tx" : "other");
#else
		(void)characteristic; (void)conn_info; (void)sub_value;
#endif
	}

	void onWrite(NimBLECharacteristic* characteristic,
	             NimBLEConnInfo& conn_info) override {
		if (!_started || characteristic != _rx) return;
		PeerState* peer = find_server(conn_info.getConnHandle());
		if (peer == nullptr || peer->disconnect_pending) return;
		std::string value = characteristic->getValue();
		if (!value.empty()) {
			ingest(*peer, (const uint8_t*)value.data(), value.length());
		}
	}

	void onResult(const NimBLEAdvertisedDevice* advertised_device) override {
		if (!_started || advertised_device == nullptr || !has_capacity() ||
		    any_client_connecting() || _connect_pending) return;
		const NimBLEAdvertisedDevice& device = *advertised_device;
		if (!device.isAdvertisingService(_service_uuid)) return;
		if (!device.isConnectable()) return;
		const NimBLEAddress& address = device.getAddress();
		if (address_active(address) || address_blacklisted(address)) return;

		const int8_t rssi = device.getRSSI();
		if (rssi < BLE_PEER_CONNECT_MIN_RSSI) return;
		const bool initiates =
			(uint64_t)NimBLEDevice::getAddress() < (uint64_t)address;
		if (_candidate_valid &&
		    (_candidate_initiates || !initiates) &&
		    (_candidate_initiates != initiates || _candidate_rssi >= rssi)) return;

		_candidate = address;
		_candidate_device = device;
		_candidate_rssi = rssi;
		_candidate_initiates = initiates;
		_candidate_valid = true;
	}

	void onConnect(NimBLEClient* client) override {
		PeerState* peer = find_client(client);
		if (peer == nullptr) return;
		peer->role = ROLE_CLIENT_SETUP;
		peer->setup_pending = true;
	}

	void onConnectFail(NimBLEClient* client, int reason) override {
		PeerState* peer = find_client(client);
		if (peer == nullptr) return;
		peer->reason = reason;
		peer->fail_pending = true;
	}

	void onDisconnect(NimBLEClient* client, int reason) override {
		PeerState* peer = find_client(client);
		if (peer == nullptr) return;
		peer->reason = reason;
		peer->disconnect_pending = true;
	}

	void onConnect(NimBLEServer*, NimBLEConnInfo& conn_info) override {
		if (active_count() >= BLE_PEER_MAX_CONNECTIONS) {
			_overflow_handle = conn_info.getConnHandle();
			return;
		}
		PeerState* peer = find_free();
		if (peer == nullptr) {
			_overflow_handle = conn_info.getConnHandle();
			return;
		}
		prepare_peer(*peer, ROLE_SERVER, conn_info.getAddress());
		peer->server_handle = conn_info.getConnHandle();
		peer->connect_pending = true;
		// A successful legacy advertisement is consumed by this connection.
		// Start another one from loop() if a slot remains.
		_advertise_not_before = millis();
		_advertise_pending = true;
	}

	void onDisconnect(NimBLEServer*, NimBLEConnInfo& conn_info,
	                  int reason) override {
		PeerState* peer = find_server(conn_info.getConnHandle());
		if (peer == nullptr) return;
		peer->reason = reason;
		peer->disconnect_pending = true;
		_advertise_not_before = millis();
		_advertise_pending = true;
	}

	void client_data(NimBLERemoteCharacteristic* characteristic,
	                 const uint8_t* data, size_t length) {
		PeerState* peer = find_remote_tx(characteristic);
		if (peer != nullptr && !peer->disconnect_pending) {
			ingest(*peer, data, length);
		}
	}

protected:
	virtual void handle_incoming(const RNS::Bytes& data) override {
		_last_in_size = data.size();
		size_t show = data.size() < 8 ? data.size() : 8;
		size_t at = 0;
		for (size_t i = 0; i < show; ++i) {
			at += snprintf(_last_in_hex + at, sizeof(_last_in_hex) - at,
			               "%02x", data.data()[i]);
		}
		_last_in_hex[at] = 0;
		try {
			InterfaceImpl::handle_incoming(data);
		} catch (const std::bad_alloc&) {
			ERROR("BLEPeerInterface::handle_incoming: out of memory");
		} catch (std::exception& e) {
			ERRORF("BLEPeerInterface::handle_incoming: %s", e.what());
		}
	}

	virtual bool send_outgoing(const RNS::Bytes& data) override {
		if (!_started || ready_count() == 0) return false;
		bool success = true;
		try {
			const size_t usable = ble_peer_usable_value_length(negotiated_mtu());
			const size_t payload = ble_peer_payload_size(usable);
			const size_t total = (data.size() + payload - 1) / payload;
			if (total == 0 || total > 65535) return false;

			uint8_t frame[BLE_PEER_HEADER_SIZE + BLE_PEER_MAX_ATTR];
			for (size_t i = 0; i < total; ++i) {
				const uint8_t type = (i == 0) ? BLE_PEER_TYPE_START
				                   : (i == total - 1) ? BLE_PEER_TYPE_END
				                                            : BLE_PEER_TYPE_CONTINUE;
				const size_t offset = i * payload;
				const size_t chunk = (data.size() - offset < payload)
				                   ? data.size() - offset : payload;
				ble_peer_write_header(frame, type, (uint16_t)i, (uint16_t)total);
				memcpy(frame + BLE_PEER_HEADER_SIZE, data.data() + offset, chunk);
				success = notify_raw(frame, BLE_PEER_HEADER_SIZE + chunk) && success;
			}
			_last_out_size = data.size();
			_last_mtu = usable + BLE_PEER_ATT_HEADER;
			_packets_out++;
			InterfaceImpl::handle_outgoing(data);
		} catch (const std::bad_alloc&) {
			ERROR("BLEPeerInterface::send_outgoing: out of memory");
			success = false;
		} catch (std::exception& e) {
			ERRORF("BLEPeerInterface::send_outgoing: %s", e.what());
			success = false;
		}
		return success;
	}

private:
	enum PeerRole : uint8_t {
		ROLE_NONE = 0,
		ROLE_SERVER,
		ROLE_CLIENT_CONNECTING,
		ROLE_CLIENT_SETUP,
		ROLE_CLIENT_READY,
	};

	struct PeerState {
		volatile uint8_t role = ROLE_NONE;
		NimBLEAddress address;
		uint16_t server_handle = BLE_HS_CONN_HANDLE_NONE;
		NimBLEClient* client = nullptr;
		NimBLERemoteCharacteristic* remote_rx = nullptr;
		NimBLERemoteCharacteristic* remote_tx = nullptr;
		RNS::Bytes identity;
		RNS::Bytes inbound;
		uint16_t expected = 0;
		uint16_t next_seq = 0;
		volatile bool connect_pending = false;
		volatile bool setup_pending = false;
		volatile bool fail_pending = false;
		volatile bool disconnect_pending = false;
		// millis() of the last thing heard FROM this peer; 0 while unused.
		uint32_t last_inbound = 0;
		volatile bool identity_pending = false;
		volatile int reason = 0;
	};

	void prepare_peer(PeerState& peer, PeerRole role,
	                  const NimBLEAddress& address) {
		peer.address = address;
		peer.server_handle = BLE_HS_CONN_HANDLE_NONE;
		peer.remote_rx = nullptr;
		peer.remote_tx = nullptr;
		peer.identity.clear();
		peer.inbound.clear();
		peer.expected = 0;
		peer.next_seq = 0;
		peer.connect_pending = false;
		peer.setup_pending = false;
		peer.fail_pending = false;
		peer.last_inbound = millis();
		peer.disconnect_pending = false;
		peer.identity_pending = false;
		peer.reason = 0;
		peer.role = role;
	}

	void clear_peer(PeerState& peer) {
		NimBLEClient* expired_client = peer.client;
		peer.role = ROLE_NONE;
		peer.address = NimBLEAddress();
		peer.server_handle = BLE_HS_CONN_HANDLE_NONE;
		peer.remote_rx = nullptr;
		peer.remote_tx = nullptr;
		peer.identity.clear();
		peer.inbound.clear();
		peer.expected = 0;
		peer.next_seq = 0;
		peer.connect_pending = false;
		peer.setup_pending = false;
		peer.fail_pending = false;
		peer.disconnect_pending = false;
		peer.identity_pending = false;
		peer.reason = 0;
		peer.client = nullptr;
		// Delete only after the callback has returned and the client is no longer
		// connected. Retaining failed clients kept the GAP procedure alive long
		// enough that advertising->start() reported success but emitted nothing.
		if (expired_client != nullptr && !expired_client->isConnected()) {
			NimBLEDevice::deleteClient(expired_client);
		}
	}

	PeerState* find_free() {
		for (PeerState& peer : _peers) if (peer.role == ROLE_NONE) return &peer;
		return nullptr;
	}

	PeerState* find_server(uint16_t handle) {
		for (PeerState& peer : _peers) {
			if (peer.role == ROLE_SERVER && peer.server_handle == handle) return &peer;
		}
		return nullptr;
	}

	PeerState* find_client(NimBLEClient* client) {
		for (PeerState& peer : _peers) {
			if (peer.role != ROLE_NONE && peer.client == client) return &peer;
		}
		return nullptr;
	}

	PeerState* find_remote_tx(NimBLERemoteCharacteristic* characteristic) {
		for (PeerState& peer : _peers) {
			if (peer.role == ROLE_CLIENT_READY && peer.remote_tx == characteristic)
				return &peer;
		}
		return nullptr;
	}

	uint8_t active_count() const {
		uint8_t count = 0;
		for (const PeerState& peer : _peers) if (peer.role != ROLE_NONE) ++count;
		return count;
	}

	uint8_t ready_count() const {
		uint8_t count = 0;
		for (const PeerState& peer : _peers) {
			if (peer.role == ROLE_CLIENT_READY ||
			    (peer.role == ROLE_SERVER &&
			     peer.identity.size() == BLE_PEER_IDENTITY_SIZE &&
			     !peer.disconnect_pending)) ++count;
		}
		return count;
	}

	bool has_capacity() const { return active_count() < BLE_PEER_MAX_CONNECTIONS; }

	bool any_client_connecting() const {
		for (const PeerState& peer : _peers) {
			if (peer.role == ROLE_CLIENT_CONNECTING ||
			    peer.role == ROLE_CLIENT_SETUP) return true;
		}
		return false;
	}

	bool address_active(const NimBLEAddress& address) const {
		for (const PeerState& peer : _peers) {
			if (peer.role != ROLE_NONE && peer.address == address) return true;
		}
		return false;
	}

	void ingest(PeerState& peer, const uint8_t* data, size_t length) {
		if (length == 0) return;
		// Keepalives land here too, which is what makes this a liveness signal.
		peer.last_inbound = millis();
		if (length == BLE_PEER_KEEPALIVE_SIZE &&
		    data[0] == BLE_PEER_KEEPALIVE_BYTE) {
			_keepalives_in++;
			return;
		}

		if (length == BLE_PEER_IDENTITY_SIZE && peer.identity.size() == 0) {
			peer.identity = RNS::Bytes(data, length);
			peer.identity_pending = true;
			_identity_writes++;
			return;
		}

		if (length >= BLE_PEER_HEADER_SIZE) {
			snprintf(_last_frag_hdr, sizeof(_last_frag_hdr),
			         "%02x%02x%02x%02x%02x", data[0], data[1], data[2],
			         data[3], data[4]);
			if (data[0] == BLE_PEER_TYPE_LONE) _frag_lone++;
			if (data[0] == BLE_PEER_TYPE_START) _frag_start++;
		}

		uint8_t type;
		uint16_t seq, total;
		if (!ble_peer_read_header(data, length, type, seq, total)) {
			reject_frame(data, length);
			return;
		}

		const uint8_t* payload = data + BLE_PEER_HEADER_SIZE;
		const size_t payload_length = length - BLE_PEER_HEADER_SIZE;
		if (type == BLE_PEER_TYPE_LONE) {
			_packets_in++;
			enqueue_inbound(RNS::Bytes(payload, payload_length));
			return;
		}

		if (type == BLE_PEER_TYPE_START) {
			peer.inbound.clear();
			peer.expected = total;
			peer.next_seq = 0;
		} else if (peer.expected == 0 || total != peer.expected ||
		           seq != peer.next_seq) {
			reject_frame(data, length);
			peer.inbound.clear();
			peer.expected = 0;
			return;
		}

		peer.inbound.append(payload, payload_length);
		peer.next_seq = (uint16_t)(seq + 1);
		if (peer.next_seq >= peer.expected) {
			peer.expected = 0;
			_packets_in++;
			RNS::Bytes packet = peer.inbound;
			peer.inbound.clear();
			enqueue_inbound(packet);
		}
	}

	void reject_frame(const uint8_t* data, size_t length) {
		_dropped++;
		if (_reject_valid) return;
		_reject_length = (uint8_t)((length > sizeof(_reject_bytes))
		                           ? sizeof(_reject_bytes) : length);
		memcpy(_reject_bytes, data, _reject_length);
		_reject_total = length;
		_reject_valid = true;
	}

	void enqueue_inbound(const RNS::Bytes& packet) {
		if (_rx_lock == nullptr) return;
		if (xSemaphoreTake(_rx_lock, pdMS_TO_TICKS(10)) != pdTRUE) {
			_dropped++;
			return;
		}
		if (_rx_queue.size() >= BLE_PEER_RX_QUEUE_DEPTH) {
			_dropped++;
		} else {
			_rx_queue.push_back(packet);
		}
		xSemaphoreGive(_rx_lock);
	}

	void drain_inbound() {
		if (_rx_lock == nullptr) return;
		for (;;) {
			RNS::Bytes packet;
			if (xSemaphoreTake(_rx_lock, pdMS_TO_TICKS(10)) != pdTRUE) return;
			const bool have = !_rx_queue.empty();
			if (have) {
				packet = _rx_queue.front();
				_rx_queue.erase(_rx_queue.begin());
			}
			xSemaphoreGive(_rx_lock);
			if (!have) return;
			handle_incoming(packet);
		}
	}

	void report_rejected() {
		if (!_reject_valid) return;
		const uint32_t now = millis();
		if (now - _last_reject_log < 5000) {
			_reject_valid = false;
			return;
		}
		_last_reject_log = now;
		char hex[3 * sizeof(_reject_bytes) + 1];
		size_t at = 0;
		for (size_t i = 0; i < _reject_length; ++i) {
			at += snprintf(hex + at, sizeof(hex) - at, "%02x ", _reject_bytes[i]);
		}
		hex[at ? at - 1 : 0] = 0;
		printf("[blepeer] rejected %u-byte frame: %s (dropped=%u)\n",
		       (unsigned)_reject_total, hex, (unsigned)_dropped);
		_reject_valid = false;
	}

	bool duplicate_identity(const PeerState& subject) const {
		if (subject.identity.size() != BLE_PEER_IDENTITY_SIZE) return false;
		for (const PeerState& peer : _peers) {
			if (&peer != &subject && peer.role != ROLE_NONE &&
			    !peer.disconnect_pending && peer.identity == subject.identity) return true;
		}
		return false;
	}

	// Drop peers that have gone silent. See BLE_PEER_PEER_TIMEOUT_MS for why an
	// unreclaimed slot stops a returning node from rejoining at all.
	void reap_silent_peers() {
		const uint32_t now = millis();
		for (PeerState& peer : _peers) {
			if (peer.role == ROLE_NONE || peer.disconnect_pending) continue;
			if (peer.last_inbound == 0) continue;
			if ((now - peer.last_inbound) < BLE_PEER_PEER_TIMEOUT_MS) continue;
			printf("[blepeer] %s silent for %ums; reclaiming its slot\n",
			       peer.address.toString().c_str(),
			       (unsigned)(now - peer.last_inbound));
			peer.disconnect_pending = true;
			disconnect_peer(peer);
		}
	}

	void disconnect_peer(PeerState& peer) {
		if (peer.role == ROLE_SERVER && _server != nullptr) {
			_server->disconnect(peer.server_handle);
		} else if (peer.client != nullptr && peer.client->isConnected()) {
			peer.client->disconnect();
		} else {
			clear_peer(peer);
		}
	}

	void process_peer_events() {
		for (PeerState& peer : _peers) {
			if (peer.role == ROLE_NONE) continue;

			if (peer.connect_pending) {
				peer.connect_pending = false;
				printf("[blepeer] inbound link %s connected (handle=%u, peers=%u/%u, unpaired)\n",
				       peer.address.toString().c_str(), (unsigned)peer.server_handle,
				       (unsigned)active_count(), (unsigned)BLE_PEER_MAX_CONNECTIONS);
			}

			if (peer.fail_pending) {
				peer.fail_pending = false;
				printf("[blepeer] central connect to %s failed, reason=%d; retry in %ums\n",
				       peer.address.toString().c_str(), peer.reason,
				       (unsigned)BLE_PEER_CONNECT_RETRY_MS);
				blacklist_failed_address(peer.address);
				clear_peer(peer);
				_next_connect_attempt = millis() + BLE_PEER_CONNECT_RETRY_MS;
				schedule_advertising(BLE_PEER_ADVERTISE_SETTLE_MS);
				continue;
			}

			if (peer.disconnect_pending) {
				peer.disconnect_pending = false;
				printf("[blepeer] %s link %s disconnected, reason=%d\n",
				       peer.role == ROLE_SERVER ? "inbound" : "central",
				       peer.address.toString().c_str(), peer.reason);
				clear_peer(peer);
				schedule_advertising(BLE_PEER_ADVERTISE_SETTLE_MS);
				continue;
			}

			if (peer.setup_pending) {
				peer.setup_pending = false;
				if (!finish_client_connection(peer)) disconnect_peer(peer);
				else schedule_advertising(BLE_PEER_ADVERTISE_SETTLE_MS);
			}

			if (peer.identity_pending) {
				peer.identity_pending = false;
				if (duplicate_identity(peer)) {
					printf("[blepeer] duplicate identity <%s> at %s; closing newer link\n",
					       peer.identity.toHex().c_str(), peer.address.toString().c_str());
					disconnect_peer(peer);
				} else {
					printf("[blepeer] accepted peer identity <%s> from %s (peers=%u/%u)\n",
					       peer.identity.toHex().c_str(), peer.address.toString().c_str(),
					       (unsigned)ready_count(), (unsigned)BLE_PEER_MAX_CONNECTIONS);
				}
			}
		}
	}

	uint16_t negotiated_mtu() const {
		uint16_t mtu = BLE_PEER_MAX_MTU;
		bool found = false;
		for (const PeerState& peer : _peers) {
			uint16_t candidate = 0;
			if (peer.role == ROLE_CLIENT_READY && peer.client != nullptr) {
				candidate = peer.client->getMTU();
			} else if (peer.role == ROLE_SERVER &&
			           peer.identity.size() == BLE_PEER_IDENTITY_SIZE &&
			           !peer.disconnect_pending) {
				candidate = _server->getPeerMTU(peer.server_handle);
			}
			if (candidate > 0) {
				if (candidate < BLE_PEER_MIN_MTU) candidate = BLE_PEER_MIN_MTU;
				if (!found || candidate < mtu) mtu = candidate;
				found = true;
			}
		}
		return found ? mtu : BLE_PEER_MIN_MTU;
	}

	bool notify_raw(const uint8_t* data, size_t length) {
		bool sent = false;
		bool success = true;
		for (PeerState& peer : _peers) {
			if (peer.role == ROLE_CLIENT_READY && peer.remote_rx != nullptr) {
				sent = true;
				success = peer.remote_rx->writeValue(data, length, false) && success;
			}
		}
		for (const PeerState& peer : _peers) {
			if (peer.role == ROLE_SERVER &&
			    peer.identity.size() == BLE_PEER_IDENTITY_SIZE &&
			    !peer.disconnect_pending && _tx != nullptr) {
				sent = true;
				const bool ok = _tx->notify(data, length, peer.server_handle);
#if defined(BLE_PEER_DIAG)
				printf("[blepeer] notify handle=%u len=%u ok=%d\n",
				       (unsigned)peer.server_handle, (unsigned)length, (int)ok);
#endif
				success = ok && success;
			}
		}
		return sent && success;
	}

	bool should_connect(const NimBLEAddress& address) const {
		return (uint64_t)NimBLEDevice::getAddress() < (uint64_t)address;
	}

	void consider_candidate() {
		if (!_candidate_valid || !has_capacity()) return;
		NimBLEScan* scan = NimBLEDevice::getScan();
		if (scan != nullptr && scan->isScanning()) return;
		_candidate_valid = false;
		if (any_client_connecting() || !retry_due() ||
		    address_active(_candidate)) return;

		if (!should_connect(_candidate)) {
			printf("[blepeer] %s has lower address; advertising for its connection\n",
			       _candidate.toString().c_str());
			return;
		}
		_connect_address = _candidate;
		_connect_device = _candidate_device;
		_connect_pending = true;
		printf("[blepeer] discovered %s (rssi=%d); connecting as central\n",
		       _connect_address.toString().c_str(), (int)_candidate_rssi);
	}

	void scan_tick() {
		if (!has_capacity() || any_client_connecting() || !retry_due()) return;
		const uint32_t now = millis();
		if (now - _last_scan < BLE_PEER_SCAN_INTERVAL_MS) return;
		_last_scan = now;
		NimBLEScan* scan = NimBLEDevice::getScan();
		if (scan == nullptr) return;
		if (!scan->isScanning()) scan->clearResults();
		_candidate_valid = false;
		_candidate_rssi = INT8_MIN;
		_candidate_initiates = false;
		scan->setScanCallbacks(this, false);
		scan->setActiveScan(true);
		scan->setInterval(100);
		scan->setWindow(80);
		if (!scan->start(BLE_PEER_SCAN_SECONDS * 1000, false, true)) {
			printf("[blepeer] scan start failed\n");
		}
	}

	void connect_to_candidate() {
		if (!has_capacity() || address_active(_connect_address)) return;
		PeerState* peer = find_free();
		if (peer == nullptr) return;

		NimBLEScan* scan = NimBLEDevice::getScan();
		if (scan != nullptr) {
			scan->stop();
			scan->clearResults();
		}
		prepare_peer(*peer, ROLE_CLIENT_CONNECTING, _connect_address);
		if (peer->client == nullptr) {
			peer->client = NimBLEDevice::createClient();
			if (peer->client != nullptr) {
				peer->client->setClientCallbacks(this, false);
				peer->client->setConnectRetries(0);
				peer->client->setConnectTimeout(BLE_PEER_CONNECT_TIMEOUT_MS);
			}
		}
		if (peer->client == nullptr) {
			printf("[blepeer] cannot allocate central client\n");
			clear_peer(*peer);
			_advertise_pending = true;
			return;
		}

		printf("[blepeer] connecting asynchronously to %s\n",
		       peer->address.toString().c_str());
		// Connect from the complete live scan record, not only its address. Android
		// uses rotating private addresses and the advertised-device overload keeps
		// the address type and current advertising metadata together.
		if (!peer->client->connect(&_connect_device, true, true, true)) {
			peer->reason = peer->client->getLastError();
			peer->fail_pending = true;
		}
	}

	bool finish_client_connection(PeerState& peer) {
		if (peer.client == nullptr || !peer.client->isConnected()) return false;
		NimBLERemoteService* service =
			peer.client->getService(NimBLEUUID(BLE_PEER_SERVICE_UUID));
		if (service == nullptr) return false;
		peer.remote_rx = service->getCharacteristic(NimBLEUUID(BLE_PEER_RX_UUID));
		peer.remote_tx = service->getCharacteristic(NimBLEUUID(BLE_PEER_TX_UUID));
		NimBLERemoteCharacteristic* identity =
			service->getCharacteristic(NimBLEUUID(BLE_PEER_IDENTITY_UUID));
		if (peer.remote_rx == nullptr || peer.remote_tx == nullptr ||
		    identity == nullptr || !identity->canRead()) return false;

		std::string identity_value = identity->readValue();
		if (identity_value.size() != BLE_PEER_IDENTITY_SIZE) return false;
		peer.identity = RNS::Bytes((const uint8_t*)identity_value.data(),
		                           identity_value.size());
		if (!peer.remote_tx->subscribe(true, &ble_peer_nimble_notify_trampoline,
		                               true)) return false;
		if (!peer.remote_rx->writeValue((uint8_t*)_identity_hash.data(),
		                                _identity_hash.size(), true)) return false;

		peer.role = ROLE_CLIENT_READY;
		peer.identity_pending = true;
		printf("[blepeer] central link %s ready, mtu=%u (peers=%u/%u)\n",
		       peer.address.toString().c_str(), (unsigned)peer.client->getMTU(),
		       (unsigned)ready_count(), (unsigned)BLE_PEER_MAX_CONNECTIONS);
		return true;
	}

	// Take a stable BLE address that sorts above any phone, so we never dial one.
	//
	// The protocol breaks the connect/wait symmetry by address: lower dials,
	// higher waits. Columba computes its own side from
	// BluetoothAdapter.getAddress(), which Android returns to unprivileged apps
	// as the constant 02:00:00:00:00:00 -- below every real address -- so a phone
	// always elects itself central. If we also dial, BOTH links exist, and
	// Columba resolves the collision in resolveDualConnectionAction(), which is
	// reached only when one address holds a central AND a peripheral role.
	// Measured on the phone, one second after a completed handshake:
	//
	//   Connecting to 40:91:51:9B:2D:D2...
	//   Deduplication: disconnecting central connection to 40:91:51:9B:2D:D2
	//
	// Rev 1 never suffers this: its public address 80:b5:4e:.. sorts above a
	// phone's private address, so it never dials, only one link is ever made,
	// dedup returns NONE, and its sessions run for hours. This board's Espressif
	// public address 40:91:51:.. sorts below, so it dialled and broke its own
	// inbound link. A static random address -- which the specification requires
	// to carry its two most significant bits set -- is always above a phone's,
	// putting this board in exactly Rev 1's position.
	//
	// Derived from the factory MAC and NOT randomised per boot. A changing
	// address is a new device to a client every time: Columba's candidate list
	// never prunes, and rotating made one board appear as four addresses with
	// candidates climbing 9 -> 10 -> 11 -> 12 against its cap of seven, so the
	// ghosts consumed the peer budget.
	void adopt_stable_high_address() {
		uint8_t mac[6];
		if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) return;
		// NimBLE takes the address little-endian; the most significant byte is last.
		uint8_t rnd[6] = { mac[5], mac[4], mac[3], mac[2], mac[1], mac[0] };
		rnd[5] |= 0xC0;
		if (!NimBLEDevice::setOwnAddr(rnd) ||
		    !NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM)) {
			printf("[blepeer] could not adopt a high static address; we may dial "
			       "phones and trip their dual-connection dedup\n");
			return;
		}
		printf("[blepeer] static address %s (sorts above phones, so we wait)\n",
		       NimBLEDevice::getAddress().toString().c_str());
	}

	// Address rotation was tried here and removed. It did make Columba fire
	// onDeviceDiscovered again -- but its candidate list never prunes, so every
	// rotation added a permanent phantom peer. Measured on the phone: one board
	// present as four addresses, candidates climbing 9 -> 10 -> 11 -> 12 while
	// the client selected only its cap of 7, so the live address stopped being
	// chosen and the ghosts consumed the peer budget. See docs/Backlog.md.

	void restart_advertising() {
		NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
		if (advertising == nullptr) return;
		if (!has_capacity()) {
			if (advertising->isAdvertising()) advertising->stop();
			return;
		}
		if (advertising->isAdvertising()) {
			_last_advertise = millis();
			return;
		}
		if (advertising->start()) {
			_last_advertise = millis();
			printf("[blepeer] advertising active (peers=%u/%u)\n",
			       (unsigned)active_count(), (unsigned)BLE_PEER_MAX_CONNECTIONS);
		} else {
			// Do not advance _last_advertise on failure; the main loop retries.
			printf("[blepeer] advertising start failed; retrying\n");
			schedule_advertising(BLE_PEER_ADVERTISE_SETTLE_MS);
		}
	}

	void schedule_advertising(uint32_t delay_ms) {
		_advertise_not_before = millis() + delay_ms;
		_advertise_pending = true;
	}

	bool retry_due() const {
		return _next_connect_attempt == 0 ||
		       (int32_t)(millis() - _next_connect_attempt) >= 0;
	}

	bool address_blacklisted(const NimBLEAddress& address) const {
		const uint32_t now = millis();
		for (size_t i = 0; i < BLE_PEER_CONNECT_BLACKLIST_SIZE; ++i) {
			if (_failed_until[i] != 0 &&
			    (int32_t)(now - _failed_until[i]) < 0 &&
			    _failed_addresses[i] == address) return true;
		}
		return false;
	}

	void blacklist_failed_address(const NimBLEAddress& address) {
		const uint32_t now = millis();
		size_t slot = 0;
		for (size_t i = 0; i < BLE_PEER_CONNECT_BLACKLIST_SIZE; ++i) {
			if (_failed_addresses[i] == address) { slot = i; break; }
			if (_failed_until[i] == 0 || (int32_t)(now - _failed_until[i]) >= 0) {
				slot = i;
				break;
			}
			if (_failed_until[i] < _failed_until[slot]) slot = i;
		}
		_failed_addresses[slot] = address;
		_failed_until[slot] = now + BLE_PEER_CONNECT_BLACKLIST_MS;
	}

	NimBLEServer* _server = nullptr;
	NimBLEService* _service = nullptr;
	NimBLECharacteristic* _rx = nullptr;
	NimBLECharacteristic* _tx = nullptr;
	NimBLECharacteristic* _identity = nullptr;
	RNS::Bytes _identity_hash;
	std::array<PeerState, BLE_PEER_MAX_CONNECTIONS> _peers;

	bool _started = false;
	bool _begin_attempted = false;
	volatile bool _advertise_pending = false;
	volatile uint32_t _advertise_not_before = 0;
	volatile bool _connect_pending = false;
	volatile uint16_t _overflow_handle = BLE_HS_CONN_HANDLE_NONE;
	uint32_t _last_scan = 0;
	uint32_t _last_advertise = 0;
	uint32_t _last_keepalive = 0;
	uint32_t _next_connect_attempt = 0;

	NimBLEUUID _service_uuid = NimBLEUUID(BLE_PEER_SERVICE_UUID);
	NimBLEAddress _candidate;
	NimBLEAddress _connect_address;
	NimBLEAdvertisedDevice _candidate_device;
	NimBLEAdvertisedDevice _connect_device;
	volatile bool _candidate_valid = false;
	volatile int8_t _candidate_rssi = INT8_MIN;
	volatile bool _candidate_initiates = false;
	std::array<NimBLEAddress, BLE_PEER_CONNECT_BLACKLIST_SIZE> _failed_addresses;
	uint32_t _failed_until[BLE_PEER_CONNECT_BLACKLIST_SIZE] = {0};

	std::vector<RNS::Bytes> _rx_queue;
	SemaphoreHandle_t _rx_lock = xSemaphoreCreateMutex();
	uint8_t _reject_bytes[12] = {0};
	uint8_t _reject_length = 0;
	uint16_t _reject_total = 0;
	volatile bool _reject_valid = false;
	uint32_t _last_reject_log = 0;

	uint32_t _packets_in = 0;
	uint32_t _packets_out = 0;
	uint32_t _dropped = 0;
	uint32_t _keepalives_in = 0;
	uint32_t _last_in_size = 0;
	char _last_in_hex[20] = {0};
	uint32_t _last_out_size = 0;
	uint32_t _last_mtu = 0;
	uint32_t _identity_writes = 0;
	char _last_frag_hdr[12] = {0};
	uint32_t _frag_lone = 0;
	uint32_t _frag_start = 0;
};

inline void ble_peer_nimble_notify_trampoline(
    NimBLERemoteCharacteristic* characteristic, uint8_t* data, size_t length,
    bool is_notify) {
	(void)is_notify;
	if (ble_peer_nimble_active != nullptr) {
		ble_peer_nimble_active->client_data(characteristic, data, length);
	}
}
