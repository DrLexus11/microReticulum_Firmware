// Reticulum BLE peer interface — the node as a peer, not a modem.
//
// A phone connects over BLE and joins the mesh *through* this node, exactly as
// it would over TCP: the node keeps its identity, its radio and its own stack,
// and BLE is simply another way in. Contrast BLESerial, which hands a host the
// modem and takes the node out of the network to serve one client.
//
// Wire format and UUIDs are in BLEPeerProtocol.h, transcribed from
// ble-reticulum rather than inferred, and pinned by
// tests/test_ble_peer_protocol.py.
//
// ROLE. Both, and it has to be both.
//
// The protocol breaks the symmetry by address: `shouldConnect = localMac <
// peerMac`, so the lower address initiates and the higher one waits as a
// peripheral. Advertising alone is therefore not enough. Android commonly
// presents a rotating private address, so either side can be lower on a given
// attempt and the over-the-air address cannot be treated as peer identity. A
// peripheral-only first attempt did deadlock: our service was on air at -43
// dBm, the client transmitted "51 bytes to 0 peer(s)", and neither side made a
// connection attempt.
//
// So this node also scans, applies the same comparison, and connects when its
// address is lower. Reticulum's 16-byte transport identity takes over after
// the GATT connection is established.

#pragma once

#include "Boards.h"

#if (HAS_BLE == true || defined(NIMBLE_PEER_TRANSPORT)) && MCU_VARIANT == MCU_ESP32

#include <microReticulum.h>
#if defined(NIMBLE_PEER_TRANSPORT)
#include <NimBLEDevice.h>
#else
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLE2902.h>
#endif

#include "BLEPeerProtocol.h"

class BLEPeerInterface;

// registerForNotify() takes a plain function pointer, not a bound method, so
// the client role needs a file-scope hook and a way back to the instance.
inline BLEPeerInterface* ble_peer_active = nullptr;
inline void ble_peer_notify_trampoline(BLERemoteCharacteristic* characteristic,
                                       uint8_t* data, size_t length,
                                       bool is_notify);

class BLEPeerInterface : public RNS::InterfaceImpl,
                         public BLECharacteristicCallbacks,
                         public BLEAdvertisedDeviceCallbacks
#if defined(NIMBLE_PEER_TRANSPORT)
                       , public NimBLEClientCallbacks,
                         public NimBLEServerCallbacks
#endif
                         {

public:
	BLEPeerInterface(const char* name) : RNS::InterfaceImpl(name) {
		_IN  = true;
		_OUT = true;
		// Reticulum's own packet ceiling. Fragmentation below carries whatever
		// this yields across an ATT MTU that is usually far smaller.
		_HW_MTU = 500;
		// RNS derives link and path timing from the interface bitrate, and a
		// zero bitrate is not "unknown" -- it is a division by zero.
		// Transport::extra_link_proof_timeout() computes (1.0/bitrate)*8*MTU,
		// so leaving this at its default gave every link an infinite
		// establishment deadline. Packets crossed, announces were relayed, and
		// no link ever completed: no messages, no pages, while the interface
		// looked perfectly healthy.
		//
		// This is an estimate of usable BLE throughput, not a measurement --
		// what matters to RNS is that it is finite and the right order of
		// magnitude. Every other interface here sets one.
		_bitrate = BLE_PEER_BITRATE;
	}
	BLEPeerInterface() : BLEPeerInterface("BLEPeerInterface") {}
	virtual ~BLEPeerInterface() { _name = "deleted"; }

#if defined(NIMBLE_PEER_TRANSPORT)
	// OZD has no PSRAM, and the legacy Bluedroid host beside Wi-Fi and ESP-NOW
	// leaves too little contiguous heap for Reticulum. Start NimBLE directly;
	// the other ESP32 targets continue to attach to their BLESerial server.
	bool begin(const RNS::Bytes& identity_hash) {
		if (_begin_attempted) return false;
		_begin_attempted = true;
		extern char bt_devname[11];
		if (!NimBLEDevice::isInitialized() &&
		    !NimBLEDevice::init(std::string(bt_devname))) {
			printf("[blepeer] NimBLE initialisation failed\n");
			return false;
		}
		NimBLEDevice::setMTU(BLE_PEER_MAX_ATTR);
		BLEServer* server = NimBLEDevice::createServer();
		if (server != nullptr) server->setCallbacks(this, false);
		return begin(server, identity_hash);
	}
#endif

	// Attach to an already-initialised BLE server. The identity hash is
	// published verbatim on the identity characteristic: peers track each other
	// by Reticulum identity because Android rotates its BLE MAC about every
	// fifteen minutes, and an address is therefore not a stable name for anyone.
	bool begin(BLEServer* server, const RNS::Bytes& identity_hash) {
		if (server == nullptr) return false;
		if (identity_hash.size() != BLE_PEER_IDENTITY_SIZE) {
			printf("[blepeer] refusing to start: identity is %u bytes, expected %d\n",
			       (unsigned)identity_hash.size(), BLE_PEER_IDENTITY_SIZE);
			return false;
		}
		_server = server;

		_service = server->createService(BLE_PEER_SERVICE_UUID);

		// Peer -> us. Write without response is what the reference uses for
		// throughput; accepting both costs nothing.
		_rx = _service->createCharacteristic(
			BLE_PEER_RX_UUID,
#if defined(NIMBLE_PEER_TRANSPORT)
			NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
#else
			BLECharacteristic::PROPERTY_WRITE |
			BLECharacteristic::PROPERTY_WRITE_NR);
#endif
		_rx->setCallbacks(this);

		// Us -> peer.
		_tx = _service->createCharacteristic(
			BLE_PEER_TX_UUID,
#if defined(NIMBLE_PEER_TRANSPORT)
			NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
#else
			BLECharacteristic::PROPERTY_READ |
			BLECharacteristic::PROPERTY_NOTIFY);
#endif
		// The CCCD needs its own write permission or a client cannot subscribe,
		// and a client that cannot subscribe has no reason to talk to us. That
		// exact omission cost a day on the other BLE service in this firmware.
#if !defined(NIMBLE_PEER_TRANSPORT)
		_tx->addDescriptor(new BLE2902());
#endif

		_identity = _service->createCharacteristic(
			BLE_PEER_IDENTITY_UUID,
#if defined(NIMBLE_PEER_TRANSPORT)
			NIMBLE_PROPERTY::READ);
#else
			BLECharacteristic::PROPERTY_READ);
#endif
		_identity->setValue((uint8_t*)identity_hash.data(), identity_hash.size());

#if !defined(NIMBLE_PEER_TRANSPORT)
		_service->start();
#endif

		// Advertise the service UUID itself, and do it by replacing the
		// advertisement rather than calling addServiceUUID().
		//
		// BLESerial sets custom advertisement data for the RNode/KISS service,
		// which sets m_customAdvData and makes addServiceUUID() a no-op -- the
		// peer UUID would never reach the air, and a scanner filtering on it
		// would find nothing however healthy the node was.
		//
		// The 31-byte budget forces a choice: flags (3) plus a 128-bit service
		// UUID (18) leaves no room for a name. For a peer that is the right
		// trade -- the phone finds this node by service UUID from inside the
		// app, not by reading a name off a system scan list. The name moves to
		// the scan response, where an active scanner still gets it.
		BLEAdvertisementData advertisement;
		advertisement.setFlags(
#if defined(NIMBLE_PEER_TRANSPORT)
			BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
#else
			ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
#endif
		advertisement.setCompleteServices(BLEUUID(BLE_PEER_SERVICE_UUID));

		BLEAdvertisementData scan_response;
		extern char bt_devname[11];
		scan_response.setName(std::string(bt_devname));

		BLEAdvertising* advertising = BLEDevice::getAdvertising();
		advertising->setAdvertisementData(advertisement);
		advertising->setScanResponseData(scan_response);
#if defined(NIMBLE_PEER_TRANSPORT)
		advertising->enableScanResponse(true);
#else
		advertising->setScanResponse(true);
#endif
		advertising->start();

		_identity_hash = identity_hash;
		_started = true;
		ble_peer_active = this;
		printf("[blepeer] service up, identity <%s>\n",
		       identity_hash.toHex().c_str());
		return true;
	}

	bool started() const { return _started; }
	size_t reassembling() const { return _inbound.size(); }
	uint32_t packets_in() const { return _packets_in; }
	uint32_t packets_out() const { return _packets_out; }
	uint32_t fragments_dropped() const { return _dropped; }
	uint32_t keepalives_in() const { return _keepalives_in; }
	uint32_t last_in_size() const  { return _last_in_size; }
	const char* last_in_hex() const { return _last_in_hex; }
	uint32_t last_out_size() const { return _last_out_size; }
	uint32_t last_mtu() const      { return _last_mtu; }
	uint32_t identity_writes() const { return _identity_writes; }
	const char* last_frag_hdr() const { return _last_frag_hdr; }
	uint32_t frag_lone() const  { return _frag_lone; }
	uint32_t frag_start() const { return _frag_start; }

	// Send the identity handshake and keep the link from idling out. Called
	// from the main loop, never from a callback.
	void loop() {
		if (!_started || _server == nullptr) return;

		// Complete a connection decided in the scan callback. Done here rather
		// than in the callback because connecting from inside a scan result is
		// how this library deadlocks.
		report_rejected();
		report_identity_handshake();
		drain_inbound();
#if defined(NIMBLE_PEER_TRANSPORT)
		process_client_events();
#endif

		// A peripheral connection (normally Columba) owns this transport until
		// it leaves. Starting a NimBLE scan while that server link is live can
		// block the Arduino loop in the host/controller call; on OZD this was
		// reproduced as a task-WDT reset exactly when an un-handshaken BlueZ
		// connection overlapped the next 20-second scan tick.
		bool server_connected = has_server_link();
		bool advertising_rearmed = false;
		if (!server_connected && _advertise_pending) {
			// A disconnect can land just as the central-role scan becomes due.
			// Stop that scan and give peripheral advertising a full interval of
			// exclusive ownership, otherwise advertising->start() may be rejected
			// and the device remains invisible until the 30-second safety rearm.
			BLEScan* scan = BLEDevice::getScan();
			if (scan != nullptr) scan->stop();
			_advertise_pending = false;
			_last_scan = millis();
			restart_advertising();
			advertising_rearmed = true;
		}
		if (server_connected) {
			_candidate_valid = false;
			_connect_pending = false;
#if defined(NIMBLE_PEER_TRANSPORT)
			// Columba may connect inbound while our own central attempt is still
			// pending. Prefer the already-established link and cancel the dial so
			// the two controllers do not remain in a central/central collision.
			if (_client_connecting && _client != nullptr) {
				_client->cancelConnect();
				_client_connecting = false;
			}
#endif
		} else if (!advertising_rearmed) {
			consider_candidate();
			if (_connect_pending) {
				_connect_pending = false;
				connect_to_peer();
			}
			scan_tick();
		}

		server_connected = has_server_link();
		const bool connected = server_connected || _client_connected;
		if (connected && !_was_connected) {
			// Nothing is pushed on connect. Our identity already sits on the
			// identity characteristic for the peer to read.
			//
			// This used to notify a bare 16-byte identity frame. The peer has
			// no such case: Columba hands every notification straight to its
			// defragmenter, so that frame was parsed as a fragment header and
			// corrupted the first thing reassembled on every connection --
			// while the link looked healthy and the identity, read from the
			// characteristic, displayed correctly.
			_last_keepalive = millis();
		}
		// Send our own heartbeat. Android reaps an idle BLE link after roughly
		// twenty to thirty seconds, and the constant existed here from the
		// start while the code that uses it did not -- so the link survived
		// only as long as traffic happened to keep it alive.
		if (connected && (millis() - _last_keepalive) >= BLE_PEER_KEEPALIVE_MS) {
			_last_keepalive = millis();
			const uint8_t beat = BLE_PEER_KEEPALIVE_BYTE;
			notify_raw(&beat, BLE_PEER_KEEPALIVE_SIZE);
		}

		if (!connected && _was_connected && !advertising_rearmed) {
			// Do not carry a half-finished packet across peers.
			_inbound.clear();
			_expected = 0;
			// The controller stops advertising as soon as a client connects, so
			// it must be restarted on every disconnect or the node is visible
			// exactly once per boot. BLESerial used to do this, and a peer
			// build had to stop it doing so -- leaving nobody to restart it.
			// The symptom is precise and misleading: the first connection works
			// and no later one ever does.
			restart_advertising();
		}
		_was_connected = connected;

		// Safety net. A missed disconnect callback, or an advertiser stopped by
		// something else in the firmware, would otherwise strand the node
		// unreachable until reboot. Re-arming while idle is cheap.
		if (!connected && (millis() - _last_advertise) >= BLE_PEER_ADVERTISE_REARM_MS) {
			restart_advertising();
		}
	}

	// --- inbound: peer writes a fragment to the RX characteristic ------------
#if defined(NIMBLE_PEER_TRANSPORT)
	void onWrite(BLECharacteristic* characteristic, NimBLEConnInfo& conn_info) override {
#else
	void onWrite(BLECharacteristic* characteristic) override {
#endif
		if (!_started || characteristic != _rx) return;
#if defined(NIMBLE_PEER_TRANSPORT)
		// This fixture exposes one Reticulum interface, so one inbound GATT link
		// owns its reassembly and identity state. A duplicate link is disconnected
		// by the main loop and must not overwrite the active peer's handshake.
		if (conn_info.getConnHandle() != _server_conn_handle) return;
#endif
		std::string value = characteristic->getValue();
		if (value.empty()) return;
		ingest((const uint8_t*)value.data(), value.length());
	}

	// Reassembly, shared by both roles: as a peripheral the peer writes to our
	// RX characteristic, as a central it notifies us on its TX. The bytes and
	// the framing are identical, so the handling must be too.
	void ingest(const uint8_t* data, size_t length) {
		if (length == 0) return;

		// The peer's heartbeat. Not a fragment, not a loss.
		if (length == BLE_PEER_KEEPALIVE_SIZE &&
		    data[0] == BLE_PEER_KEEPALIVE_BYTE) {
			_keepalives_in++;
			return;
		}

		// The identity handshake, per Columba's ble-architecture.md: a central
		// writes its 16-byte identity to the peripheral's RX characteristic,
		// and the peripheral detects it as "exactly 16 bytes AND no identity
		// recorded for this peer yet". That second clause is what makes it
		// unambiguous -- a fragment of 5 header bytes plus an 11-byte payload
		// is also 16 bytes, so length alone is not enough, and only the first
		// such write is a handshake.
		if (length == BLE_PEER_IDENTITY_SIZE && _peer_identity.size() == 0) {
			_peer_identity = RNS::Bytes(data, length);
			_identity_writes++;
			_identity_log_pending = true;
			return;
		}

		// Record the raw 5-byte fragment header exactly as received. Whatever
		// the peer uses for a single-fragment packet is what it expects back:
		// LONE (0x00) or START (0x01) with total=1. Getting this backwards
		// costs nothing inbound -- we accept both -- and silently discards
		// everything we transmit.
		if (length >= BLE_PEER_HEADER_SIZE) {
			snprintf(_last_frag_hdr, sizeof(_last_frag_hdr), "%02x%02x%02x%02x%02x",
			         data[0], data[1], data[2], data[3], data[4]);
			if (data[0] == BLE_PEER_TYPE_LONE)  _frag_lone++;
			if (data[0] == BLE_PEER_TYPE_START) _frag_start++;
		}

		uint8_t type; uint16_t seq, total;
		if (!ble_peer_read_header(data, length, type, seq, total)) {
			_dropped++;
			// Capture, do not print: this runs on a BLE task with no stack to
			// spare. The main loop prints it. Without this a rejected frame is
			// indistinguishable from a lost one, and the whole LONE bug looked
			// exactly like packet loss for a day.
			if (!_reject_valid) {
				_reject_length = (uint8_t)((length > sizeof(_reject_bytes))
				                           ? sizeof(_reject_bytes) : length);
				memcpy(_reject_bytes, data, _reject_length);
				_reject_total = length;
				_reject_valid = true;
			}
			return;
		}

		const uint8_t* payload = data + BLE_PEER_HEADER_SIZE;
		const size_t payload_length = length - BLE_PEER_HEADER_SIZE;

		// A lone fragment carries a complete packet. Deliver it without
		// touching reassembly state, so a lone packet arriving mid-reassembly
		// cannot corrupt a multi-fragment packet already in progress.
		if (type == BLE_PEER_TYPE_LONE) {
			_packets_in++;
			enqueue_inbound(RNS::Bytes(payload, payload_length));
			return;
		}

		if (type == BLE_PEER_TYPE_START) {
			_inbound.clear();
			_expected = total;
			_next_seq = 0;
		} else if (_expected == 0 || total != _expected || seq != _next_seq) {
			// Out of order or arriving with no START. Reticulum retransmits, so
			// discarding is correct and cheaper than holding partial state.
			_dropped++;
			// Capture here too: a frame with a valid header that is simply not
			// a START lands in this branch, and without the bytes it is
			// indistinguishable from genuine loss.
			if (!_reject_valid) {
				_reject_length = (uint8_t)((length > sizeof(_reject_bytes))
				                           ? sizeof(_reject_bytes) : length);
				memcpy(_reject_bytes, data, _reject_length);
				_reject_total = length;
				_reject_valid = true;
			}
			_inbound.clear();
			_expected = 0;
			return;
		}

		_inbound.append(payload, payload_length);
		_next_seq = (uint16_t)(seq + 1);

		// END completes the packet; the sequence check above already proved
		// every fragment arrived in order.
		if (_next_seq >= _expected) {
			_expected = 0;
			_packets_in++;
			const RNS::Bytes packet = _inbound;
			_inbound.clear();
			enqueue_inbound(packet);
		}
	}

	// Bring the advertiser back up. Safe to call when already advertising.
	void restart_advertising() {
		_last_advertise = millis();
		BLEAdvertising* advertising = BLEDevice::getAdvertising();
		if (advertising != nullptr) advertising->start();
	}

	// Print whatever the header parser last refused, at most once every few
	// seconds, from the main task.
	void report_rejected() {
		if (!_reject_valid) return;
		const uint32_t now = millis();
		if (now - _last_reject_log < 5000) { _reject_valid = false; return; }
		_last_reject_log = now;
		char hex[3 * sizeof(_reject_bytes) + 1];
		size_t at = 0;
		for (size_t i = 0; i < _reject_length; i++)
			at += snprintf(hex + at, sizeof(hex) - at, "%02x ", _reject_bytes[i]);
		hex[at ? at - 1 : 0] = 0;
		printf("[blepeer] rejected %u-byte frame: %s (dropped=%u)\n",
		       (unsigned)_reject_total, hex, (unsigned)_dropped);
		_reject_valid = false;
	}

	void report_identity_handshake() {
		if (!_identity_log_pending) return;
		_identity_log_pending = false;
		printf("[blepeer] accepted peer identity <%s> over unpaired GATT\n",
		       _peer_identity.toHex().c_str());
	}

	// Reassembly runs in a BLE callback -- BTC_TASK for a server write, the
	// client task for a notification -- and neither has the stack for
	// Reticulum's inbound path, which parses, decrypts and routes. Calling
	// handle_incoming() there tripped "Stack canary watchpoint triggered
	// (BTC_TASK)" and rebooted the node on the first packet of every
	// connection. The callback now only hands the packet over.
	void enqueue_inbound(const RNS::Bytes& packet) {
		if (_rx_lock == nullptr) return;
		if (xSemaphoreTake(_rx_lock, pdMS_TO_TICKS(10)) != pdTRUE) { _dropped++; return; }
		if (_rx_queue.size() >= BLE_PEER_RX_QUEUE_DEPTH) {
			// Full means the main loop is not draining. Drop the newest rather
			// than growing without bound; Reticulum retransmits.
			_dropped++;
		} else {
			_rx_queue.push_back(packet);
		}
		xSemaphoreGive(_rx_lock);
	}

	// Main task: hand queued packets to Reticulum with a real stack underneath.
	void drain_inbound() {
		if (_rx_lock == nullptr) return;
		// Drain until empty rather than a fixed slice: a cap equal to the queue
		// depth cannot keep up with a burst that arrives while the loop is
		// busy, and the backlog then costs packets at the producer instead.
		for (;;) {
			RNS::Bytes packet;
			if (xSemaphoreTake(_rx_lock, pdMS_TO_TICKS(10)) != pdTRUE) return;
			const bool have = !_rx_queue.empty();
			if (have) { packet = _rx_queue.front(); _rx_queue.erase(_rx_queue.begin()); }
			xSemaphoreGive(_rx_lock);
			if (!have) return;
			handle_incoming(packet);
		}
	}

	// Lower address initiates; higher waits. Addresses compare as the byte
	// strings they are, which is what the reference does after stripping
	// separators.
	bool should_connect(const std::string& peer_address) const {
		std::string ours = BLEDevice::getAddress().toString();
		std::string theirs = peer_address;
		auto strip = [](std::string& v) {
			std::string out;
			for (char c : v) if (c != ':') out += (char)tolower((unsigned char)c);
			v = out;
		};
		strip(ours); strip(theirs);
		return ours < theirs;
	}

	// --- central role -------------------------------------------------------

	// A scan result carrying our service UUID. Decide by address whether we are
	// the side that connects, exactly as the reference does.
#if defined(NIMBLE_PEER_TRANSPORT)
	void onResult(const BLEAdvertisedDevice* advertised_device) override {
		if (advertised_device == nullptr) return;
		const BLEAdvertisedDevice& device = *advertised_device;
#else
	void onResult(BLEAdvertisedDevice device) override {
#endif
		// Runs on BTC_TASK, whose stack is small and fixed. Do as close to
		// nothing as possible: record the candidate and return. An earlier
		// version compared addresses and printf'd here, and the combination of
		// std::string temporaries and printf overflowed the task stack --
		// "Stack canary watchpoint triggered (BTC_TASK)" -- rebooting the node
		// about ten seconds after every peer appeared.
		if (!_started || _client_connected || _connect_pending
#if defined(NIMBLE_PEER_TRANSPORT)
		    || _client_connecting
#endif
		   ) return;
		if (!device.isAdvertisingService(_service_uuid)) return;

#if defined(NIMBLE_PEER_TRANSPORT)
		const BLEAddress& address = device.getAddress();
		if (address_blacklisted(address)) return;
		const bool initiates = (uint64_t)BLEDevice::getAddress() < (uint64_t)address;
		const int8_t rssi = device.getRSSI();
		// Too weak to reach. Discovering a peer and being able to connect to it
		// are different things, and dialling one we cannot reach costs ten
		// seconds of a radio that Wi-Fi and ESP-NOW are also using. See
		// BLE_PEER_CONNECT_MIN_RSSI.
		if (rssi < BLE_PEER_CONNECT_MIN_RSSI) return;
		// Let the whole burst run. Prefer any peer for which the deterministic
		// MAC rule makes us central, then the strongest signal within that role.
		// Connecting to the first packet seen made one unreachable advertiser
		// permanently starve every other Columba/RAD peer in the room.
		if (_candidate_valid &&
		    (_candidate_initiates || !initiates) &&
		    (_candidate_initiates != initiates || _candidate_rssi >= rssi)) return;
#else
		if (_candidate_valid) return;
#endif
		_candidate       = device.getAddress();
		_candidate_type  = device.getAddressType();
#if defined(NIMBLE_PEER_TRANSPORT)
		_candidate_rssi = rssi;
		_candidate_initiates = initiates;
#endif
		_candidate_valid = true;
	}

	// The decision half of onResult(), running on the main task where there is
	// stack to spare for string handling and logging.
	void consider_candidate() {
		if (!_candidate_valid) return;
#if defined(NIMBLE_PEER_TRANSPORT)
		BLEScan* scan = BLEDevice::getScan();
		if (scan != nullptr && scan->isScanning()) return;
#endif
		_candidate_valid = false;
		if (_client_connected || _connect_pending
#if defined(NIMBLE_PEER_TRANSPORT)
		    || _client_connecting || !retry_due()
#endif
		   ) return;

		const std::string peer = _candidate.toString();
		if (!should_connect(peer)) {
			// The peer has the lower address, so it initiates and we wait as a
			// peripheral. Logged once because a silent no-op here looks
			// identical to not having seen the device at all.
			if (!_waiting_logged) {
				_waiting_logged = true;
				printf("[blepeer] %s has the lower address; waiting as peripheral\n",
				       peer.c_str());
			}
			return;
		}
		_connect_address = _candidate;
		_connect_type    = _candidate_type;
		_connect_pending = true;
		printf("[blepeer] discovered %s (rssi=%d), we hold the lower address; will connect\n",
		       peer.c_str(),
#if defined(NIMBLE_PEER_TRANSPORT)
		       (int)_candidate_rssi);
#else
		       0);
#endif
	}

	// Called from the notify trampoline for the client role. Same reassembly as
	// the server path -- the bytes and the framing are identical.
	void client_data(const uint8_t* data, size_t length) { ingest(data, length); }

	void client_disconnected() {
		_client_connected = false;
		_remote_rx = nullptr;
		_inbound.clear();
		_expected = 0;
		printf("[blepeer] client link closed\n");
	}

#if defined(NIMBLE_PEER_TRANSPORT)
	void onConnect(NimBLEClient*) override {
		_client_connecting = false;
		_client_setup_pending = true;
	}
	void onConnectFail(NimBLEClient*, int reason) override {
		_client_connecting = false;
		_client_fail_reason = reason;
		_client_fail_pending = true;
	}
	void onDisconnect(NimBLEClient*, int reason) override {
		_client_connecting = false;
		_client_disconnect_reason = reason;
		_client_disconnect_pending = true;
	}
	void onConnect(NimBLEServer*, NimBLEConnInfo& conn_info) override {
		const uint16_t handle = conn_info.getConnHandle();
		if (_server_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
			_server_conn_handle = handle;
			_server_peer_address = conn_info.getAddress();
			_peer_identity.clear();
			_inbound.clear();
			_expected = 0;
			_server_connect_pending = true;
		} else if (_server_conn_handle != handle) {
			_duplicate_server_handle = handle;
			_duplicate_server_address = conn_info.getAddress();
			_duplicate_server_pending = true;
		}
	}
	void onDisconnect(NimBLEServer*, NimBLEConnInfo& conn_info, int reason) override {
		const uint16_t handle = conn_info.getConnHandle();
		if (handle != _server_conn_handle) {
			if (handle == _duplicate_server_handle) _duplicate_server_pending = false;
			return;
		}
		_server_disconnect_address = conn_info.getAddress();
		_server_disconnect_reason = reason;
		_server_conn_handle = BLE_HS_CONN_HANDLE_NONE;
		_peer_identity.clear();
		_inbound.clear();
		_expected = 0;
		_server_disconnect_pending = true;
		// NimBLE invokes this on its host task. Re-entering the advertiser from
		// that callback risks a host lock inversion, so let the Arduino loop do
		// the actual restart.
		_advertise_pending = true;
	}
#endif

protected:
	virtual void handle_incoming(const RNS::Bytes& data) override {
		// What Reticulum is actually being handed. A packet that crosses the
		// wire, reassembles, and is then ignored looks identical from the
		// counters to one that never arrived, so print the first bytes: a
		// Reticulum header is readable at a glance and tells us whether we are
		// delivering packets or rubble.
		_last_in_size = data.size();
		{
			size_t show = data.size() < 8 ? data.size() : 8;
			size_t at = 0;
			for (size_t i = 0; i < show; i++)
				at += snprintf(_last_in_hex + at, sizeof(_last_in_hex) - at,
				               "%02x", data.data()[i]);
			_last_in_hex[at] = 0;
		}
		try {
			InterfaceImpl::handle_incoming(data);
		}
		catch (const std::bad_alloc&) { ERROR("BLEPeerInterface::handle_incoming: out of memory"); }
		catch (std::exception& e)     { ERRORF("BLEPeerInterface::handle_incoming: %s", e.what()); }
	}

	// --- outbound: fragment and notify --------------------------------------
	virtual bool send_outgoing(const RNS::Bytes& data) override {
		bool success = true;
		try {
			// Either role can carry traffic: as a central our link lives on the
			// client, and the server then reports zero connections. Gating on
			// the server alone silently refused every outbound packet whenever
			// we were the side that initiated.
			const bool server_link = has_server_link();
			if (!_started || (!server_link && !_client_connected)) {
				return false;
			}
			const size_t usable  = ble_peer_usable_value_length(negotiated_mtu());
			const size_t payload = ble_peer_payload_size(usable);
			const size_t total   = (data.size() + payload - 1) / payload;
			if (total == 0 || total > 65535) return false;

			uint8_t frame[BLE_PEER_HEADER_SIZE + BLE_PEER_MAX_ATTR];
			for (size_t i = 0; i < total; i++) {
				// Fragment zero is START even when it is also the only one.
				//
				// BleConstants.kt declares FRAGMENT_TYPE_LONE = 0x00, but the
				// peer never emits it: measured on the wire, every
				// single-fragment packet arrives as 01 0000 0001 -- START with
				// total=1 -- and the LONE constant is unused anywhere in the
				// client. Its reassembler discards what it does not recognise,
				// so sending LONE made the entire outbound direction vanish
				// while inbound kept working perfectly.
				const uint8_t type = (i == 0) ? BLE_PEER_TYPE_START
				                  : (i == total - 1) ? BLE_PEER_TYPE_END
				                                     : BLE_PEER_TYPE_CONTINUE;
				const size_t offset = i * payload;
				const size_t chunk  = (data.size() - offset < payload)
				                    ? (data.size() - offset) : payload;
				ble_peer_write_header(frame, type, (uint16_t)i, (uint16_t)total);
				memcpy(frame + BLE_PEER_HEADER_SIZE, data.data() + offset, chunk);
				notify_raw(frame, BLE_PEER_HEADER_SIZE + chunk);
			}
			_last_out_size = data.size();
			_last_mtu = negotiated_mtu();
			_packets_out++;
			InterfaceImpl::handle_outgoing(data);
		}
		catch (const std::bad_alloc&) { ERROR("BLEPeerInterface::send_outgoing: out of memory"); success = false; }
		catch (std::exception& e)     { ERRORF("BLEPeerInterface::send_outgoing: %s", e.what()); success = false; }
		return success;
	}

private:
	// The MTU belongs to whichever link we are actually using. Asking the
	// server while connected as a central reports nothing useful, so we would
	// fragment to the 23-byte floor and waste most of a 512-byte MTU.
	uint16_t negotiated_mtu() {
		if (_client_connected && _client != nullptr) {
			const uint16_t mtu = _client->getMTU();
			return (mtu >= BLE_PEER_MIN_MTU) ? mtu : (uint16_t)BLE_PEER_MIN_MTU;
		}
		if (!has_server_link()) return BLE_PEER_MIN_MTU;
		uint16_t mtu = BLE_PEER_MIN_MTU;
#if defined(NIMBLE_PEER_TRANSPORT)
		mtu = _server->getPeerMTU(_server_conn_handle);
#else
		mtu = _server->getPeerMTU(_server->getConnId());
#endif
		return (mtu >= BLE_PEER_MIN_MTU) ? mtu : (uint16_t)BLE_PEER_MIN_MTU;
	}

	// One send path, two roles. As a central we write to the peer's RX
	// characteristic; as a peripheral we notify on our own TX. A node can hold
	// both at once, so try the client link first and fall back to the server.
	void notify_raw(const uint8_t* data, size_t length) {
		if (_client_connected && _remote_rx != nullptr) {
			// Write without response: the reference uses it for throughput, and
			// Reticulum retransmits what it must.
			_remote_rx->writeValue((uint8_t*)data, length, false);
			return;
		}
		if (_tx == nullptr) return;
		_tx->setValue((uint8_t*)data, length);
#if defined(NIMBLE_PEER_TRANSPORT)
		_tx->notify();
#else
		_tx->notify(true);
#endif
	}

	// Periodic short scans. Continuous scanning starves a board that is also
	// running a LoRa radio and a Reticulum stack, and the reference itself
	// scans in bursts with an idle backoff.
	void scan_tick() {
		if (_client_connected
#if defined(NIMBLE_PEER_TRANSPORT)
		    || _client_connecting || !retry_due()
#endif
		   ) return;
		const uint32_t now = millis();
		if (now - _last_scan < BLE_PEER_SCAN_INTERVAL_MS) return;
		_last_scan = now;
		BLEScan* scan = BLEDevice::getScan();
		if (scan == nullptr) return;
#if defined(NIMBLE_PEER_TRANSPORT)
		// Android peers use rotating private addresses. Never let a result from a
		// previous burst survive into the next connection attempt: dialling that
		// address can only run to BLE_HS_ETIMEOUT, even though the same peer is
		// already advertising under a new address.
		if (!scan->isScanning()) scan->clearResults();
		_candidate_valid = false;
		_candidate_rssi = INT8_MIN;
		_candidate_initiates = false;
		scan->setScanCallbacks(this, false);
#else
		scan->setAdvertisedDeviceCallbacks(this, false);
#endif
		scan->setActiveScan(true);
		scan->setInterval(100);
		scan->setWindow(80);
		// Non-blocking: results arrive in onResult().
#if defined(NIMBLE_PEER_TRANSPORT)
		if (!scan->start(BLE_PEER_SCAN_SECONDS * 1000, false, true)) {
			printf("[blepeer] scan start failed\n");
		}
#else
		scan->start(BLE_PEER_SCAN_SECONDS, nullptr, false);
#endif
	}

	void connect_to_peer() {
		BLEScan* scan = BLEDevice::getScan();
		if (scan != nullptr) {
			scan->stop();
#if defined(NIMBLE_PEER_TRANSPORT)
			// _connect_address owns its own copy; the scan result that supplied it
			// can be discarded before connecting.
			scan->clearResults();
#endif
		}

		if (_client == nullptr) {
			_client = BLEDevice::createClient();
#if defined(NIMBLE_PEER_TRANSPORT)
			if (_client != nullptr) {
				_client->setClientCallbacks(this, false);
				_client->setConnectRetries(0);
				_client->setConnectTimeout(BLE_PEER_CONNECT_TIMEOUT_MS);
			}
#endif
		}
		if (_client == nullptr) return;
#if defined(NIMBLE_PEER_TRANSPORT)
		printf("[blepeer] connecting asynchronously to %s (address type %u)\n",
		       _connect_address.toString().c_str(),
		       (unsigned)_connect_address.getType());
		_client_connecting = true;
		if (!_client->connect(_connect_address, true, true, true)) {
			_client_connecting = false;
			_client_fail_reason = _client->getLastError();
			_client_fail_pending = true;
		}
		return;
#else
		printf("[blepeer] connecting to %s\n", _connect_address.toString().c_str());
		if (!_client->connect(_connect_address, _connect_type)) {
			printf("[blepeer] connect failed\n");
			return;
		}
		if (!finish_client_connection()) _client->disconnect();
#endif
	}

	// Finish service discovery and the v2.2 identity handshake after the GAP
	// connection has completed. On NimBLE this is called by the Arduino loop,
	// never by the host callback; on Bluedroid it follows the blocking connect.
	bool finish_client_connection() {
		BLERemoteService* service = _client->getService(BLEUUID(BLE_PEER_SERVICE_UUID));
		if (service == nullptr) {
			printf("[blepeer] peer has no Reticulum service; disconnecting\n");
			return false;
		}
		_remote_rx = service->getCharacteristic(BLEUUID(BLE_PEER_RX_UUID));
		BLERemoteCharacteristic* remote_tx =
			service->getCharacteristic(BLEUUID(BLE_PEER_TX_UUID));
		BLERemoteCharacteristic* remote_identity =
			service->getCharacteristic(BLEUUID(BLE_PEER_IDENTITY_UUID));
		if (_remote_rx == nullptr || remote_tx == nullptr ||
		    remote_identity == nullptr || !remote_identity->canRead()) {
			printf("[blepeer] peer service is missing RX, TX or identity; disconnecting\n");
			_remote_rx = nullptr;
			return false;
		}

		// Columba's central sequence reads identity before subscribing and
		// writing its own identity. Mirror it, and reject incomplete peers rather
		// than publishing a connected-but-inert Reticulum interface.
		std::string identity_value = remote_identity->readValue();
		if (identity_value.size() != BLE_PEER_IDENTITY_SIZE) {
			printf("[blepeer] peer identity is %u bytes, expected %u; disconnecting\n",
			       (unsigned)identity_value.size(), (unsigned)BLE_PEER_IDENTITY_SIZE);
			_remote_rx = nullptr;
			return false;
		}
		_peer_identity = RNS::Bytes((const uint8_t*)identity_value.data(),
		                            identity_value.size());

		// Subscribing is what makes the link two-way. A client that connects and
		// does not subscribe receives nothing, which is indistinguishable from a
		// peer that never speaks.
#if defined(NIMBLE_PEER_TRANSPORT)
		if (!remote_tx->subscribe(true, &ble_peer_notify_trampoline, true)) {
			printf("[blepeer] notification subscription failed\n");
			_remote_rx = nullptr;
			return false;
		}
#else
		remote_tx->registerForNotify(&ble_peer_notify_trampoline, true, true);
#endif
		// Written to RX, never notified: identity travels central -> peripheral
		// on the RX characteristic only.
#if defined(NIMBLE_PEER_TRANSPORT)
		if (!_remote_rx->writeValue((uint8_t*)_identity_hash.data(),
		                            _identity_hash.size(), true)) {
			printf("[blepeer] identity handshake write failed; disconnecting\n");
			_remote_rx = nullptr;
			return false;
		}
#else
		_remote_rx->writeValue((uint8_t*)_identity_hash.data(),
		                       _identity_hash.size(), true);
#endif
		_client_connected = true;
		_inbound.clear();
		_expected = 0;
		printf("[blepeer] connected as central to %s, mtu=%u\n",
		       _connect_address.toString().c_str(), (unsigned)_client->getMTU());
		return true;
	}
	bool has_server_link() const {
		if (_server == nullptr) return false;
#if defined(NIMBLE_PEER_TRANSPORT)
		return _server_conn_handle != BLE_HS_CONN_HANDLE_NONE;
#else
		return _server->getConnectedCount() > 0;
#endif
	}

#if defined(NIMBLE_PEER_TRANSPORT)
	bool retry_due() const {
		return _next_connect_attempt == 0 ||
		       (int32_t)(millis() - _next_connect_attempt) >= 0;
	}

	bool address_blacklisted(const BLEAddress& address) const {
		const uint32_t now = millis();
		for (size_t i = 0; i < BLE_PEER_CONNECT_BLACKLIST_SIZE; ++i) {
			if (_failed_until[i] != 0 &&
			    (int32_t)(now - _failed_until[i]) < 0 &&
			    _failed_addresses[i] == address) return true;
		}
		return false;
	}

	void blacklist_failed_address(const BLEAddress& address) {
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

	void process_client_events() {
		if (_server_connect_pending) {
			_server_connect_pending = false;
			printf("[blepeer] inbound peripheral link %s connected (handle=%u, unpaired)\n",
			       _server_peer_address.toString().c_str(),
			       (unsigned)_server_conn_handle);
		}
		if (_server_disconnect_pending) {
			_server_disconnect_pending = false;
			printf("[blepeer] inbound peripheral link %s disconnected, reason=%d\n",
			       _server_disconnect_address.toString().c_str(),
			       _server_disconnect_reason);
		}
		if (_duplicate_server_pending && _server != nullptr) {
			_duplicate_server_pending = false;
			printf("[blepeer] rejecting duplicate inbound link %s (handle=%u)\n",
			       _duplicate_server_address.toString().c_str(),
			       (unsigned)_duplicate_server_handle);
			_server->disconnect(_duplicate_server_handle);
		}
		const bool server_connected = has_server_link();
		if (_client_fail_pending) {
			_client_fail_pending = false;
			_client_setup_pending = false;
			printf("[blepeer] central connect failed, reason=%d; advertising for %ums before retry\n",
			       _client_fail_reason, (unsigned)BLE_PEER_CONNECT_RETRY_MS);
			_remote_rx = nullptr;
			blacklist_failed_address(_connect_address);
			_candidate_valid = false;
			BLEScan* scan = BLEDevice::getScan();
			if (scan != nullptr && !scan->isScanning()) scan->clearResults();
			_next_connect_attempt = millis() + BLE_PEER_CONNECT_RETRY_MS;
			if (!server_connected) restart_advertising();
		}
		if (_client_disconnect_pending) {
			_client_disconnect_pending = false;
			_client_setup_pending = false;
			const bool completed_link = _client_connected;
			client_disconnected();
			printf("[blepeer] central link disconnected, reason=%d\n",
			       _client_disconnect_reason);
			if (!completed_link) blacklist_failed_address(_connect_address);
			_candidate_valid = false;
			BLEScan* scan = BLEDevice::getScan();
			if (scan != nullptr && !scan->isScanning()) scan->clearResults();
			_next_connect_attempt = millis() + BLE_PEER_CONNECT_RETRY_MS;
			if (!server_connected) restart_advertising();
		}
		if (_client_setup_pending) {
			_client_setup_pending = false;
			// If Columba completed the inbound direction during our outgoing GAP
			// attempt, keep that usable link and discard the duplicate central one.
			if (server_connected && _client != nullptr) {
				_client->disconnect();
			} else if ((_client == nullptr || !_client->isConnected() ||
			            !finish_client_connection()) && _client != nullptr) {
				_client->disconnect();
			}
		}
	}
#endif

	BLEServer*         _server   = nullptr;
	BLEService*        _service  = nullptr;
	BLECharacteristic* _rx       = nullptr;
	BLECharacteristic* _tx       = nullptr;
	BLECharacteristic* _identity = nullptr;

	RNS::Bytes _identity_hash;
	RNS::Bytes _peer_identity;
	RNS::Bytes _inbound;
	uint16_t   _expected = 0;
	uint16_t   _next_seq = 0;

	BLEClient*               _client        = nullptr;
	BLERemoteCharacteristic* _remote_rx     = nullptr;
	BLEAddress               _connect_address
#if defined(NIMBLE_PEER_TRANSPORT)
		= BLEAddress();
	uint8_t                  _connect_type = BLE_ADDR_PUBLIC;
#else
		= BLEAddress("00:00:00:00:00:00");
	esp_ble_addr_type_t      _connect_type = BLE_ADDR_TYPE_PUBLIC;
#endif
	bool     _connect_pending = false;
	bool     _client_connected = false;
#if defined(NIMBLE_PEER_TRANSPORT)
	volatile bool _client_connecting = false;
	volatile bool _client_setup_pending = false;
	volatile bool _client_fail_pending = false;
	volatile bool _client_disconnect_pending = false;
	volatile bool _server_connect_pending = false;
	volatile bool _server_disconnect_pending = false;
	volatile bool _duplicate_server_pending = false;
	volatile int  _client_fail_reason = 0;
	volatile int  _client_disconnect_reason = 0;
	volatile int  _server_disconnect_reason = 0;
	volatile uint16_t _server_conn_handle = BLE_HS_CONN_HANDLE_NONE;
	volatile uint16_t _duplicate_server_handle = BLE_HS_CONN_HANDLE_NONE;
	BLEAddress _server_peer_address;
	BLEAddress _server_disconnect_address;
	BLEAddress _duplicate_server_address;
	uint32_t _next_connect_attempt = 0;
	BLEAddress _failed_addresses[BLE_PEER_CONNECT_BLACKLIST_SIZE];
	uint32_t _failed_until[BLE_PEER_CONNECT_BLACKLIST_SIZE] = {0};
#endif
	bool     _waiting_logged   = false;
	uint32_t _last_scan        = 0;

	bool     _started       = false;
	bool     _begin_attempted = false;
	bool     _was_connected = false;
	volatile bool _advertise_pending = false;
	uint32_t _last_keepalive = 0;
	uint32_t _keepalives_in = 0;
	uint32_t _last_advertise = 0;
	uint32_t _last_in_size = 0;
	char     _last_in_hex[20] = {0};
	uint32_t _last_out_size = 0;
	uint32_t _last_mtu = 0;
	uint32_t _identity_writes = 0;
	volatile bool _identity_log_pending = false;
	char     _last_frag_hdr[12] = {0};
	uint32_t _frag_lone = 0;
	uint32_t _frag_start = 0;
	BLEUUID  _service_uuid = BLEUUID(BLE_PEER_SERVICE_UUID);
	BLEAddress _candidate
#if defined(NIMBLE_PEER_TRANSPORT)
		= BLEAddress();
	uint8_t _candidate_type = BLE_ADDR_PUBLIC;
#else
		= BLEAddress("00:00:00:00:00:00");
	esp_ble_addr_type_t _candidate_type = BLE_ADDR_TYPE_PUBLIC;
#endif
	volatile bool _candidate_valid = false;
#if defined(NIMBLE_PEER_TRANSPORT)
	volatile int8_t _candidate_rssi = INT8_MIN;
	volatile bool _candidate_initiates = false;
#endif
	std::vector<RNS::Bytes> _rx_queue;
	SemaphoreHandle_t _rx_lock = xSemaphoreCreateMutex();
	uint8_t  _reject_bytes[12] = {0};
	uint8_t  _reject_length = 0;
	uint16_t _reject_total = 0;
	volatile bool _reject_valid = false;
	uint32_t _last_reject_log = 0;
	uint32_t _packets_in  = 0;
	uint32_t _packets_out = 0;
	uint32_t _dropped     = 0;
};

inline void ble_peer_notify_trampoline(BLERemoteCharacteristic* characteristic,
                                       uint8_t* data, size_t length,
                                       bool is_notify) {
	(void)characteristic; (void)is_notify;
	if (ble_peer_active != nullptr) ble_peer_active->client_data(data, length);
}

#endif // HAS_BLE && MCU_ESP32
