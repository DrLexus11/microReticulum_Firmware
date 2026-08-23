// RNS TCP server interface.
//
// Lets Reticulum clients attach to this node over IP -- in practice, residents'
// phones joining the node's SoftAP when building infrastructure is gone, or any
// host on the LAN when it is not.
//
// This is the resident attachment path for QuakeMesh. BLE cannot serve that role
// today: it is one client at a time and its MITM pairing needs a random passkey
// that is only readable on a display these boards do not have (see
// docs/MeshResilience.md). TCP needs no pairing, no passkey, and serves a
// household at once.
//
// Why it matters that each client is a separate Reticulum peer: every phone
// carries its own identity, its own destinations and its own LXMF address, with
// end-to-end encryption this node cannot read. The node is pure transport. That
// is the whole difference from a shared-key channel where a household has to
// pretend to be one user.
//
// Framing is HDLC, matching RNS's TCPInterface -- NOT the KISS framing used by
// the console on port 7633. They are different protocols on different ports and
// must not be confused.
//
// One interface object multiplexes all clients, rather than spawning an
// interface per connection. That models the AP as a shared segment (which is
// what it physically is): clients reach each other directly as well as reaching
// the LoRa side, and RNS deduplicates by packet hash, so echoing an inbound
// packet back out to the other clients is correct rather than a loop.

#pragma once

#include <microReticulum.h>

#include <WiFi.h>
#include <sys/socket.h>
#include <errno.h>

// Standard RNS TCP port. Distinct from the KISS console on 7633, and distinct
// from the UDP interface despite sharing the number -- different protocol.
#ifndef TCP_SERVER_PORT
  #define TCP_SERVER_PORT 4242
#endif

// Concurrent clients. Five covers an average EU household (4-5 people, one
// identity each) with a little headroom.
//
// Do not raise this casually: Arduino's lwIP defaults to about 10 sockets, and
// this node already spends some on the UDP interface and the KISS console. A
// listener plus five clients lands near nine. Going higher needs
// CONFIG_LWIP_MAX_SOCKETS raised to match, and the per-connection lwIP buffers
// come out of internal heap, not PSRAM.
#ifndef TCP_SERVER_MAX_CLIENTS
  #define TCP_SERVER_MAX_CLIENTS 5
#endif

// Per-client receive buffer size; matches the interface HW MTU.
#define TCPI_RX_BUFLEN 1064
// Worst case for an escaped frame: every payload byte doubles, plus two flags.
#define TCPI_TX_BUFLEN (2 * TCPI_RX_BUFLEN + 2)

// HDLC framing, as used by RNS TCPInterface.
#define TCPI_HDLC_FLAG      0x7E
#define TCPI_HDLC_ESC       0x7D
#define TCPI_HDLC_ESC_MASK  0x20

extern bool wifi_initialized;

class TCPServerInterface : public RNS::InterfaceImpl {

public:
	TCPServerInterface(const char* name) : RNS::InterfaceImpl(name), _server(TCP_SERVER_PORT) {
		_IN  = true;
		_OUT = true;
		_HW_MTU = 1064;
		for (int i = 0; i < TCP_SERVER_MAX_CLIENTS; i++) {
			_rx[i] = (uint8_t*)malloc(TCPI_RX_BUFLEN);
			reset_rx(i);
		}
		_tx = (uint8_t*)malloc(TCPI_TX_BUFLEN);
	}
	TCPServerInterface() : TCPServerInterface("TCPServerInterface") {}
	virtual ~TCPServerInterface() {
		for (int i = 0; i < TCP_SERVER_MAX_CLIENTS; i++) {
			if (_rx[i]) { free(_rx[i]); _rx[i] = nullptr; }
		}
		if (_tx) { free(_tx); _tx = nullptr; }
		_name = "deleted";
	}

	// Overrides InterfaceImpl::start(). Returns success, per the base class.
	virtual bool start() override {
		if (_started) return true;
		_server.begin();
		// WiFiServer::begin() returns void and gives up SILENTLY if the socket,
		// bind or listen call fails -- it simply returns with _listening false.
		// Reporting success here regardless would produce the worst possible
		// symptom: a log line claiming the interface is listening while it
		// accepts nobody, which is an hour of debugging in the wrong place.
		// operator bool() exposes _listening, so check it.
		if (!_server) {
			printf("[tcpi] ERROR: could not bind port %u -- interface is up but "
			       "will accept no clients\n", (unsigned)TCP_SERVER_PORT);
			return false;
		}
		_server.setNoDelay(true);   // packets are small and latency-sensitive
		_started = true;
		printf("[tcpi] listening on port %u (max %d clients)\n",
		       (unsigned)TCP_SERVER_PORT, TCP_SERVER_MAX_CLIENTS);
		return true;
	}

	virtual void stop() override {
		if (!_started) return;
		for (int i = 0; i < TCP_SERVER_MAX_CLIENTS; i++) {
			if (_clients[i]) { _clients[i].stop(); }
			reset_rx(i);
		}
		_server.end();
		_started = false;
	}

	int client_count() {
		int n = 0;
		for (int i = 0; i < TCP_SERVER_MAX_CLIENTS; i++) {
			if (peer_alive(i)) n++;
		}
		return n;
	}

	// Accept new clients and drain readable ones. Overrides
	// InterfaceImpl::loop(), which Reticulum calls for every registered
	// interface on each pass -- so this needs no wiring in the main sketch.
	virtual void loop() override {
		// Self-starting: the listener can only bind once WiFi is up, and WiFi
		// comes up (or returns after a reconnect) long after interfaces are
		// constructed. Binding here rather than at construction avoids ordering
		// assumptions about which happens first.
		if (!wifi_initialized) return;
		// Retry a failed bind on an interval rather than every pass: this runs
		// thousands of times a second, and a permanently unbindable port would
		// otherwise bury the log.
		if (!_started) {
			if (millis() - _last_start_try < 5000) return;
			_last_start_try = millis();
			if (!start()) return;
		}
		#ifdef TCPI_DEBUG
		{
			static uint32_t calls = 0, last = 0;
			calls++;
			if (millis() - last > 10000) {
				last = millis();
				printf("[tcpi] dbg loop=%lu started=%d hasClient=%d\n",
				       (unsigned long)calls, (int)_started, (int)_server.hasClient());
				for (int i = 0; i < TCP_SERVER_MAX_CLIENTS; i++) {
					printf("[tcpi]   slot%d bool=%d fd=%d alive=%d conn=%d\n",
					       i, (int)(bool)_clients[i], _clients[i] ? _clients[i].fd() : -1,
					       (int)peer_alive(i), (int)_clients[i].connected());
				}
			}
		}
		#endif
		accept_new();
		for (int i = 0; i < TCP_SERVER_MAX_CLIENTS; i++) {
			if (!_clients[i]) continue;
			if (!peer_alive(i)) { drop(i, "disconnected"); continue; }
			// Read in chunks. read() one byte at a time is a syscall per byte --
			// the receive-side twin of the write-side bug that made this
			// interface slower than LoRa.
			int avail = _clients[i].available();
			while (avail > 0) {
				uint8_t chunk[256];
				int want = avail > (int)sizeof(chunk) ? (int)sizeof(chunk) : avail;
				int got = _clients[i].read(chunk, want);
				if (got <= 0) break;
				for (int n = 0; n < got; n++) { feed(i, chunk[n]); }
				avail -= got;
			}
		}
	}

protected:
	virtual void handle_incoming(const RNS::Bytes& data) {
		try {
			InterfaceImpl::handle_incoming(data);
		}
		catch (const std::bad_alloc&) {
			ERROR("TCPServerInterface::handle_incoming: bad_alloc - out of memory");
		}
		catch (std::exception& e) {
			ERRORF("TCPServerInterface::handle_incoming: %s", e.what());
		}
	}

	virtual bool send_outgoing(const RNS::Bytes& data) {
		bool success = true;
		try {
			if (_started && wifi_initialized) {
				for (int i = 0; i < TCP_SERVER_MAX_CLIENTS; i++) {
					if (!peer_alive(i)) continue;
					if (!write_framed(i, data)) {
						// A client that cannot take the frame is dropped rather
						// than retried: a half-written frame would desynchronise
						// its HDLC stream, and every later packet on that
						// connection would be garbage.
						drop(i, "write failed");
						success = false;
					}
				}
			}
			InterfaceImpl::handle_outgoing(data);
		}
		catch (const std::bad_alloc&) {
			ERROR("TCPServerInterface::send_outgoing: bad_alloc - out of memory");
			success = false;
		}
		catch (std::exception& e) {
			ERRORF("TCPServerInterface::send_outgoing: %s", e.what());
			success = false;
		}
		return success;
	}

private:
	// Is this slot's peer still there?
	//
	// WiFiClient::connected() probes with a ZERO-length recv(), which returns 0
	// whether the peer closed or simply sent nothing -- so a cleanly
	// disconnected client keeps reporting connected and holds its slot for
	// ever. That was not theoretical: during bring-up a handful of test
	// connections filled every slot with dead clients, and the interface then
	// silently accepted nobody.
	//
	// Peeking one byte is unambiguous: 0 means the peer performed an orderly
	// shutdown, EWOULDBLOCK means alive but idle.
	bool peer_alive(int i) {
		if (!_clients[i]) return false;
		int f = _clients[i].fd();
		if (f < 0) return false;
		uint8_t b;
		int r = ::recv(f, &b, 1, MSG_DONTWAIT | MSG_PEEK);
		if (r > 0) return true;
		if (r == 0) return false;
		return (errno == EWOULDBLOCK || errno == EAGAIN);
	}

	void reset_rx(int i) { _rx_len[i] = 0; _in_frame[i] = false; _escape[i] = false; }

	void drop(int i, const char* why) {
		// Stop first, then count. Subtracting one from client_count() was wrong:
		// this is usually called *because* peer_alive(i) already returned false,
		// so the slot was not in the count to begin with -- the log undercounted,
		// and with a single client it printed -1.
		_clients[i].stop();
		reset_rx(i);
		printf("[tcpi] client %d %s (now %d connected)\n", i, why, client_count());
	}

	void accept_new() {
		if (!_server.hasClient()) return;
		for (int i = 0; i < TCP_SERVER_MAX_CLIENTS; i++) {
			if (peer_alive(i)) continue;
			if (_clients[i]) { _clients[i].stop(); }
			_clients[i] = _server.accept();
			reset_rx(i);
			printf("[tcpi] client %d connected from %s (now %d)\n",
			       i, _clients[i].remoteIP().toString().c_str(), client_count());
			return;
		}
		// Full. Reject explicitly rather than leaving the connection hanging in
		// the accept backlog, where the client would see a connected socket that
		// never carries traffic.
		WiFiClient reject = _server.accept();
		if (reject) {
			printf("[tcpi] refused client from %s: all %d slots in use\n",
			       reject.remoteIP().toString().c_str(), TCP_SERVER_MAX_CLIENTS);
			reject.stop();
		}
	}

	// Feed one received byte through the HDLC state machine.
	void feed(int i, uint8_t b) {
		if (b == TCPI_HDLC_FLAG) {
			if (_in_frame[i] && _rx_len[i] > 0) {
				RNS::Bytes data(_rx[i], _rx_len[i]);
				handle_incoming(data);
			}
			// A FLAG both ends the current frame and opens the next, so an
			// interrupted sender resynchronises on its next flag rather than
			// merging two frames.
			_in_frame[i] = true; _rx_len[i] = 0; _escape[i] = false;
			return;
		}
		if (!_in_frame[i]) return;          // junk before the first flag
		if (b == TCPI_HDLC_ESC) { _escape[i] = true; return; }
		if (_escape[i]) { b ^= TCPI_HDLC_ESC_MASK; _escape[i] = false; }
		if (!_rx[i]) return;
		if (_rx_len[i] < TCPI_RX_BUFLEN) {
			_rx[i][_rx_len[i]++] = b;
		} else {
			// Oversized frame: drop it and wait for the next flag. Keeping a
			// partial frame would hand RNS a truncated packet.
			_in_frame[i] = false; _rx_len[i] = 0; _escape[i] = false;
		}
	}

	// Build the whole escaped frame, then write it ONCE.
	//
	// The first version wrote a byte at a time, which combined with
	// setNoDelay(true) -- Nagle disabled -- turned every byte into its own TCP
	// segment: a 500-byte packet left as ~500 packets, each with 40 bytes of
	// IP/TCP header and a full WiFi frame exchange behind it. Over a link that
	// should be orders of magnitude faster than LoRa, page loads came out
	// *slower* than LoRa. One write per frame is the whole fix; NoDelay is
	// correct once frames are whole, since it avoids Nagle's 40ms wait on the
	// small packets RNS actually sends.
	bool write_framed(int i, const RNS::Bytes& data) {
		if (!_tx) return false;
		const uint8_t* p = data.data();
		size_t len = 0;
		_tx[len++] = TCPI_HDLC_FLAG;
		for (size_t n = 0; n < data.size(); n++) {
			uint8_t b = p[n];
			if (b == TCPI_HDLC_FLAG || b == TCPI_HDLC_ESC) {
				_tx[len++] = TCPI_HDLC_ESC;
				_tx[len++] = (uint8_t)(b ^ TCPI_HDLC_ESC_MASK);
			} else {
				_tx[len++] = b;
			}
		}
		_tx[len++] = TCPI_HDLC_FLAG;
		return _clients[i].write(_tx, len) == len;
	}

	WiFiServer _server;
	WiFiClient _clients[TCP_SERVER_MAX_CLIENTS];
	// Heap-allocated rather than inline members, and this matters on ESP32.
	//
	// PSRAM_MALLOC_THRESHOLD spills any allocation over ~1 KB into PSRAM. With
	// the receive buffers inline this object was ~5.5 KB, so `new
	// TCPServerInterface()` put the WHOLE object -- WiFiServer and WiFiClient
	// members included -- into PSRAM. The listener then bound (a client could
	// complete a TCP handshake) but hasClient() never reported it, so nothing
	// was ever accepted and clients sat connected receiving nothing.
	//
	// Keeping the object small leaves the socket objects in internal RAM where
	// the network stack needs them; the plain data buffers are fine in PSRAM.
	uint8_t*   _rx[TCP_SERVER_MAX_CLIENTS] = { nullptr };
	uint8_t*   _tx = nullptr;   // shared: send_outgoing() is not reentrant
	size_t     _rx_len[TCP_SERVER_MAX_CLIENTS];
	bool       _in_frame[TCP_SERVER_MAX_CLIENTS];
	bool       _escape[TCP_SERVER_MAX_CLIENTS];
	bool       _started = false;
	uint32_t   _last_start_try = 0;
};
