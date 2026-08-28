// Copyright (C) 2024, Mark Qvist

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <WiFi.h>
#if defined(UDP_TRANSPORT)
#include <WiFiUdp.h>
#include <microReticulum/Bytes.h>
#endif

#if CONFIG_IDF_TARGET_ESP32
  #include "esp32/rom/rtc.h"
#elif CONFIG_IDF_TARGET_ESP32S2
  #include "esp32s2/rom/rtc.h"
#elif CONFIG_IDF_TARGET_ESP32C3
  #include "esp32c3/rom/rtc.h"
#elif CONFIG_IDF_TARGET_ESP32S3
  #include "esp32s3/rom/rtc.h"
#else 
  #error Target CONFIG_IDF_TARGET is not supported
#endif

#define WIFI_UPDATE_INTERVAL_MS 500
#define WR_SOCKET_TIMEOUT 6
// How long a *connected* client may go without sending readable bytes before
// the listener reclaims its slot.
//
// This was 6500 ms, which is incompatible with how Reticulum keeps a TCP
// interface alive. RNS sets SO_KEEPALIVE with TCP_KEEPIDLE=5s and lets the
// kernel prove liveness (TCPInterface.py, set_timeouts_linux); an idle
// Reticulum link deliberately sends no application data at all, sometimes for
// minutes. At 6.5 seconds this server therefore closed healthy links on a
// schedule, and the client reconnected immediately: Rev 1 was observed
// accepting 17 connections in 200 seconds from one Columba instance -- one
// every 11.8 seconds, indefinitely.
//
// Each of those cycles allocates and frees lwIP socket state, which is
// DMA-capable internal RAM and so never spills to PSRAM whatever
// PSRAM_MALLOC_THRESHOLD is set to. That is the shape of the internal-heap
// slide that takes this board off the air, and it is why the timeout matters
// far beyond a slot being held.
//
// Five minutes still reclaims a genuinely wedged client -- the case the
// original short timeout was reaching for -- while leaving normal quiet
// traffic alone. Peer death is detected long before that by TCP keepalive,
// which is what connected() reports on.
#define WR_READ_TIMEOUT_MS 300000
#define WR_RECONNECT_INTERVAL_MS 10000

uint32_t wifi_update_interval_ms = WIFI_UPDATE_INTERVAL_MS;
uint32_t last_wifi_update = 0;
uint32_t wr_last_connect_try = 0;
uint32_t wr_last_read = 0;

WiFiClient connection;
WiFiServer remote_listener(7633, 1);
// Address on which the legacy KISS listener was last started. In station mode
// setup reaches wifi_remote_start() before DHCP completes; starting the server
// there can leave it without a usable listening socket. Rebind exactly once
// when an address arrives, just as the UDP transport does below.
IPAddress kiss_bound_ip((uint32_t)0);
IPAddress ap_ip(10, 0, 0, 1);
IPAddress ap_nm(255, 255, 255, 0);
IPAddress wr_device_ip;
char wr_hostname[10];
wl_status_t wr_wifi_status = WL_IDLE_STATUS;
#if defined(UDP_TRANSPORT)
WiFiUDP udp;
// Address the UDP socket was last bound with; see wifi_update_status().
IPAddress udp_bound_ip((uint32_t)0);
// UDP packet counters. Announces leaving the board prove nothing about whether
// it can *hear* the mesh; these separate "not transmitting" from "not receiving".
volatile uint32_t udp_rx_count = 0;
volatile uint32_t udp_tx_count = 0;
RNS::Bytes udp_buffer;
#if defined(HAS_RNS)
extern RNS::Interface udp_interface;
#endif
#endif

uint8_t wifi_mode = WIFI_OFF;
bool wifi_init_ran = false;

// --- SoftAP fallback timing, provisionable at runtime ---
//
// The Config.h values are the defaults; these are what the code reads. Correct
// values depend on the building -- how slow the router is to boot, how long a
// resident may hold the node before it goes looking for an uplink again -- and
// a deployed node should not need a reflash to change them.
//
// Seconds on the wire, milliseconds in use: an operator setting a fallback
// delay does not think in milliseconds, and the provisioning schema should not
// make them.
uint32_t wifi_ap_fallback_ms = WIFI_AP_FALLBACK_MS;
uint32_t wifi_ap_retry_sta_ms = WIFI_AP_RETRY_STA_MS;
uint32_t wifi_ap_max_defer_ms = WIFI_AP_MAX_DEFER_MS;

// Mirrors WiFi.softAPgetStationNum() so provisioning can report it without
// pulling the WiFi headers into that translation unit. Updated wherever the
// fallback state machine already asks.
uint8_t wifi_ap_client_count = 0;

// --- SoftAP fallback state (see WIFI_AP_FALLBACK_MS in Config.h) ---
// True while we are serving our OWN access point because the configured
// station network could not be reached. Distinct from wifi_mode == WR_WIFI_AP,
// which means the operator asked for AP mode deliberately.
bool     wifi_ap_fallback_active = false;
uint32_t wifi_sta_failing_since  = 0;   // 0 = not currently failing
uint32_t wifi_ap_last_sta_retry  = 0;
char     wifi_ap_ssid[33]        = {0};
char     wifi_ap_psk[16]         = {0};
uint32_t wifi_ap_deferring_since = 0;   // 0 = not currently deferring
// The node's provisioned name, used to build the AP SSID. Defined in the
// sketch and populated during setup, after WiFi first comes up -- which is
// why the SSID is built at fallback time rather than at init.
extern char nomadnet_name[64];
bool wifi_initialized = false;

char wr_ssid[33];
char wr_psk[33];

extern uint16_t udp_port;
extern void host_disconnected();

//void wifi_dbg(String msg) { Serial.print("[WiFi] "); Serial.println(msg); }
void wifi_dbg(const char* msg) { printf("[WiFi] %s\n", msg); }

uint8_t wifi_remote_mode() { return wifi_mode; }

bool wifi_is_connected() { return (wr_wifi_status == WL_CONNECTED); }
bool wifi_host_is_connected() { if (connection) { return true; } else { return false; } }

extern void eeprom_update(int mapped_addr, uint8_t byte);

// Persist the station SSID to EEPROM.
//
// Lives here rather than in Provisioning.cpp because config_addr() and
// ADDR_CONF_SSID come from Config.h, whose macros collide with the Provisioning
// headers and cannot be included there. wifi_remote_init() reloads wr_ssid from
// EEPROM on every boot, so a provisioning setter that wrote only the variable
// accepted the value and then silently reverted on reboot.
void wifi_persist_ssid(const char* ssid) {
	const size_t length = ssid ? strnlen(ssid, 32) : 0;
	for (uint8_t i = 0; i < 32; i++) {
		eeprom_update(config_addr(ADDR_CONF_SSID + i),
		              i < length ? (uint8_t)ssid[i] : 0x00);
	}
	strncpy(wr_ssid, ssid ? ssid : "", sizeof(wr_ssid) - 1);
	wr_ssid[sizeof(wr_ssid) - 1] = 0x00;
}

// Build the fallback AP SSID: node name plus a MAC suffix.
//
// The name alone is not unique -- a building full of RAD-01s provisioned from
// the same template would all advertise the same SSID, and a resident could not
// tell which one is in their flat. Appending MAC bytes follows the Meshtastic
// convention users already recognise.
void wifi_build_ap_ssid() {
	uint8_t mac[6] = {0};
	WiFi.macAddress(mac);
	// Prefer the provisioned node name; fall back to the RNode device name if
	// provisioning has not run yet.
	const char* base = (nomadnet_name[0] != 0) ? nomadnet_name : bt_devname;
	char clean[24]; size_t c = 0;
	for (size_t i = 0; base[i] != 0 && c < sizeof(clean) - 1; i++) {
		char ch = base[i];
		// Keep SSIDs boring: alphanumerics and dashes survive, spaces and
		// brackets (the default name contains both) become dashes.
		if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		    (ch >= '0' && ch <= '9') || ch == '-') { clean[c++] = ch; }
		else if (ch == ' ' || ch == '_' || ch == '[' || ch == ']') { if (c && clean[c-1] != '-') clean[c++] = '-'; }
	}
	while (c && clean[c-1] == '-') c--;      // no trailing dash before the suffix
	clean[c] = 0;
	snprintf(wifi_ap_ssid, sizeof(wifi_ap_ssid), "%s-%02X%02X", clean, mac[4], mac[5]);

#ifdef WIFI_AP_PSK
	// Explicit override, including "" for an open network.
	snprintf(wifi_ap_psk, sizeof(wifi_ap_psk), "%s", WIFI_AP_PSK);
#else
	// Derive a per-node key from the MAC. A fleet-wide shared key means one
	// resident's password opens every neighbour's node; this gives each node
	// its own, deterministically, so SSID and key can be printed on the
	// enclosure label.
	//
	// The alphabet omits 0/O/1/l/I: these get read off a label and typed by
	// someone in a stairwell, and an ambiguous character there is a support
	// call. WPA2 requires at least 8 characters.
	static const char alpha[] = "abcdefghjkmnpqrstuvwxyz23456789";
	uint32_t h = 2166136261u;                       // FNV-1a over the MAC
	for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
	for (int i = 0; i < 8; i++) {
		wifi_ap_psk[i] = alpha[h % (sizeof(alpha) - 1)];
		h = h / (sizeof(alpha) - 1) + (h << 7) + 0x9E3779B9u;
	}
	wifi_ap_psk[8] = 0;
#endif
}

// Raise our own AP because the configured network is unreachable or absent.
void wifi_remote_start_ap_fallback() {
	wifi_build_ap_ssid();
	WiFi.mode(WIFI_AP);
	if (wifi_ap_psk[0] != 0) { WiFi.softAP(wifi_ap_ssid, wifi_ap_psk, wr_channel); }
	else                     { WiFi.softAP(wifi_ap_ssid, NULL, wr_channel); }
	delay(150);
	WiFi.softAPConfig(ap_ip, ap_ip, ap_nm);
	wifi_initialized = true;
	wifi_ap_fallback_active = true;
	wifi_ap_last_sta_retry = millis();
	wr_device_ip = WiFi.softAPIP();
	wr_wifi_status = WL_CONNECTED;
	wifi_ap_deferring_since = 0;
	// The key is NOT logged by default.
	//
	// serial_write() sends log output to whichever host transport is attached --
	// including the KISS console on port 7633, which has no authentication, so
	// anyone on the LAN could read it. Printing a credential there fails a
	// security review regardless of how narrow the practical risk is.
	//
	// Read it deliberately with -DWIFI_AP_LOG_PSK when labelling hardware.
	//
	// Be clear about what this does and does not achieve: the key is derived
	// from the MAC by an algorithm in open-source firmware, and the MAC is
	// broadcast in every frame -- so anyone who can see the AP can compute the
	// key. Hiding it from logs closes an incidental leak; it does not make the
	// key a secret. A genuinely secret PSK has to be provisioned per node, with
	// the distribution problem that implies. See docs/PrivateMesh.md.
	if (wifi_ap_psk[0] != 0) {
#ifdef WIFI_AP_LOG_PSK
		printf("[WiFi] serving fallback AP \"%s\" key \"%s\" at %s\n",
		       wifi_ap_ssid, wifi_ap_psk, wr_device_ip.toString().c_str());
#else
		printf("[WiFi] serving fallback AP \"%s\" (PSK set; build with "
		       "-DWIFI_AP_LOG_PSK to print it) at %s\n",
		       wifi_ap_ssid, wr_device_ip.toString().c_str());
#endif
	} else {
		printf("[WiFi] serving fallback AP \"%s\" (open) at %s\n",
		       wifi_ap_ssid, wr_device_ip.toString().c_str());
	}
}

void wifi_remote_start_ap() {
  WiFi.mode(WIFI_AP);
  if (wr_ssid[0] != 0x00) {
    if (wr_psk[0] != 0x00) { WiFi.softAP(wr_ssid, wr_psk, wr_channel); }
    else                   { WiFi.softAP(wr_ssid, NULL, wr_channel); }
  } else {
    if (wr_psk[0] != 0x00) { WiFi.softAP(bt_devname, wr_psk, wr_channel); }
    else                   { WiFi.softAP(bt_devname, NULL, wr_channel); }
  }
  delay(150);
  WiFi.softAPConfig(ap_ip, ap_ip, ap_nm);
  wifi_initialized = true;
}

void wifi_remote_start_sta() {
  WiFi.mode(WIFI_STA);

  uint8_t ip[4]; bool ip_ok = true;
  for (uint8_t i = 0; i < 4; i++) { ip[i]  = EEPROM.read(config_addr(ADDR_CONF_IP+i)); }
  if (ip[0]==0x00 && ip[1]==0x00 && ip[2]==0x00 && ip[3]==0x00) { ip_ok = false; }
  if (ip[0]==0xFF && ip[1]==0xFF && ip[2]==0xFF && ip[3]==0xFF) { ip_ok = false; }

  uint8_t nm[4]; bool nm_ok = true;
  for (uint8_t i = 0; i < 4; i++) { nm[i]  = EEPROM.read(config_addr(ADDR_CONF_NM+i)); }
  if (nm[0]==0x00 && nm[1]==0x00 && nm[2]==0x00 && nm[3]==0x00) { nm_ok = false; }
  if (nm[0]==0xFF && nm[1]==0xFF && nm[2]==0xFF && nm[3]==0xFF) { nm_ok = false; }

  if (ip_ok && nm_ok) {
    IPAddress sta_ip(ip[0], ip[1], ip[2], ip[3]);
    IPAddress sta_nm(nm[0], nm[1], nm[2], nm[3]);
    WiFi.config(sta_ip, sta_ip, sta_nm);
  }

  WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  WiFi.setPmf(true, false);   // capable, not required
#endif

  //WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
  //  printf("[WiFi] event=%d\n", event);
  //});

  delay(100);
  printf("[WiFi] ssid: %s\n", wr_ssid);
  //printf("[WiFi] psk: %s\n", wr_psk);
  if (wr_ssid[0] != 0x00) {
    if (wr_psk[0] != 0x00) { WiFi.begin(wr_ssid, wr_psk); }
    else                   { WiFi.begin(wr_ssid); }
  }
  
  delay(500);
  //delay(10000);
  wr_wifi_status = WiFi.status(); 
  printf("[WiFi] status: %d\n", wr_wifi_status);
  //printf("[WiFi] ip: %s\n", WiFi.localIP());
  wifi_initialized = true;
  wr_last_connect_try = millis();
}

void wifi_remote_stop() {
  remote_listener.end();
  kiss_bound_ip = IPAddress((uint32_t)0);
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_MODE_NULL);
  wifi_initialized = false;
}

void wifi_remote_start() {
  if      (wifi_mode == WR_WIFI_AP)  { wifi_remote_start_ap(); }
  else if (wifi_mode == WR_WIFI_STA) { wifi_remote_start_sta(); }
  else                               { wifi_remote_stop(); }

  if (wifi_initialized == true) {
    // TCP 7633 is unauthenticated KISS, not RNS. Keep it closed until
    // Provisioning has loaded the persisted secure-node policy.
    if (wireless_kiss_policy_ready && wireless_kiss_allowed) {
      IPAddress bind_ip = wifi_mode == WR_WIFI_AP ? WiFi.softAPIP() : WiFi.localIP();
      if (bind_ip != IPAddress((uint32_t)0)) {
        remote_listener.begin();
        remote_listener.setTimeout(WR_SOCKET_TIMEOUT);
        kiss_bound_ip = bind_ip;
      }
      wr_state = WR_STATE_ON;
    } else {
      remote_listener.end();
      kiss_bound_ip = IPAddress((uint32_t)0);
      wr_state = WR_STATE_OFF;
    }
#if defined(UDP_TRANSPORT)
    udp.begin(udp_port);
#endif
  } else {
    remote_listener.end();
    kiss_bound_ip = IPAddress((uint32_t)0);
    wr_state = WR_STATE_OFF;
#if defined(UDP_TRANSPORT)
    udp.stop();
#endif
  }
}

// Re-apply TCP 7633 policy after Provisioning loads or factory reset restores
// defaults. WiFi and the RNS interfaces on 4242 remain independent.
void wifi_remote_apply_kiss_policy() {
  if (!wifi_initialized) {
    kiss_bound_ip = IPAddress((uint32_t)0);
    wr_state = WR_STATE_OFF;
    return;
  }
  if (wireless_kiss_policy_ready && wireless_kiss_allowed) {
    IPAddress bind_ip = wifi_mode == WR_WIFI_AP ? WiFi.softAPIP() : WiFi.localIP();
    if (bind_ip != IPAddress((uint32_t)0)) {
      remote_listener.begin();
      remote_listener.setTimeout(WR_SOCKET_TIMEOUT);
      kiss_bound_ip = bind_ip;
    }
    wr_state = WR_STATE_ON;
  } else {
    if (connection) connection.stop();
    WiFiClient client = remote_listener.available();
    while (client) {
      client.stop();
      client = remote_listener.available();
    }
    remote_listener.end();
    kiss_bound_ip = IPAddress((uint32_t)0);
    wr_state = WR_STATE_OFF;
  }
}

void wifi_remote_init() {
  printf("Initializing WiFi...\n");
  memcpy(wr_hostname, bt_devname, 5);
  memcpy(wr_hostname+5, bt_devname+6, 4);
  wr_hostname[9] = 0x00;
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_MODE_NULL);
  WiFi.setHostname(wr_hostname);

  wr_ssid[32] = 0x00; wr_psk[32] = 0x00;
  for (uint8_t i = 0; i < 32; i++) { wr_ssid[i] = EEPROM.read(config_addr(ADDR_CONF_SSID+i)); if (wr_ssid[i] == 0xFF) { wr_ssid[i] = 0x00; } }
  for (uint8_t i = 0; i < 32; i++) { wr_psk[i]  = EEPROM.read(config_addr(ADDR_CONF_PSK+i));  if (wr_psk[i]  == 0xFF) { wr_psk[i]  = 0x00; } }
  wr_channel = EEPROM.read(eeprom_addr(ADDR_CONF_WCHN)); if (wr_channel < 1 || wr_channel > 14) { wr_channel = WR_CHANNEL_DEFAULT; }
  wifi_remote_start();
  wifi_init_ran = true;
}

void wifi_remote_close_all() {
  // wifi_dbg("Close all"); // TODO: Remove debug
  if (connection) { connection.stop(); }
  WiFiClient client = remote_listener.available();
  while (client) { client.stop(); client = remote_listener.available(); }
  wr_state = (wireless_kiss_policy_ready && wireless_kiss_allowed)
             ? WR_STATE_ON : WR_STATE_OFF;
}

void wifi_remote_check_active() {
  if (millis()-wr_last_read >= WR_READ_TIMEOUT_MS) {
    // wifi_dbg("Connection activity timed out"); // TODO: Remove debug
    if (connection && connection.connected()) {
      connection.stop();
      wifi_remote_close_all();
      host_disconnected();
    }
  }
}

bool wifi_remote_available() {
  if (!wireless_kiss_policy_ready || !wireless_kiss_allowed) return false;
  if (connection) {
    if (connection.connected()) {
      if (connection.available()) { wr_last_read = millis(); return true; }
      else                        { wifi_remote_check_active(); return false; }
    } else {
      // wifi_dbg("Client disconnected"); // TODO: Remove debug
      wifi_remote_close_all();
      return false;
    }
  } else {
    WiFiClient client = remote_listener.available();
    if (!client) { return false; }
    else {
      // Log who connects to the KISS listener. An idle client here is what
      // repeatedly tripped host_disconnected() (see handoff section 16).
      printf("[kiss-tcp] client connected from %s at %lums\n",
             client.remoteIP().toString().c_str(), (unsigned long)millis());
      connection = client;
      wr_state = WR_STATE_CONNECTED;
      wr_last_read = millis();
      if (connection.available()) { return true; }
      else                        { return false; }
    }
  }
}

uint8_t wifi_remote_read() {
  if (connection && connection.available()) { return connection.read(); }
  else {
    // wifi_dbg("Error: No data to read from TCP socket"); // TODO: Remove debug
    if (connection) { wifi_remote_close_all(); }
    return 0xC0;
  }
}

void wifi_remote_write(uint8_t byte) { if (connection) { connection.write(byte); } }

void wifi_update_status() {
  wr_wifi_status = WiFi.status();
  printf("[WiFi] status: %d\n", wr_wifi_status);
  if (wr_wifi_status == WL_CONNECTED) {
    wr_device_ip = WiFi.localIP();
    if (wifi_initialized && wireless_kiss_policy_ready && wireless_kiss_allowed &&
        wr_device_ip != IPAddress((uint32_t)0) && wr_device_ip != kiss_bound_ip) {
      // TCP servers started before DHCP may never acquire a working listener.
      // Recreate it when the station address becomes usable, and again after
      // an address change. Secure-node mode never enters this branch.
      if (connection) connection.stop();
      remote_listener.end();
      remote_listener.begin();
      remote_listener.setTimeout(WR_SOCKET_TIMEOUT);
      kiss_bound_ip = wr_device_ip;
      wr_state = WR_STATE_ON;
      printf("[WiFi] KISS TCP bound on %s:7633\n",
             wr_device_ip.toString().c_str());
    }
#if defined(UDP_TRANSPORT)
    // wifi_remote_start() opens the UDP socket immediately after WiFi.begin(),
    // which is before DHCP has assigned an address -- it binds with no local IP
    // and is never reopened, because wifi_remote_init() only re-runs when the
    // station drops. The interface then reports an empty address and never
    // joins the mesh. Rebind once, when an address actually arrives.
    if (wifi_initialized && wr_device_ip != IPAddress((uint32_t)0) && wr_device_ip != udp_bound_ip) {
      // Check begin()'s result and only latch the address on success. The first
      // attempt can land before the netif is ready to bind -- especially now that
      // boot reaches this point in ~4s -- and an unchecked failure left the
      // socket unbound forever: transmit still worked (beginPacket() needs no
      // bound socket) so the board announced normally while being completely
      // deaf, which is exactly how it presented.
      udp.stop();
      uint8_t ok = udp.begin(udp_port);
      if (ok) {
        udp_bound_ip = wr_device_ip;   // latch only on success, so failures retry
        printf("[WiFi] udp bound on %s:%u\n", wr_device_ip.toString().c_str(), (unsigned)udp_port);
      } else {
        printf("[WiFi] udp bind FAILED on %s:%u, will retry\n", wr_device_ip.toString().c_str(), (unsigned)udp_port);
      }
    }
#endif
    //printf("[WiFi] ip: %s\n", WiFi.localIP());
  }
  if (wifi_mode == WR_WIFI_AP && wifi_initialized) { wr_device_ip = WiFi.softAPIP(); wr_wifi_status = WL_CONNECTED; }
  if (wifi_init_ran && wifi_mode == WR_WIFI_STA && !wifi_ap_fallback_active &&
      wr_wifi_status != WL_CONNECTED) {
    if (millis()-wr_last_connect_try >= WR_RECONNECT_INTERVAL_MS) { wifi_remote_init(); }

#if WIFI_AP_FALLBACK_MS > 0
    // A node whose network is gone is useless to the people around it. After
    // trying long enough that this is not merely a router rebooting, raise our
    // own AP so residents have something to join.
    //
    // A node with no SSID at all -- never provisioned, or flashed blank -- is
    // treated as immediately unreachable rather than waiting out the timer for
    // a network that does not exist.
    bool never_configured = (wr_ssid[0] == 0x00);
    if (wifi_sta_failing_since == 0) { wifi_sta_failing_since = millis(); }
    if (never_configured || (millis() - wifi_sta_failing_since >= wifi_ap_fallback_ms)) {
      printf("[WiFi] %s -- falling back to own AP\n",
             never_configured ? "no station network configured"
                              : "station network unreachable");
      wifi_remote_start_ap_fallback();
    }
#endif
  } else if (wr_wifi_status == WL_CONNECTED && !wifi_ap_fallback_active) {
    wifi_sta_failing_since = 0;      // connected: reset the fallback timer
  }

#if WIFI_AP_FALLBACK_MS > 0
  // Keep the client-count mirror truthful whenever the fallback is up, and
  // clear it the moment it is not.
  //
  // Updating it only inside the retry window was wrong three ways: it went stale
  // for a whole retry interval (ten minutes by default), it kept its last value
  // for ever once the node rejoined the station network, and -- worst -- it
  // never updated at all on a node with no configured SSID, because that block
  // is gated on `wr_ssid` being set. A never-provisioned node is exactly the one
  // that goes straight to AP mode and stays there, so the operator asking "is
  // anyone actually connected to this thing?" would always have been told zero.
  if (wifi_ap_fallback_active) {
    wifi_ap_client_count = WiFi.softAPgetStationNum();
  } else if (wifi_ap_client_count != 0) {
    wifi_ap_client_count = 0;
  }

  // While serving the fallback AP, look for the configured network again --
  // but ONLY when nobody is associated. Dropping a resident mid-message to go
  // chase an uplink is the wrong trade; the uplink can wait until they leave.
  if (wifi_ap_fallback_active && wr_ssid[0] != 0x00 &&
      millis() - wifi_ap_last_sta_retry >= wifi_ap_retry_sta_ms) {
    wifi_ap_last_sta_retry = millis();
    const uint8_t stations = wifi_ap_client_count;
    if (stations > 0 && wifi_ap_deferring_since == 0) { wifi_ap_deferring_since = millis(); }
    bool defer_expired = (wifi_ap_deferring_since != 0 &&
                          millis() - wifi_ap_deferring_since >= wifi_ap_max_defer_ms);
    if (stations > 0 && !defer_expired) {
      // Someone is using us; the uplink can wait.
      printf("[WiFi] fallback AP has %u client(s) -- deferring station retry\n",
             (unsigned)stations);
    } else if (stations > 0 && defer_expired) {
      // ...but not for ever. In a building with continuous occupancy an
      // unbounded defer means never rejoining a router that came back hours
      // ago, so past the ceiling we retry regardless.
      printf("[WiFi] fallback AP deferred %lums with %u client(s) -- retrying anyway\n",
             (unsigned long)(millis() - wifi_ap_deferring_since), (unsigned)stations);
      wifi_ap_fallback_active = false;
      wifi_sta_failing_since = 0;
      wifi_ap_deferring_since = 0;
      wifi_remote_init();
    } else {
      wifi_ap_deferring_since = 0;
      printf("[WiFi] fallback AP idle -- retrying station network \"%s\"\n", wr_ssid);
      wifi_ap_fallback_active = false;
      wifi_sta_failing_since = 0;
      wifi_remote_init();
    }
  }
#endif
}

void update_wifi() {
#if defined(UDP_TRANSPORT)
  if (wifi_initialized) {
    int packet_len = udp.parsePacket();
    if (packet_len > 0) {
      udp_rx_count++;
      if (packet_len > UDP_RX_CAPACITY) {
        // Reading into a smaller buffer returns a truncated prefix. Drain and
        // reject the whole datagram so Transport never sees partial input.
        uint8_t discard[64];
        while (udp.available() > 0) udp.read(discard, sizeof(discard));
        WARNINGF("Dropped oversized UDP datagram (%d > %u bytes)",
                 packet_len, (unsigned)UDP_RX_CAPACITY);
      } else {
        int len = udp.read(udp_buffer.writable(UDP_RX_CAPACITY), UDP_RX_CAPACITY);
        if (len > 0 && len == packet_len) {
          udp_buffer.resize((size_t)len);
#if defined(HAS_RNS)
          if (udp_interface) udp_interface.handle_incoming(udp_buffer);
#endif
        } else if (len >= 0) {
          WARNINGF("Dropped incomplete UDP datagram (%d of %d bytes)",
                   len, packet_len);
        }
      }
    }
  }
#endif
  if (millis()-last_wifi_update >= wifi_update_interval_ms) {
    wifi_update_status();
    last_wifi_update = millis();
  }
}
