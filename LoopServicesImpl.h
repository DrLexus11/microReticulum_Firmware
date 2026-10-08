// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

// The adapters behind LoopServices.h. Included once, from the end of
// RNode_Firmware.ino, after the functions they call: heap_watch(),
// radio_rx_watchdog() and the rest are file-static there, and so is the state
// their health is measured from.
//
// This is the wrapping step, not the rewrite. Each poll() calls the function
// loop() used to call, unchanged, in the same order; what is new is that each
// is now timed by the runner, carries its own health, and writes its own part
// of the board's telemetry report. Moving the bodies and their state into the
// services is R5 (the application layer), one service at a time.
//
// Health comes from something measured, never from a flag the service set
// (IService.h). A service with nothing to measure yet reports Unmeasured rather
// than claiming to be healthy: the runner's timing still covers it.
//
// Budgets. The default is 20 ms. A service whose slow path is one announce --
// one Ed25519 signature and a packet, measured at 230-318 ms on Rev 2 --
// declares ANNOUNCE_BUDGET_MS instead, so an overrun there means more than the
// signature it cannot avoid. reticulum.loop keeps the default on purpose: one
// call processes however many inbound packets are waiting, so it is not bounded
// at all today, and its overruns are the measurement of that. Bounding it is
// library work, not this file's.

#include "LoopServices.h"
#include "LoopPhase.h"
#include "TelemetryUplink.h"
#include "TelemetryCodec.h"
#include "TelemetryDetailCodec.h"
#include "TelemetryNeighbours.h"
#if defined(LXMF_PROPAGATION_NODE)
#include <new>
#include "LXMFCompose.h"
#include "TelemetryBatchCodec.h"
#include "TelemetryKept.h"
#endif

// Below these, the heap is short of what the mesh stack needs to keep working.
// heap8/largest8, not "internal": see the [mem] line in heap_watch().
#ifndef SERVICE_HEAP8_LOW
#define SERVICE_HEAP8_LOW      (24u * 1024u)
#endif
#ifndef SERVICE_LARGEST8_LOW
#define SERVICE_LARGEST8_LOW   (8u * 1024u)
#endif
// An exception out of reticulum.loop() within this window marks it degraded.
#ifndef ANNOUNCE_BUDGET_MS
#define ANNOUNCE_BUDGET_MS 500u
#endif
#ifndef SERVICE_RNS_ERROR_WINDOW_MS
#define SERVICE_RNS_ERROR_WINDOW_MS 60000u
#endif

// A service that is one phase of the old loop. The phase is what the TASK_WDT
// breadcrumb records, and its name is the service's name, so a watchdog report
// and the service table use the same words.
class LoopService : public IService {
public:
  explicit LoopService(uint8_t phase) : _phase(phase) {}
  uint8_t phase() const { return _phase; }
  const char* name() const override { return loop_phase_name(_phase); }
  bool init(const AppContext&) override { return true; }
  bool start() override { return true; }
  Health health() const override { return Health{ServiceState::Unmeasured, ""}; }
  void telemetry(NodeTelemetry&) const override {}
  void stop() override {}
private:
  const uint8_t _phase;
};

#if defined(ESP32) && defined(HAS_RNS)
class HeapService : public LoopService {
public:
  HeapService() : LoopService(LOOP_PHASE_HEAP) {}
  void poll(uint32_t) override { heap_watch(); }
  Health health() const override {
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL) < SERVICE_HEAP8_LOW)
      return Health{ServiceState::Degraded, "heap8 low"};
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL) < SERVICE_LARGEST8_LOW)
      return Health{ServiceState::Degraded, "largest8 low"};
    return Health{ServiceState::Healthy, ""};
  }
  void telemetry(NodeTelemetry& report) const override {
    report.heap_bytes = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    report.largest_bytes = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
      report.psram_known = true;
      report.psram_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    }
  }
};
#endif

#if defined(HAS_RNS) && defined(URTN_STATS_PAGES)
class NomadAnnounceService : public LoopService {
public:
  NomadAnnounceService() : LoopService(LOOP_PHASE_NOMAD_ANN) {}
  void poll(uint32_t) override { nomadnet_announce_watch(); }
  uint32_t budget_ms() const override { return ANNOUNCE_BUDGET_MS; }
  Health health() const override {
    if (!nomadnet_enabled) return Health{ServiceState::Disabled, "nomadnet off"};
    return LoopService::health();
  }
};
#endif

#if defined(HAS_RNS) && defined(RRC_HUB)
class RrcHubService : public LoopService {
public:
  RrcHubService() : LoopService(LOOP_PHASE_RRC) {}
  void poll(uint32_t) override { rrc_hub_loop(); }
  uint32_t budget_ms() const override { return ANNOUNCE_BUDGET_MS; }
};
#endif

#if defined(BLE_PEER_TRANSPORT)
class BlePeerService : public LoopService {
public:
  BlePeerService() : LoopService(LOOP_PHASE_BLE_PEER) {}
  void poll(uint32_t) override {
    // Started lazily rather than at init: the GATT server does not exist until
    // Bluetooth has come up, and the transport identity is not loaded until
    // Reticulum has. Waiting for both here avoids ordering assumptions that
    // would fail silently.
  #if defined(NIMBLE_PEER_TRANSPORT)
    if (ble_peer_impl != nullptr && !ble_peer_impl->started() &&
        RNS::Transport::identity()) {
      ble_peer_impl->begin(RNS::Transport::identity().hash());
    }
  #else
    if (ble_peer_impl != nullptr && !ble_peer_impl->started() &&
        bt_state != BT_STATE_OFF && bt_state != BT_STATE_NA &&
        SerialBT.ble_server != nullptr && RNS::Transport::identity()) {
      ble_peer_impl->begin(SerialBT.ble_server, RNS::Transport::identity().hash());
    }
  #endif
    if (ble_peer_impl != nullptr) ble_peer_impl->loop();
  }
  Health health() const override {
    if (ble_peer_impl == nullptr) return Health{ServiceState::Failed, "no interface"};
    if (!ble_peer_impl->started()) return Health{ServiceState::Starting, "not advertising"};
  #if defined(NIMBLE_PEER_TRANSPORT)
    // No peer is not a fault -- nobody may be in range -- but it is said.
    if (ble_peer_impl->connected_peers() == 0) return Health{ServiceState::Healthy, "no peers"};
    return Health{ServiceState::Healthy, ""};
  #else
    return LoopService::health();
  #endif
  }
  void telemetry(NodeTelemetry& report) const override {
    report.if_present |= TELEMETRY_IF_BLE;
    if (ble_peer_impl != nullptr && ble_peer_impl->started()) report.if_up |= TELEMETRY_IF_BLE;
  #if defined(NIMBLE_PEER_TRANSPORT)
    if (ble_peer_impl != nullptr) report.ble_peers = ble_peer_impl->connected_peers();
  #endif
  }
};
#endif

#if defined(HAS_RNS) && defined(LORA_TRANSPORT)
// A build can carry LoRa transport on a board with no radio (the OZD ESP-NOW
// fixture): startRadio() treats that as deliberate, and so do these.
#if defined(NO_LORA_HARDWARE)
static constexpr bool loop_services_lora_absent = true;
#else
static constexpr bool loop_services_lora_absent = false;
#endif

class RadioRxWatchService : public LoopService {
public:
  RadioRxWatchService() : LoopService(LOOP_PHASE_RADIO_WD) {}
  void poll(uint32_t) override { radio_rx_watchdog(); }
  // Silence for half the watchdog period is reported before the watchdog acts
  // on the whole of it. A quiet channel and a deaf receiver look the same from
  // here (see radio_rx_watchdog()); the reason says which was measured.
  Health health() const override {
    if (loop_services_lora_absent) return Health{ServiceState::Disabled, "no LoRa hardware"};
    if (!radio_online) return Health{ServiceState::Degraded, "radio offline"};
  #if RADIO_RX_WATCHDOG_MS > 0
    if (!rx_tracking) return Health{ServiceState::Starting, "no rx yet"};
    const uint32_t quiet = millis() - rx_last_change_ms;
    if (quiet >= RADIO_RX_WATCHDOG_MS / 2) {
      snprintf(_reason, sizeof(_reason), "no rx %lus", (unsigned long)(quiet / 1000));
      return Health{ServiceState::Degraded, _reason};
    }
    return Health{ServiceState::Healthy, ""};
  #else
    return LoopService::health();
  #endif
  }
  void telemetry(NodeTelemetry& report) const override {
    if (loop_services_lora_absent) return;
    report.if_present |= TELEMETRY_IF_LORA;
    if (radio_online) report.if_up |= TELEMETRY_IF_LORA;
  }
private:
  mutable char _reason[24] = {};
};

class LoraConfigService : public LoopService {
public:
  LoraConfigService() : LoopService(LOOP_PHASE_LORA_CFG) {}
  void poll(uint32_t) override { lora_config_consistency_watch(); }
  Health health() const override {
    if (loop_services_lora_absent) return Health{ServiceState::Disabled, "no LoRa hardware"};
    return LoopService::health();
  }
};

class RadioCommitService : public LoopService {
public:
  RadioCommitService() : LoopService(LOOP_PHASE_RADIO_CMT) {}
  void poll(uint32_t) override { radio_commit_confirm_watch(); }
  // A PHY change waiting for its first packet: working, not yet proven.
  Health health() const override {
    if (loop_services_lora_absent) return Health{ServiceState::Disabled, "no LoRa hardware"};
    if (rb_armed) return Health{ServiceState::Degraded, "config unconfirmed"};
    return Health{ServiceState::Healthy, ""};
  }
};

#if defined(LXMF_PROPAGATION_NODE)
class LxmfAnnounceService : public LoopService {
public:
  LxmfAnnounceService() : LoopService(LOOP_PHASE_LXMF_ANN) {}
  void poll(uint32_t) override { lxmf_propagation_announce_watch(); }
  uint32_t budget_ms() const override { return ANNOUNCE_BUDGET_MS; }
};

class LxmfSyncService : public LoopService {
public:
  LxmfSyncService() : LoopService(LOOP_PHASE_LXMF_SYNC) {}
  void poll(uint32_t) override { lxmf_peer_sync_watch(); }
};
#endif
#endif

#ifdef HAS_RNS
class ReticulumService : public LoopService {
public:
  ReticulumService() : LoopService(LOOP_PHASE_RETICULUM) {}
  void poll(uint32_t now_ms) override {
    if (!reticulum) return;
    try {
      reticulum.loop();
    }
    catch (const std::bad_alloc&) {
      note_error(now_ms);
      ERROR("RNS loop failed: bad_alloc - out of memory");
    }
    catch (std::exception& e) {
      note_error(now_ms);
      ERRORF("RNS loop failed: %s", e.what());
    }
  }
  Health health() const override {
    if (!reticulum) return Health{ServiceState::Failed, "not started"};
    if (_errors > 0 && millis() - _last_error_ms < SERVICE_RNS_ERROR_WINDOW_MS)
      return Health{ServiceState::Degraded, "loop threw"};
    return Health{ServiceState::Healthy, ""};
  }
  void telemetry(NodeTelemetry& report) const override {
    // The live table; path_table() is the retired one, empty since microStore.
    report.paths = RNS::Transport::new_path_table().size();
    report.nodes = RNS::Identity::known_destinations().size();
  }
private:
  void note_error(uint32_t now_ms) { ++_errors; _last_error_ms = now_ms; }
  uint32_t _errors = 0;
  uint32_t _last_error_ms = 0;
};
#endif

// Reports on the others: one line per HEAP_REPORT_INTERVAL_MS, beside [mem], so
// a soak log shows a service going wrong without anyone asking. Unmeasured
// services are counted, healthy ones are not listed; anything else is named
// with its state and reason. Registered last, so its own poll is measured like the rest.
class ServiceReportService : public LoopService {
public:
  explicit ServiceReportService(const ServiceRunner& runner)
    : LoopService(LOOP_PHASE_SVC_REPORT), _runner(runner) {}
  void poll(uint32_t now_ms) override {
    if (_last != 0 && now_ms - _last < HEAP_REPORT_INTERVAL_MS) return;
    _last = now_ms == 0 ? 1 : now_ms;
    size_t running = 0, slowest = 0, unmeasured = 0;
    uint32_t overruns = 0, slowest_ms = 0;
    for (size_t i = 0; i < _runner.count(); ++i) {
      if (_runner.running(i)) ++running;
      if (_runner.health(i).state == ServiceState::Unmeasured) ++unmeasured;
      const ServiceTiming* t = _runner.timing(i);
      if (t == nullptr) continue;
      overruns += t->overruns;
      if (t->worst_ms > slowest_ms) { slowest_ms = t->worst_ms; slowest = i; }
    }
    // Built whole and printed once, so other output cannot split the line.
    size_t at = (size_t)snprintf(_line, sizeof(_line), "[svc] running=%u/%u unmeasured=%u overruns=%lu slowest=%s:%lums",
        (unsigned)running, (unsigned)_runner.count(), (unsigned)unmeasured, (unsigned long)overruns,
        _runner.service(slowest)->name(), (unsigned long)slowest_ms);
    for (size_t i = 0; i < _runner.count() && at < sizeof(_line); ++i) {
      const Health h = _runner.health(i);
      if (h.state == ServiceState::Healthy || h.state == ServiceState::Unmeasured) continue;
      at += (size_t)snprintf(_line + at, sizeof(_line) - at, " | %s %s%s%s",
          _runner.service(i)->name(), service_state_name(h.state),
          h.reason[0] ? ": " : "", h.reason);
    }
    printf("%s\n", _line);
  }
private:
  const ServiceRunner& _runner;
  uint32_t _last = 0;
  char _line[256] = {};
};

#ifdef HAS_RNS
// ---- telemetry uplink (PR F step F4a) ---------------------------------------
//
// The board's health report, to the nearest gateway (TelemetryUplink.h). The
// report is the sum of the node's status and every service's own account: the
// node-level fields first, then ServiceRunner::collect() for what the services
// own -- heap, PSRAM, BLE and LoRa state.

static TelemetryGateways& telemetry_gateways() {
  static TelemetryGateways gateways;
  return gateways;
}

class TelemetryUplinkAnnounceHandler : public RNS::AnnounceHandler {
public:
  TelemetryUplinkAnnounceHandler() : RNS::AnnounceHandler(TELEMETRY_UPLINK_FILTER) {}
  void received_announce(const RNS::Bytes& destination_hash,
                         const RNS::Identity& announced_identity,
                         const RNS::Bytes& app_data) override {
    (void)announced_identity; (void)app_data;
    if (telemetry_gateways().count((uint32_t)millis()) == 0) {
      printf("[telemetry] gateway heard: <%s>\n", destination_hash.toHex().substr(0, 16).c_str());
    }
    telemetry_gateways().heard(destination_hash.data(), destination_hash.size(), (uint32_t)millis());
  }
};

static uint8_t telemetry_reset_code() {
#if defined(ESP32)
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return TELEMETRY_RESET_POWERON;
    case ESP_RST_SW:       return TELEMETRY_RESET_SOFTWARE;
    case ESP_RST_PANIC:    return TELEMETRY_RESET_PANIC;
    case ESP_RST_TASK_WDT: return TELEMETRY_RESET_TASK_WDT;
    case ESP_RST_INT_WDT:  return TELEMETRY_RESET_INT_WDT;
    case ESP_RST_BROWNOUT: return TELEMETRY_RESET_BROWNOUT;
    case ESP_RST_EXT:      return TELEMETRY_RESET_EXTERNAL;
    case ESP_RST_UNKNOWN:  return TELEMETRY_RESET_UNKNOWN;
    default:               return TELEMETRY_RESET_OTHER;
  }
#else
  return TELEMETRY_RESET_UNKNOWN;
#endif
}

// What no service owns: who this is, how long it has run, why it restarted,
// which carriers it has, and its battery.
static void telemetry_fill_node(NodeTelemetry& report) {
  const NodeStatusView s = node_status();
  const RNS::Bytes self = RNS::Transport::identity().hash();
  if (self.size() >= 4) {
    const uint8_t* h = self.data();
    report.sender_id = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
                       ((uint32_t)h[2] << 8) | (uint32_t)h[3];
  }
  report.uptime_s = s.uptime_s;
  report.reset = telemetry_reset_code();
  report.boots = s.boots;
  report.crashes = s.crashes;
  report.panics = s.panics;
  if (s.lora_present)   report.if_present |= TELEMETRY_IF_LORA;
  if (s.lora_active)    report.if_up      |= TELEMETRY_IF_LORA;
  if (s.ble_present)    report.if_present |= TELEMETRY_IF_BLE;
  if (s.ble_active)     report.if_up      |= TELEMETRY_IF_BLE;
  if (s.wifi_present)   report.if_present |= TELEMETRY_IF_WIFI;
  if (s.wifi_active)    report.if_up      |= TELEMETRY_IF_WIFI;
  if (s.espnow_present) report.if_present |= TELEMETRY_IF_ESPNOW;
  if (s.espnow_active)  report.if_up      |= TELEMETRY_IF_ESPNOW;
  report.espnow_peers = s.espnow_peers;
  report.paths = s.paths;
  report.nodes = s.nodes;
  report.relaying = s.relaying;
  report.relay_expected = s.relay_expected;
  report.time_current = s.time_current;
  if (battery_installed && battery_ready) {
    report.battery_known = true;
    report.battery_mv = (uint16_t)(battery_voltage * 1000.0f);
    report.battery_pct = (uint8_t)battery_percent;
  }
}

#ifndef TELEMETRY_BATCH_INTERVAL_MS
// T2: kept reports leave about hourly while no gateway is in reach -- one LXMF
// message into this node's own propagation store, collected by the gateway
// when it next reaches the store.
#define TELEMETRY_BATCH_INTERVAL_MS (60UL * 60UL * 1000UL)
#endif
#define TELEMETRY_BATCH_TITLE "telemetry/batch"

// Live reports go only to a gateway heard this recently. Reticulum keeps a path
// for days after its far end has gone, so "has a path" cannot tell a board that
// drove out of range; a gateway announces every 10 minutes, and two and a half
// of those unheard is the board's cue to keep its reports instead (T2). A false
// alarm only delays reports into a batch. Without a propagation store there is
// nothing to keep them in, and a stale gateway is still the best guess.
#ifndef TELEMETRY_GATEWAY_LIVE_MS
#if defined(LXMF_PROPAGATION_NODE)
#define TELEMETRY_GATEWAY_LIVE_MS (25UL * 60UL * 1000UL)
#else
#define TELEMETRY_GATEWAY_LIVE_MS TELEMETRY_GATEWAY_EXPIRY_MS
#endif
#endif

#ifndef TELEMETRY_SEND_BUDGET_MS
#define TELEMETRY_SEND_BUDGET_MS 300
#endif

#ifndef TELEMETRY_DETAIL_INTERVAL_MS
// The detail report's cadence: who this board hears and what it runs change
// slowly, and on LoRa its ~100-300 bytes are shared airtime.
#define TELEMETRY_DETAIL_INTERVAL_MS (30UL * 60UL * 1000UL)
#endif

static uint32_t detail_sat32(size_t v) { return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v; }
static int8_t detail_dbm(float v) {
  if (!(v > -128.0f) || v > 127.0f) return DETAIL_RSSI_UNKNOWN;   // NaN and the -292 "unknown" sentinel
  return (int8_t)v;
}

// Who this board hears directly, recorded by the receive callback as announces
// arrive (TelemetryNeighbours.h). Interface kinds are cached by interface, so a
// received announce costs no string copy; only its identity hash allocates.
static NeighbourTable& telemetry_neighbours() { static NeighbourTable table; return table; }

void telemetry_neighbour_heard(const RNS::Bytes& raw, const RNS::Interface& interface) {
  size_t at = 0;
  const AnnounceSource source = announce_neighbour(raw.data(), raw.size(), at);
  if (source == AnnounceSource::None) return;

  static struct { const void* impl; uint8_t kind; } kinds[DETAIL_MAX_INTERFACES] = {};
  const void* impl = const_cast<RNS::Interface&>(interface).get();
  uint8_t kind = DETAIL_IF_OTHER;
  bool cached = false;
  for (auto& k : kinds) {
    if (k.impl == impl) { kind = k.kind; cached = true; break; }
  }
  if (!cached) {
    kind = detail_kind_of(interface.name().c_str());
    for (auto& k : kinds) {
      if (k.impl == nullptr) { k.impl = impl; k.kind = kind; break; }
    }
  }

  uint32_t id;
  if (source == AnnounceSource::TransportId) {
    id = neighbour_id_of(raw.data() + at);
  } else {
    const RNS::Bytes hash = RNS::Identity::full_hash(raw.mid(at, NEIGHBOUR_PUBKEY_LEN));
    if (hash.size() < 4) return;
    id = neighbour_id_of(hash.data());
  }
  // Our own announce, looped back, is not a neighbour: kept out of the table,
  // where it would take a slot and could evict a real one.
  static uint32_t self_id = 0;
  if (self_id == 0 && RNS::Transport::identity()) self_id = neighbour_id_of(RNS::Transport::identity().hash().data());
  if (id == self_id) return;
  int8_t rssi = DETAIL_RSSI_UNKNOWN;
#if defined(LORA_TRANSPORT) && !defined(NO_LORA_HARDWARE)
  if (kind == DETAIL_IF_LORA) rssi = detail_dbm((float)last_rssi);   // this packet's, the radio's last
#endif
  telemetry_neighbours().heard(id, kind, rssi, (uint32_t)millis());
}

// The detail report: what this board runs, its interfaces, its radio, its
// propagation store and who it hears (TelemetryDetailCodec.h).
static void telemetry_fill_detail(NodeDetail& d, uint32_t sender_id, uint32_t uptime_s) {
  // Reset field by field: `d = NodeDetail{}` may build an 800-byte temporary
  // on the loop task's stack.
  d.env[0] = 0;
  d.if_count = 0;
  d.radio_known = false;
  d.propagation_known = false;
  d.nb_count = 0;
  d.neighbours_truncated = false;
  d.temperature_c = DETAIL_TEMP_UNKNOWN;
  d.lora_rx = 0;
  d.lora_tx = 0;
  d.lora_crc_errors = DETAIL_UNKNOWN32;
  d.time_source = 0;
  d.time_age_s = DETAIL_NEVER;
  d.ifac_rejected = DETAIL_UNKNOWN32;
  d.sender_id = sender_id;
  d.uptime_s = uptime_s;
  memcpy(d.fw_hash, dev_firmware_hash, 4);
  d.fw_version = (uint16_t)((MAJ_VERS << 8) | MIN_VERS);
#ifdef FW_BUILD_ENV
  strncpy(d.env, FW_BUILD_ENV, DETAIL_ENV_MAX);
  d.env[DETAIL_ENV_MAX] = 0;
#endif
  for (const RNS::Interface& iface : RNS::Transport::get_interfaces()) {
    if (d.if_count >= DETAIL_MAX_INTERFACES) break;
    DetailInterface& f = d.interfaces[d.if_count++];
    f.kind = detail_kind_of(iface.name().c_str());
    f.up = iface.online();
    f.rx_bytes = detail_sat32(iface.rxbytes());
    f.tx_bytes = detail_sat32(iface.txbytes());
  }
#if defined(LORA_TRANSPORT) && !defined(NO_LORA_HARDWARE)
  if (lora_interface) {
    d.radio_known = true;
    d.rssi = detail_dbm((float)last_rssi);
    d.snr_q = (int8_t)last_snr_raw;                 // the radio's own quarter-dB figure
    d.noise = detail_dbm((float)noise_floor);
    d.utilisation_pct = (uint8_t)(total_channel_util * 100.0f + 0.5f);
    d.airtime_pct = (uint8_t)(airtime * 100.0f + 0.5f);
  }
#endif
#if defined(LXMF_PROPAGATION_NODE)
  d.propagation_known = true;
  d.store_messages = (uint32_t)lxmf_store_index.size();
  d.store_bytes = detail_sat32(lxmf_store_bytes());
  d.pn_peers = (uint8_t)(lxmf_peers().size() > 255 ? 255 : lxmf_peers().size());
  const LXMFSyncStats& sync = lxmf_sync_stats();
  d.sync_ok = sync.ok;
  d.sync_fail = sync.failed;
  d.last_sync_s = sync.any_ok ? ((uint32_t)millis() - sync.last_ok_ms) / 1000u : DETAIL_NEVER;
#endif
  telemetry_neighbours().fill(d, (uint32_t)millis());

  // FLAG_SYSTEM: the chip's temperature, LoRa packets and CRC errors, the
  // clock's source and age, IFAC rejections (unknown until the library counts
  // them).
  d.system_known = true;
#if defined(ESP32)
  // The chip's own sensor, read once per report (a few ms; the RAD boards have
  // no PMU, whose reading Power.h would otherwise keep).
  const float t = temperatureRead();
  if (t == t) d.temperature_c = (int8_t)(t > 127.0f ? 127 : (t < -127.0f ? -127 : (int)t));   // not NaN
#endif
#if defined(LORA_TRANSPORT) && !defined(NO_LORA_HARDWARE)
  d.lora_rx = stat_rx;
  d.lora_tx = stat_tx;
  if (LoRa != nullptr) d.lora_crc_errors = LoRa->crcErrors();
#endif
  if (RNS::Utilities::OS::wall_time_known()) {
    d.time_source = (uint8_t)RNS::Utilities::OS::wall_time_source();
    const uint64_t now = RNS::Utilities::OS::monotonic_time_millis();
    const uint64_t adopted = RNS::Utilities::OS::wall_time_adopted_at();
    d.time_age_s = now > adopted ? (uint32_t)((now - adopted) / 1000u) : 0u;
  }
}

class TelemetryUplinkService : public LoopService {
public:
  explicit TelemetryUplinkService(const ServiceRunner& runner)
    : LoopService(LOOP_PHASE_TELEMETRY), _runner(runner), _schedule(0) {}

  // A poll that sends encrypts one packet to the gateway: 167 ms measured on
  // Rev 1 (2026-10-04), once per health report and once per detail report,
  // never both in one poll. A T2 batch (sign, encrypt, flash write) measured
  // 159-237 ms on Rev 1 (2026-10-08), in a poll of its own; its announce in
  // the next. Every other poll returns at once.
  uint32_t budget_ms() const override { return TELEMETRY_SEND_BUDGET_MS; }

  bool init(const AppContext& context) override {
    _schedule = TelemetrySchedule(context.boot_ms);
#if defined(LXMF_PROPAGATION_NODE)
    if (!t2_allocate()) printf("[telemetry] T2 off: no memory for kept reports\n");
#endif
    static RNS::HAnnounceHandler handler(new TelemetryUplinkAnnounceHandler());
    RNS::Transport::register_announce_handler(handler);
    return true;
  }

  // One report a minute after boot, then every TELEMETRY_INTERVAL_MS. With no
  // gateway reachable, ask for a path to the newest heard and retry sooner.
  void poll(uint32_t now_ms) override {
    if (_detail_gateway) { send_detail(now_ms); return; }
#if defined(LXMF_PROPAGATION_NODE)
    // The announce that lets the gateway verify a batch: a poll after the
    // batch, so the two signatures never share one.
    if (_announce_pending) {
      _announce_pending = false;
      announce_delivery();
      return;
    }
    // Back in reach with reports kept: they leave now, a poll after the live
    // report so the two cryptographic jobs never share one.
    if (_flush_gateway) {
      const RNS::Identity gateway = _flush_gateway;
      _flush_gateway = {RNS::Type::NONE};
      compose_kept(now_ms, gateway);
      return;
    }
#endif
    if (!_schedule.due(now_ms)) return;
    _attempted = true;
    if (!reticulum || !RNS::Transport::identity()) { _schedule.unreachable(now_ms); return; }

    const TelemetryGateways::Entry* best = telemetry_gateways().best(now_ms, [](const uint8_t* hash) -> uint8_t {
      const RNS::Bytes h(hash, TELEMETRY_HASH_LEN);
      if (!RNS::Transport::has_path(h)) return TELEMETRY_HOPS_UNKNOWN;
      const uint8_t hops = RNS::Transport::hops_to(h);
      return hops >= TELEMETRY_HOPS_UNKNOWN ? (uint8_t)(TELEMETRY_HOPS_UNKNOWN - 1) : hops;
    }, TELEMETRY_GATEWAY_LIVE_MS);
    if (best == nullptr) {
      const TelemetryGateways::Entry* newest = telemetry_gateways().newest(now_ms);
      if (newest != nullptr) RNS::Transport::request_path(RNS::Bytes(newest->hash, TELEMETRY_HASH_LEN));
      keep_while_unreachable(now_ms);
      _schedule.unreachable(now_ms);
      return;
    }
    const RNS::Bytes gateway_hash(best->hash, TELEMETRY_HASH_LEN);
    const RNS::Identity gateway_identity = RNS::Identity::recall(gateway_hash);
#if defined(LXMF_PROPAGATION_NODE)
    if (gateway_identity) _last_gateway = gateway_identity;
#endif
    if (!gateway_identity) {
      RNS::Transport::request_path(gateway_hash);
      keep_while_unreachable(now_ms);
      _schedule.unreachable(now_ms);
      return;
    }

    NodeTelemetry report;
    telemetry_fill_node(report);
    _runner.collect(report);
    uint8_t wire[TELEMETRY_WIRE_MAX_LEN];
    const size_t len = telemetry_encode(report, wire, sizeof(wire));
    if (len == 0) { _schedule.unreachable(now_ms); return; }

    // A single packet, like a position report: a link would cost kilobytes of
    // heap to deliver thirty bytes, and a health report needs no reply. SINGLE
    // is already encrypted to the gateway's identity.
    RNS::Destination gateway(gateway_identity, RNS::Type::Destination::OUT,
                             RNS::Type::Destination::SINGLE,
                             RNS::Type::Transport::APP_NAME, TELEMETRY_UPLINK_ASPECT);
    RNS::Packet packet(gateway, RNS::Bytes(wire, len));
    packet.send();
    ++_sent;
    if (!_schedule.ever_sent()) {
      printf("[telemetry] first report to <%s>, %u bytes\n",
             gateway_hash.toHex().substr(0, 16).c_str(), (unsigned)len);
    }
    _schedule.sent(now_ms);

    // The detail report follows on the next poll -- same gateway, first with
    // the first health report, then every half hour. A poll apart, so the two
    // encryptions (~170 ms each on Rev 1) never share one.
    if (!_detail_sent || now_ms - _detail_last >= TELEMETRY_DETAIL_INTERVAL_MS) {
      _detail_gateway = gateway_identity;
      _detail_sender = report.sender_id;
      _detail_uptime = report.uptime_s;
    }
#if defined(LXMF_PROPAGATION_NODE)
    if (t2() && !t2()->kept.empty()) _flush_gateway = gateway_identity;
#endif
  }

  Health health() const override {
    const uint32_t now = (uint32_t)millis();
    if (!_attempted) return Health{ServiceState::Starting, "first report pending"};
    if (telemetry_gateways().count(now) == 0) return Health{ServiceState::Degraded, "no gateway heard"};
    if (!_schedule.ever_sent()) return Health{ServiceState::Degraded, "no path to a gateway"};
    const uint32_t since = now - _schedule.last_sent();
    if (since > 2u * TELEMETRY_INTERVAL_MS) {
      snprintf(_reason, sizeof(_reason), "last report %lus ago", (unsigned long)(since / 1000));
      return Health{ServiceState::Degraded, _reason};
    }
    return Health{ServiceState::Healthy, ""};
  }

private:
#if defined(LXMF_PROPAGATION_NODE)
  // T2, LXMF reach (TelemetryKept.h, TelemetryBatchCodec.h): the kept reports,
  // and the batch they leave in, as one block allocated once in init() -- in
  // PSRAM where the board has it, since internal RAM is what these boards run
  // short of (about 5 KB).
  struct T2Buffers {
    TelemetryKept kept;
    BatchEntry entries[TELEMETRY_KEPT_MAX_ENTRIES];
    uint8_t batch[TELEMETRY_BATCH_MAX_LEN];
  };
  static T2Buffers*& t2() { static T2Buffers* buffers = nullptr; return buffers; }
  static bool t2_allocate() {
    if (t2()) return true;
    void* mem = nullptr;
#if defined(ESP32)
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) mem = heap_caps_malloc(sizeof(T2Buffers), MALLOC_CAP_SPIRAM);
#endif
    if (mem == nullptr) mem = malloc(sizeof(T2Buffers));
    if (mem == nullptr) return false;
    t2() = new (mem) T2Buffers();
    return true;
  }

  static uint32_t unix_now() {
    return RNS::Utilities::OS::wall_time_known() ? (uint32_t)RNS::Utilities::OS::wall_time() : 0u;
  }

  // While no gateway is in reach: one health report per interval, a detail
  // report per detail interval, nothing cryptographic; then the batch, once an
  // hour's worth is kept or the keeper is nearly full.
  void keep_while_unreachable(uint32_t now_ms) {
    if (!t2() || !RNS::Transport::identity()) return;
    TelemetryKept& kept = t2()->kept;
    if (!_kept_any || now_ms - _kept_last >= TELEMETRY_INTERVAL_MS) {
      _kept_any = true;
      _kept_last = now_ms;
      NodeTelemetry report;
      telemetry_fill_node(report);
      _runner.collect(report);
      uint8_t wire[TELEMETRY_WIRE_MAX_LEN];
      const size_t len = telemetry_encode(report, wire, sizeof(wire));
      if (len > 0) kept.keep(wire, len, now_ms, unix_now());
      if (!_kept_detail_any || now_ms - _kept_detail_last >= TELEMETRY_DETAIL_INTERVAL_MS) {
        _kept_detail_any = true;
        _kept_detail_last = now_ms;
        NodeDetail& detail = detail_scratch();
        uint8_t* dwire = detail_wire_scratch();
        telemetry_fill_detail(detail, report.sender_id, report.uptime_s);
        const size_t dlen = telemetry_detail_encode(detail, dwire, TELEMETRY_DETAIL_WIRE_MAX_LEN);
        if (dlen > 0) kept.keep(dwire, dlen, now_ms, unix_now());
      }
      return;   // compose on a later poll, not beside the reports' filling
    }
    if (kept.empty()) return;
    const bool hour = now_ms - kept.oldest_ms() >= TELEMETRY_BATCH_INTERVAL_MS;
    const bool nearly_full = kept.encoded_len() + TELEMETRY_BATCH_ENTRY_HEADER + TELEMETRY_DETAIL_WIRE_MAX_LEN
                             > TELEMETRY_BATCH_MAX_LEN;
    if (!hour && !nearly_full) return;
    if (_compose_failed && now_ms - _compose_failed_at < TELEMETRY_INTERVAL_MS) return;
    // Addressed to the newest gateway heard, or -- in a partition longer than
    // the gateway list's two-hour expiry -- the last one this node knew.
    const TelemetryGateways::Entry* newest = telemetry_gateways().newest(now_ms);
    if (newest != nullptr) {
      const RNS::Identity heard = RNS::Identity::recall(RNS::Bytes(newest->hash, TELEMETRY_HASH_LEN));
      if (heard) _last_gateway = heard;
    }
    if (!_last_gateway) return;   // no gateway known since boot: nobody to address; keep
    compose_kept(now_ms, _last_gateway);
  }

  // The kept reports as one LXMF message to the gateway's delivery
  // destination, signed by this node, into this node's own propagation store.
  // On the next poll this node's delivery destination is announced, so the
  // gateway can verify the signature when it collects the message. One
  // signature, one encryption and a flash write in this poll: 159-237 ms on
  // Rev 1, within TELEMETRY_SEND_BUDGET_MS.
  void compose_kept(uint32_t now_ms, const RNS::Identity& gateway_identity) {
    if (!t2() || t2()->kept.empty()) return;
    const uint32_t started = (uint32_t)millis();
    TelemetryKept& kept = t2()->kept;
    BatchEntry* entries = t2()->entries;
    uint8_t* batch = t2()->batch;
    const RNS::Identity& self = RNS::Transport::identity();
    const size_t n = kept.entries(entries, TELEMETRY_KEPT_MAX_ENTRIES, now_ms);
    const RNS::Bytes self_hash = self.hash();
    const uint8_t* h = self_hash.data();
    const uint32_t sender = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | (uint32_t)h[3];
    const size_t len = telemetry_batch_encode(sender, unix_now(), kept.dropped(), entries, n, batch,
                                              TELEMETRY_BATCH_MAX_LEN);

    RNS::Destination self_delivery(self, RNS::Type::Destination::OUT, RNS::Type::Destination::SINGLE,
                                   LXMF_APP_NAME, LXMF_DELIVERY_ASPECT);
    RNS::Destination gateway(gateway_identity, RNS::Type::Destination::OUT, RNS::Type::Destination::SINGLE,
                             LXMF_APP_NAME, LXMF_DELIVERY_ASPECT);
    const double timestamp = RNS::Utilities::OS::wall_time_known() ? RNS::Utilities::OS::wall_time() : 0.0;
    const RNS::Bytes blob = len > 0 ? lxmf_compose_propagated(self, self_delivery.hash(), gateway, timestamp,
                                                              RNS::Bytes(TELEMETRY_BATCH_TITLE),
                                                              RNS::Bytes(batch, len))
                                    : RNS::Bytes();
    if (!blob || !lxmf_store_put(blob)) {
      _compose_failed = true;
      _compose_failed_at = now_ms;
      printf("[telemetry] could not store a batch of %u kept report(s); kept for later\n", (unsigned)n);
      return;
    }
    _compose_failed = false;
    printf("[telemetry] %u kept report(s) in a %u-byte batch for <%s>, held in this node's store (%lu ms)\n",
           (unsigned)n, (unsigned)len, gateway.hash().toHex().substr(0, 16).c_str(),
           (unsigned long)((uint32_t)millis() - started));
    kept.clear();
    _announce_pending = true;
  }

  // This node's lxmf.delivery, registered once so it answers path requests and
  // can announce. Registered elsewhere already (the RRC bridge, when built):
  // that announces it, and nothing is lost here.
  static void announce_delivery() {
    static bool tried = false;
    static RNS::Destination delivery{RNS::Type::NONE};
    if (!tried) {
      tried = true;
      try {
        delivery = RNS::Destination(RNS::Transport::identity(), RNS::Type::Destination::IN,
                                    RNS::Type::Destination::SINGLE, LXMF_APP_NAME, LXMF_DELIVERY_ASPECT);
      } catch (const std::exception&) {
        delivery = {RNS::Type::NONE};
      }
    }
    if (delivery) delivery.announce();
  }
#else
  void keep_while_unreachable(uint32_t) {}   // T2 needs a propagation store
#endif

  // One detail report and its wire bytes for the live send and the keeper
  // alike: static, so nothing this size is allocated per report.
  static NodeDetail& detail_scratch() { static NodeDetail d; return d; }
  static uint8_t* detail_wire_scratch() { static uint8_t w[TELEMETRY_DETAIL_WIRE_MAX_LEN]; return w; }

  // Statics for the report and its wire bytes: nothing this size is
  // allocated per report.
  void send_detail(uint32_t now_ms) {
    NodeDetail& detail = detail_scratch();
    uint8_t* wire = detail_wire_scratch();
    const RNS::Identity gateway_identity = _detail_gateway;
    _detail_gateway = {RNS::Type::NONE};
    telemetry_fill_detail(detail, _detail_sender, _detail_uptime);
    const size_t len = telemetry_detail_encode(detail, wire, TELEMETRY_DETAIL_WIRE_MAX_LEN);
    if (len == 0) return;
    RNS::Destination gateway(gateway_identity, RNS::Type::Destination::OUT,
                             RNS::Type::Destination::SINGLE,
                             RNS::Type::Transport::APP_NAME, TELEMETRY_UPLINK_ASPECT);
    RNS::Packet packet(gateway, RNS::Bytes(wire, len));
    packet.send();
    if (!_detail_sent) {
      printf("[telemetry] first detail report, %u bytes: %u interface(s), %u neighbour(s)\n",
             (unsigned)len, (unsigned)detail.if_count, (unsigned)detail.nb_count);
    }
    _detail_sent = true;
    _detail_last = now_ms;
  }

  const ServiceRunner& _runner;
  TelemetrySchedule _schedule;
  bool _attempted = false;
  uint32_t _sent = 0;
  bool _detail_sent = false;
  uint32_t _detail_last = 0;
  RNS::Identity _detail_gateway{RNS::Type::NONE};
  uint32_t _detail_sender = 0;
  uint32_t _detail_uptime = 0;
#if defined(LXMF_PROPAGATION_NODE)
  bool _kept_any = false;
  uint32_t _kept_last = 0;
  bool _kept_detail_any = false;
  uint32_t _kept_detail_last = 0;
  bool _compose_failed = false;
  uint32_t _compose_failed_at = 0;
  RNS::Identity _flush_gateway{RNS::Type::NONE};
  // Batches are addressed to the last gateway this node knew, which does not
  // expire: live selection needs a recent one, a batch only needs an address.
  RNS::Identity _last_gateway{RNS::Type::NONE};
  bool _announce_pending = false;
#endif
  mutable char _reason[32] = {};
};
#endif

static uint32_t loop_services_clock() { return (uint32_t)millis(); }

// The breadcrumb, before every poll: if this poll never returns, the TASK_WDT
// report on the next boot names it.
static void loop_services_before_poll(size_t index);

static ServiceRunner& loop_services_runner() {
  static ServiceRunner runner(loop_services_clock, loop_services_before_poll);
  return runner;
}

static void loop_services_before_poll(size_t index) {
  // Every service in this runner is a LoopService (loop_services_start adds
  // nothing else), so the cast is exact.
  const IService* service = loop_services_runner().service(index);
  if (service != nullptr) loop_phase(static_cast<const LoopService*>(service)->phase());
}

const ServiceRunner& loop_services() { return loop_services_runner(); }

// A full table is a build configuration error: said, in one line, never a
// silent drop (ServiceRunner::add).
static void loop_services_add(ServiceRunner& runner, LoopService* service) {
  if (!runner.add(service)) {
    printf("[svc] %s not added: service table full (%u), raise SERVICE_RUNNER_CAPACITY\n",
           service->name(), (unsigned)SERVICE_RUNNER_CAPACITY);
  }
}

void loop_services_start() {
  ServiceRunner& runner = loop_services_runner();
  // The order is loop()'s, which it was before F3b. Statics, so nothing is
  // allocated after boot.
#if defined(ESP32) && defined(HAS_RNS)
  static HeapService heap;                 loop_services_add(runner, &heap);
#endif
#if defined(HAS_RNS) && defined(URTN_STATS_PAGES)
  static NomadAnnounceService nomad;       loop_services_add(runner, &nomad);
#endif
#if defined(HAS_RNS) && defined(RRC_HUB)
  static RrcHubService rrc;                loop_services_add(runner, &rrc);
#endif
#if defined(BLE_PEER_TRANSPORT)
  static BlePeerService ble_peer;          loop_services_add(runner, &ble_peer);
#endif
#if defined(HAS_RNS) && defined(LORA_TRANSPORT)
  static RadioRxWatchService radio_wd;     loop_services_add(runner, &radio_wd);
  static LoraConfigService lora_cfg;       loop_services_add(runner, &lora_cfg);
  static RadioCommitService radio_cmt;     loop_services_add(runner, &radio_cmt);
#if defined(LXMF_PROPAGATION_NODE)
  static LxmfAnnounceService lxmf_ann;     loop_services_add(runner, &lxmf_ann);
  static LxmfSyncService lxmf_sync;        loop_services_add(runner, &lxmf_sync);
#endif
#endif
#ifdef HAS_RNS
  static ReticulumService rns;             loop_services_add(runner, &rns);
#endif
  static ServiceReportService report(runner);
  loop_services_add(runner, &report);
#ifdef HAS_RNS
  static TelemetryUplinkService telemetry(runner);
  loop_services_add(runner, &telemetry);
#endif

  AppContext context;
  context.boot_ms = (uint32_t)millis();
  const size_t running = runner.start_all(context);
  printf("[svc] %u of %u services running\n", (unsigned)running, (unsigned)runner.count());
  for (size_t i = 0; i < runner.count(); ++i) {
    const Health h = runner.health(i);
    if (runner.running(i) && h.state != ServiceState::Disabled) continue;
    printf("[svc] %s %s: %s\n", runner.service(i)->name(), service_state_name(h.state), h.reason);
  }
}

void loop_services_poll() { loop_services_runner().poll_all(); }
