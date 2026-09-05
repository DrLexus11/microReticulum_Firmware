#include "NodeStatus.h"

#if defined(HAS_RNS)

#include <microStore/FileSystem.h>

// Config.h cannot be included here -- see the note in NodeStatus.h -- so the
// one constant this needs is restated, exactly as Provisioning.cpp restates it.
#ifndef MODE_TNC
#define MODE_TNC 0x12
#endif

// Declared rather than included, the way Pages.h reaches the same globals: the
// firmware's state lives in the .ino translation unit and pulling its headers
// in here would drag the whole RNode build surface with them.
extern char nomadnet_name[];
extern uint8_t op_mode;
extern microStore::FileSystem filesystem;

#if MCU_VARIANT == MCU_ESP32
extern uint32_t boot_count;
#endif

#if HAS_WIFI == true
#include <WiFi.h>
#endif

#if HAS_WIFI == true && defined(ESPNOW_TRANSPORT)
extern uint32_t espnow_peer_count();
extern bool espnow_local_has_upstream();
extern uint32_t espnow_channel();
extern const char* espnow_recovery_state_name();
extern bool espnow_recovery_active();
extern bool espnow_recovery_pinned();
extern bool espnow_recovery_failed();
extern uint32_t espnow_recovery_channel();
#endif

#if defined(BLE_PEER_TRANSPORT)
extern bool ble_peer_started();
extern uint32_t ble_peer_packets_in();
#endif

#if defined(RRC_HUB)
#include "RRCHub.h"
#endif

// The counters live in a file of their own rather than in the bootlog, which is
// truncated once it passes 4 KB. A lifetime count that a log rotation can erase
// is not a lifetime count.
static const char* RESTART_COUNTS_PATH = "./restart_counts";

void node_restart_counts_load() {
  if (!filesystem.exists(RESTART_COUNTS_PATH)) return;
  microStore::File f = filesystem.open(RESTART_COUNTS_PATH, microStore::File::ModeRead);
  if (!f) return;
  uint32_t values[2] = {0, 0};
  if (f.read((uint8_t*)values, sizeof(values)) == sizeof(values)) {
    node_crash_count = values[0];
    node_panic_count = values[1];
  }
  f.close();
}

static void node_restart_counts_store() {
  microStore::File f = filesystem.open(RESTART_COUNTS_PATH, microStore::File::ModeWrite, true);
  if (!f) return;
  const uint32_t values[2] = {node_crash_count, node_panic_count};
  f.write((const uint8_t*)values, sizeof(values));
  f.close();
}

// Called once, at startup, after the filesystem is up. Only writes when this
// boot was actually abnormal, so a healthy node never touches the flash.
void node_restart_counts_record_boot() {
  node_restart_counts_load();
#if MCU_VARIANT == MCU_ESP32
  bool changed = false;
  switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
      node_panic_count++;
      changed = true;
      break;
    // A watchdog reset and a brownout are both "it stopped without being
    // asked", which is what this counter is for. They are separated from a
    // panic because a panic means the firmware knows it was wrong, and these
    // mean nobody was left to say so.
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
      node_crash_count++;
      changed = true;
      break;
    default:
      break;
  }
  if (changed) node_restart_counts_store();
#endif
}

// --- census handlers ---------------------------------------------------------
//
// One handler per aspect. Transport matches an announce against a handler by
// rebuilding the destination hash from the filter and the announcing identity,
// so these strings are the full expanded names, not bare aspects.
namespace {

class CensusHandler : public RNS::AnnounceHandler {
 public:
  CensusHandler(const char* filter, NodeCensusKind kind)
      : RNS::AnnounceHandler(filter), _kind(kind) {}
  void received_announce(const RNS::Bytes& destination_hash,
                         const RNS::Identity& announced_identity,
                         const RNS::Bytes& app_data) override {
    (void)announced_identity;
    (void)app_data;
    node_census_record(_kind, destination_hash.data(), destination_hash.size());
  }

 private:
  NodeCensusKind _kind;
};

}  // namespace

// Counts one entry per distinct announcing identity, regardless of aspect.
// The others key on the destination hash; this one has to key on the identity,
// or a single node announcing six destinations would count as six nodes.
class NodeCountHandler : public RNS::AnnounceHandler {
 public:
  NodeCountHandler() : RNS::AnnounceHandler(nullptr) {}
  void received_announce(const RNS::Bytes& destination_hash,
                         const RNS::Identity& announced_identity,
                         const RNS::Bytes& app_data) override {
    (void)destination_hash;
    (void)app_data;
    if (!announced_identity) return;
    const RNS::Bytes hash = announced_identity.hash();
    node_census_record(NODE_CENSUS_NODE, hash.data(), hash.size());
  }
};

void node_census_begin() {
  static RNS::HAnnounceHandler nodes(new NodeCountHandler());
  RNS::Transport::register_announce_handler(nodes);
  static RNS::HAnnounceHandler peers(
      new CensusHandler("lxmf.delivery", NODE_CENSUS_PEER));
  static RNS::HAnnounceHandler relays(
      new CensusHandler("lxmf.propagation", NODE_CENSUS_RELAY));
  static RNS::HAnnounceHandler nomad(
      new CensusHandler("nomadnetwork.node", NODE_CENSUS_NOMAD));
  RNS::Transport::register_announce_handler(peers);
  RNS::Transport::register_announce_handler(relays);
  RNS::Transport::register_announce_handler(nomad);
}

// Seconds since this board booted.
//
// Not OS::monotonic_time(): that is Reticulum's logical clock, which carries a
// persisted offset across reboots. The panel read 21h48m on a board that had
// been powered for four minutes, which is exactly the kind of number that
// makes a diagnostic worse than no diagnostic.
//
// millis() wraps every 49.7 days and these nodes are meant to run for months,
// so the wraps are counted.
uint32_t node_uptime_seconds() {
  static uint32_t last_millis = 0;
  static uint32_t wraps = 0;
  const uint32_t now = millis();
  if (now < last_millis) wraps++;
  last_millis = now;
  return (uint32_t)(((uint64_t)wraps * 4294967296ULL + (uint64_t)now) / 1000ULL);
}

NodeStatusView node_status() {
  using OS = RNS::Utilities::OS;
  NodeStatusView s;

  s.name = nomadnet_name;

#if defined(RRC_HUB)
  s.rrc_hub = rrc_hub_running();
#endif
#if defined(LXMF_PROPAGATION_NODE)
  s.propagation = true;
#endif

  // LoRa. A board with no modem fitted reports absent, not down: there is
  // nothing wrong with it.
#if !defined(NO_LORA_HARDWARE) || NO_LORA_HARDWARE == false
  s.lora_present = true;
  extern bool radio_online;
  s.lora_active = radio_online;
#endif

#if defined(BLE_PEER_TRANSPORT)
  s.ble_present = true;
  s.ble_active = ble_peer_started();
#endif

#if HAS_WIFI == true
  s.wifi_present = true;
  s.wifi_active = (WiFi.status() == WL_CONNECTED);
#endif

#if HAS_WIFI == true && defined(ESPNOW_TRANSPORT)
  s.espnow_present = true;
  s.espnow_peers = (uint16_t)espnow_peer_count();
  s.espnow_active = (s.espnow_peers > 0);
  s.espnow_channel = (uint8_t)espnow_channel();
  s.recovery_state = espnow_recovery_state_name();
  s.recovery_active = espnow_recovery_active();
  s.recovery_pinned = espnow_recovery_pinned();
  s.recovery_failed = espnow_recovery_failed();
  s.recovery_channel = (uint8_t)espnow_recovery_channel();
#endif

  s.time_known = OS::wall_time_known();
  s.stratum = OS::wall_time_stratum();
  s.unix_ms = OS::wall_time_millis();
  switch (OS::wall_time_source()) {
    case OS::WallTimeSource::NTP:                  s.time_source = "NTP"; break;
    case OS::WallTimeSource::GNSS:                 s.time_source = "GPS"; break;
    case OS::WallTimeSource::RTC:                  s.time_source = "RTC"; break;
    case OS::WallTimeSource::SIGNED_BEACON:        s.time_source = "BCN"; break;
    case OS::WallTimeSource::AUTHENTICATED_CLIENT: s.time_source = "PER"; break;
    // Restored from storage and never confirmed since: a lower bound, not a
    // measurement. Worth flagging on the panel rather than dressing up as UTC.
    case OS::WallTimeSource::PERSISTED:            s.time_source = "OLD"; break;
    default:                                       s.time_source = ""; break;
  }

  const NodeCensus& c = node_census();
  s.peers = c.counts[NODE_CENSUS_PEER];
  s.relays = c.counts[NODE_CENSUS_RELAY];
  s.nomad = c.counts[NODE_CENSUS_NOMAD];
  s.census_full = (c.overflowed > 0);
  s.nodes = c.counts[NODE_CENSUS_NODE];
  s.paths = (uint16_t)RNS::Transport::new_path_table().size();

  s.uptime_s = node_uptime_seconds();
#if MCU_VARIANT == MCU_ESP32
  s.boots = boot_count;
#endif
  s.crashes = node_crash_count;
  s.panics = node_panic_count;

  // Health, not traffic. Every interface that is fitted must be up; one that
  // was never fitted cannot be down.
  s.interfaces_ok =
      (!s.lora_present || s.lora_active) &&
      (!s.ble_present || s.ble_active) &&
      (!s.wifi_present || s.wifi_active);
  // Which one, not just that one. "IF ??" sends someone to a diagnostic page
  // to learn what a two-character label could have told them.
  if (s.lora_present && !s.lora_active) s.down_interface = "LR";
  else if (s.ble_present && !s.ble_active) s.down_interface = "BT";
  else if (s.wifi_present && !s.wifi_active) s.down_interface = "WF";
  // "Am I part of a working mesh", which is the question someone glancing at
  // the panel is asking. Neither op_mode nor transport_enabled answers it:
  // both mean "is this node a *relay*", and the firmware deliberately forces
  // transport_enabled false whenever op_mode is not MODE_TNC. A radio-less
  // board is an endpoint by construction, so gating on either reported
  // MESH -- on a node with seventeen paths in its table and two live radios.
  //
  // Relaying is a separate fact and belongs on the interfaces page, not here.
  const bool any_radio = s.lora_active || s.ble_active || s.espnow_active || s.wifi_active;
  // Paths, not nodes. "Somewhere to reach" is a route in the table; the node
  // count is now distinct identities heard announcing since boot, which is
  // legitimately zero for the first announce interval after a restart. Gating
  // the mesh indicator on it made a freshly booted, correctly peered board
  // report NO MESH for minutes.
  s.mesh_on = any_radio && (s.paths > 0);
  s.relaying = (op_mode == MODE_TNC) && RNS::Reticulum::transport_enabled();

  return s;
}

#endif  // HAS_RNS
