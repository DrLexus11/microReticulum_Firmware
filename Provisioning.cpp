// Copyright (C) 2026, Chad Attermann

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

#ifdef HAS_PROVISIONING

#include "Provisioning.h"

// For MCU_VARIANT / MCU_ESP32 and the HAS_* / WIFI_AP_* feature macros used to
// guard optional fields below. These otherwise arrive only through an
// incidental include chain, and a guard that depends on one is a guard waiting
// to be silently inverted -- an undefined macro makes `X > 0` read as `0 > 0`
// and the fields vanish with no error. That has now happened twice here.
#include "Boards.h"
#if defined(RRC_HUB)
#include "RRCHub.h"
#include "RRCBridge.h"

#if (HAS_BLUETOOTH == true || HAS_BLE == true) && MCU_VARIANT == MCU_ESP32
// Declared rather than included. Config.h cannot be included here -- it collides
// on macro names -- and Bluetooth.h is a definitions header pulled into the
// sketch translation unit. These all have external linkage, so declaring them
// links correctly, and doing it explicitly keeps the guard above resting only on
// macros Boards.h actually provides. Guarding on a macro this file cannot see
// has silently dropped provisioning fields twice in this project.
extern uint8_t bt_state;
extern uint32_t pairing_pin;
extern char bt_devname[11];
void bt_start();
void bt_stop();
void bt_conf_save(bool);
void bt_enable_pairing();
void bt_disable_pairing();
int bt_bonded_peer_count();

// Mirrors Config.h, which is the source of truth. These are reported to hosts
// over KISS, so they are effectively protocol constants and do not drift.
#define PROV_BT_STATE_NA        0xff
#define PROV_BT_STATE_OFF       0x00
#define PROV_BT_STATE_PAIRING   0x02
#define PROV_BT_STATE_CONNECTED 0x03
#endif
#endif
#include "RadioPresets.h"
#include "WebSocketConsole.h"

//#include "Config.h"

#ifdef HAS_GPIO
#include "GPIO.h"
#endif

#include <microReticulum/Log.h>

// KISS framing constants. We don't include "Framing.h" because it defines
// the parser's module-state globals (IN_FRAME, ESCAPE, command, frame_len)
// at file scope without extern guards — pulling it into a second TU
// produces ODR clashes. The wire-format values below are protocol
// constants and must match Framing.h's definitions.
#define FEND              0xC0
#define CMD_LOG           0x80
#define CMD_PROVISION_RSP 0x87

#include <microReticulum/Transport.h>
#include <microReticulum/Reticulum.h>
#include <microReticulum/Interface.h>
#include <microReticulum/Identity.h>
#include <microReticulum/Destination.h>
#include <microReticulum/Provisioning/Provisioning.h>

#include <string>
#include <vector>

// lora_interface is always declared in RNode_Firmware.ino (constructed
// with RNS::Type::NONE), even when LORA_TRANSPORT is not defined. Its
// operator bool() returns true only after setup() assigns it a real
// LoRaInterface implementation — so a single runtime check works in
// both compile configurations.
#if defined(LORA_TRANSPORT)
// Config.h defines globals at file scope, so it cannot be pulled into a second
// translation unit (see the note above); mirror the two operating-mode constants
// here instead, guarded so they collapse if that ever changes.
#ifndef MODE_HOST
  #define MODE_HOST 0x11
  #define MODE_TNC  0x12
#endif
extern uint8_t prov_op_mode;

extern RNS::Interface lora_interface;
extern uint32_t lora_freq;
extern uint32_t lora_bw;
extern int lora_sf;
extern int lora_cr;
extern int lora_txp;
extern uint32_t lora_bitrate;
extern uint8_t radio_preset_current();
extern bool radio_preset_apply(uint8_t idx);
extern uint8_t implicit_l;
#if MCU_VARIANT == MCU_ESP32
extern uint32_t loop_stack_free_min;
#endif
extern int noise_floor;
extern int current_rssi;
extern int last_rssi;
extern uint8_t last_snr_raw;
extern float st_airtime_limit;
extern float lt_airtime_limit;
#endif
#if defined(UDP_TRANSPORT)
// udp_interface is only declared in RNode_Firmware.ino under UDP_TRANSPORT,
// so this extern (and its dependents) must stay behind the same guard.
extern RNS::Interface udp_interface;
extern IPAddress wr_device_ip;
extern uint16_t udp_port;
extern uint8_t wifi_mode;
extern char wr_ssid[33];
extern void wifi_persist_ssid(const char* ssid);
// Guarded on HAS_WIFI alone. Config.h cannot be included in this translation
        // unit -- its macros collide with the Provisioning headers -- so
        // WIFI_AP_FALLBACK_MS is not visible here. Testing an invisible macro is
        // how these fields silently disappeared the first time.
#if HAS_WIFI == true
extern uint32_t wifi_ap_fallback_ms;
extern uint32_t wifi_ap_retry_sta_ms;
extern uint32_t wifi_ap_max_defer_ms;
extern bool wifi_ap_fallback_active;
extern char wifi_ap_ssid[33];
extern uint8_t wifi_ap_client_count;
#endif
#endif
#if defined(TCP_SERVER_TRANSPORT)
extern RNS::Interface tcp_server_interface;
#endif
#if HAS_WIFI
extern void wifi_remote_apply_kiss_policy();
#endif
#if HAS_BLUETOOTH || HAS_BLE == true
extern bool bt_init_ran;
extern bool bt_enabled;
extern void bt_start();
extern void bt_stop();
#endif
extern bool console_active;
extern bool wireless_kiss_policy_ready;
extern bool wireless_kiss_allowed;
extern bool kiss_framed_logs;
extern bool nomadnet_enabled;
extern RNS::Destination nomadnet_destination;
extern char nomadnet_name[64];
extern float battery_voltage;
extern float battery_percent;
extern uint8_t battery_state;
extern void hard_reset(void);
extern void eeprom_conf_save();

// ---------------------------------------------------------------------------
// External hooks into the rest of the firmware.
//
// serial_write / escaped_serial_write are defined inline in Utilities.h
// (compiled in the RNode_Firmware.ino TU). Forward-declaring them here
// avoids pulling Utilities.h — which is not include-guarded and contains
// file-scope globals — into a second translation unit.
//
// Radio config knobs and op_mode live in Config.h's global namespace and
// are only referenced by the (currently commented-out) radio namespace
// registration below. Pulling Config.h is enough since they're declared
// there at file scope.
// ---------------------------------------------------------------------------
extern void serial_write(uint8_t byte);
extern void escaped_serial_write(uint8_t byte);

// ---------------------------------------------------------------------------
// Public globals
// ---------------------------------------------------------------------------
bool provisioning_started = false;
RNS::Bytes provision_rx_buf;

static bool lora_ifac_enabled = false;
static std::string lora_ifac_netname;
static std::string lora_ifac_passphrase;
static bool tcp_ifac_enabled = false;
static std::string tcp_ifac_netname;
static std::string tcp_ifac_passphrase;
static bool udp_ifac_enabled = false;
static std::string udp_ifac_netname;
static std::string udp_ifac_passphrase;
static bool secure_node_enabled = false;

static void apply_ifac_configuration(RNS::Interface& interface,
                                     bool enabled,
                                     const std::string& netname,
                                     const std::string& passphrase,
                                     uint8_t access_code_bytes,
                                     const char* label) {
  if (!interface) return;
  if (!enabled) {
    interface.disable_ifac();
    INFOF("%s IFAC disabled", label);
    return;
  }

  // A provisioned-but-incomplete access-control configuration must never
  // silently turn the interface into an open one.
  interface.disable_ifac();
  interface.require_ifac(true);
  if (netname.empty() || passphrase.empty()) {
    ERRORF("%s IFAC configuration is incomplete; interface is fail-closed", label);
    return;
  }
  if (!interface.enable_ifac(netname.c_str(), passphrase.c_str(),
                             access_code_bytes)) {
    ERRORF("%s IFAC key derivation failed; interface is fail-closed", label);
    return;
  }
  INFOF("%s IFAC enabled for network '%s' (%u-byte access code)",
        label, netname.c_str(), (unsigned)access_code_bytes);
}

static void apply_all_ifac_configuration() {
#if defined(LORA_TRANSPORT)
  apply_ifac_configuration(lora_interface,
                           lora_ifac_enabled || secure_node_enabled,
                           lora_ifac_netname, lora_ifac_passphrase, 8, "LoRa");
#endif
#if defined(TCP_SERVER_TRANSPORT)
  apply_ifac_configuration(tcp_server_interface,
                           tcp_ifac_enabled || secure_node_enabled,
                           tcp_ifac_netname, tcp_ifac_passphrase, 16, "TCP");
#endif
#if defined(UDP_TRANSPORT)
  apply_ifac_configuration(udp_interface,
                           udp_ifac_enabled || secure_node_enabled,
                           udp_ifac_netname, udp_ifac_passphrase, 16, "UDP");
#endif
}

static void apply_secure_node_configuration() {
  wireless_kiss_policy_ready = true;
  wireless_kiss_allowed = !secure_node_enabled;
#if HAS_WIFI
  wifi_remote_apply_kiss_policy();
#endif
#if defined(ENABLE_WEBSOCKETS) && __has_include(<WiFi.h>)
  if (secure_node_enabled) ws_console::shutdown();
  else ws_console::init(81);
#endif
#if HAS_BLUETOOTH || HAS_BLE == true
  if (secure_node_enabled) {
    if (bt_init_ran) bt_stop();
  } else if (bt_init_ran && bt_enabled && !console_active) {
    bt_start();
  }
#endif
  INFOF("Secure-node posture %s; wireless KISS management %s",
        secure_node_enabled ? "enabled" : "disabled",
        wireless_kiss_allowed ? "enabled" : "disabled");
}

static void validate_ifac_commit(RNS::Provisioning::Namespace& ns,
                                 const char* label) {
  using namespace RNS::Provisioning;
  Value v;
  bool enabled = ns.draft(1, v) ? v.as_bool() : ns.working(1).as_bool();
  std::string netname = ns.draft(2, v) ? v.as_string() : ns.working(2).as_string();
  std::string passphrase = ns.draft(3, v) ? v.as_string() : ns.working(3).as_string();
  if (enabled && (netname.empty() || passphrase.empty())) {
    ERRORF("Rejected %s IFAC commit: network name and passphrase are required", label);
    ns.clear_draft();
  }
}

// ---------------------------------------------------------------------------
// Register Provisioning namespaces. Called from init_provisioning()
// before Provisioner::begin().
//
// The "radio" namespace registration is kept here purely as reference —
// EEPROM is currently the source of truth for radio configuration and a
// future revival of Provisioning-backed radio config will need its own
// migration strategy. See git history around the original Provisioning
// integration for the prior wiring.
// ---------------------------------------------------------------------------
static void register_provisioning_namespaces() {
  using namespace RNS::Provisioning;

  // ----- General namespace -----
  auto general = Provisioner::instance()
    .register_namespace("RNode General Config", PROV_NS_GENERAL)
      .field_bool("Kiss Framed Logs", PROV_GENERAL_KISS_LOG, FF_LIVE_APPLY, kiss_framed_logs,
        [](const Value& v) { kiss_framed_logs = v.as_bool(); return true; },
        []() { return kiss_framed_logs; });

#ifdef URTN_STATS_PAGES
    general
      .field_bool("NomadNet Enabled", PROV_GENERAL_NOMADNET_ENABLE, FF_REBOOT_REQUIRED, nomadnet_enabled,
        [](const Value& v) { nomadnet_enabled = v.as_bool(); return true; },
        []() { return nomadnet_enabled; })
      .field_string("NomadNet Name", PROV_GENERAL_NOMADNET_NAME, FF_REBOOT_REQUIRED, nomadnet_name, sizeof(nomadnet_name)-1,
        [](const Value& v) { strncpy(nomadnet_name, v.as_string().c_str(), sizeof(nomadnet_name)); return true; },
        []() { return nomadnet_name; });
#endif

#if defined(RRC_HUB)
#if (HAS_BLUETOOTH == true || HAS_BLE == true) && MCU_VARIANT == MCU_ESP32
  // ----- Bluetooth -----
  //
  // There was no provisioning surface for Bluetooth at all, and that is most of
  // why pairing "did not work". The stack is configured correctly --
  // ESP_LE_AUTH_REQ_SC_MITM_BOND and ESP_IO_CAP_OUT are already set, which is
  // bonding, MITM and Secure Connections with a display-only IO capability --
  // but bt_security_request_callback() rejects every pairing attempt unless
  // bt_allow_pairing is true, and that is only true for 35 seconds after an
  // explicit trigger. Until now the only triggers were a KISS frame nothing
  // sends by hand and a five-second button hold.
  //
  // So a phone tapping the device in its Bluetooth settings was refused, while
  // a GATT explorer that never requests pairing appeared to work. That is the
  // reported symptom exactly, and it is not a stack defect.
  //
  // The passkey is exposed because on a board with no display there is
  // otherwise no way to learn it. It is BLE_FIXED_PASSKEY on such boards, a
  // compile-time constant, so this reveals nothing the firmware image does not
  // already contain -- and it is read over the same link that can already
  // reflash and reconfigure the node.
  Provisioner::instance()
    .register_namespace("Bluetooth", PROV_NS_BLUETOOTH)
      .field_bool("Enabled", PROV_BT_ENABLED, FF_LIVE_APPLY,
        bt_state != PROV_BT_STATE_OFF && bt_state != PROV_BT_STATE_NA,
        [](const Value& v) {
          if (v.as_bool()) { bt_start(); bt_conf_save(true); }
          else             { bt_stop();  bt_conf_save(false); }
          return true;
        },
        []() { return bt_state != PROV_BT_STATE_OFF && bt_state != PROV_BT_STATE_NA; })
      // Write true to open the pairing window. Not a stored setting: it is a
      // momentary action, and it reads back false because the window closes
      // itself after BT_PAIRING_TIMEOUT.
      .field_bool("Pair Now", PROV_BT_PAIR, FF_LIVE_APPLY, false,
        [](const Value& v) {
          if (!v.as_bool()) { bt_disable_pairing(); return true; }
          if (bt_state == PROV_BT_STATE_OFF) { bt_start(); bt_conf_save(true); }
          if (bt_state != PROV_BT_STATE_CONNECTED) { bt_enable_pairing(); }
          return true;
        },
        []() { return bt_state == PROV_BT_STATE_PAIRING; })
      .metric_int("State", PROV_BT_STATE,
        []() { return static_cast<fint_t>(bt_state); })
      .metric_int("Pairing Passkey", PROV_BT_PASSKEY,
        []() { return static_cast<fint_t>(pairing_pin); })
      .metric_string("Device Name", PROV_BT_DEVNAME,
        []() { return bt_devname; })
      // Whether a bond is actually stored. A phone reporting "not ready to
      // pair" on reconnect looks identical whether the node has forgotten the
      // bond or is refusing a peer it remembers, and those have opposite
      // fixes. This is the number that tells them apart.
      .metric_int("Bonded Peers", PROV_BT_BONDS,
        []() { return static_cast<fint_t>(bt_bonded_peer_count()); })
      .end();
#endif

  // ----- Ephemeral RRC group-chat hub -----
  // Configuration is persisted and applied on reboot because the Destination,
  // bounded state engine and timers are constructed during RNS startup.
  Provisioner::instance()
    .register_namespace("RRC Hub", PROV_NS_RRC)
      .field_bool("Enabled", PROV_RRC_ENABLED, FF_REBOOT_REQUIRED, rrc_hub_enabled,
        [](const Value& v) { rrc_hub_enabled = v.as_bool(); return true; },
        []() { return rrc_hub_enabled; })
      .field_string("Hub Name", PROV_RRC_NAME, FF_REBOOT_REQUIRED,
        rrc_hub_name, sizeof(rrc_hub_name)-1,
        [](const Value& v) {
          const std::string& name = v.as_string();
          if (name.empty() || name.size() >= sizeof(rrc_hub_name)) return false;
          snprintf(rrc_hub_name, sizeof(rrc_hub_name), "%s", name.c_str());
          return true;
        },
        []() { return rrc_hub_name; })
      .field_int("Announce Interval (s)", PROV_RRC_ANNOUNCE_INTERVAL,
        FF_REBOOT_REQUIRED, rrc_hub_announce_interval_seconds, 60, 86400,
        [](const Value& v) {
          rrc_hub_announce_interval_seconds = static_cast<uint32_t>(v.as_int());
          return true;
        }, []() { return static_cast<fint_t>(rrc_hub_announce_interval_seconds); })
      .field_int("Max Sessions", PROV_RRC_MAX_SESSIONS,
        FF_REBOOT_REQUIRED, rrc_hub_max_sessions, 1, 8,
        [](const Value& v) {
          rrc_hub_max_sessions = static_cast<uint8_t>(v.as_int()); return true;
        }, []() { return static_cast<fint_t>(rrc_hub_max_sessions); })
      .field_int("Max Rooms / Session", PROV_RRC_MAX_ROOMS_SESSION,
        FF_REBOOT_REQUIRED, rrc_hub_max_rooms_per_session, 1, 8,
        [](const Value& v) {
          rrc_hub_max_rooms_per_session = static_cast<uint8_t>(v.as_int());
          return true;
        }, []() { return static_cast<fint_t>(rrc_hub_max_rooms_per_session); })
      .field_int("Max Body Bytes", PROV_RRC_MAX_BODY_BYTES,
        FF_REBOOT_REQUIRED, rrc_hub_max_body_bytes, 1, 280,
        [](const Value& v) {
          rrc_hub_max_body_bytes = static_cast<uint16_t>(v.as_int()); return true;
        }, []() { return static_cast<fint_t>(rrc_hub_max_body_bytes); })
      .field_int("Rate / Minute", PROV_RRC_RATE_PER_MINUTE,
        FF_REBOOT_REQUIRED, rrc_hub_rate_per_minute, 1, 600,
        [](const Value& v) {
          rrc_hub_rate_per_minute = static_cast<uint16_t>(v.as_int()); return true;
        }, []() { return static_cast<fint_t>(rrc_hub_rate_per_minute); })
      .field_int("PING Interval (s)", PROV_RRC_PING_INTERVAL,
        FF_REBOOT_REQUIRED, rrc_hub_ping_interval_seconds, 10, 3600,
        [](const Value& v) {
          rrc_hub_ping_interval_seconds = static_cast<uint32_t>(v.as_int());
          return true;
        }, []() { return static_cast<fint_t>(rrc_hub_ping_interval_seconds); })
      .field_int("PONG Timeout (s)", PROV_RRC_PONG_TIMEOUT,
        FF_REBOOT_REQUIRED, rrc_hub_pong_timeout_seconds, 5, 600,
        [](const Value& v) {
          rrc_hub_pong_timeout_seconds = static_cast<uint32_t>(v.as_int());
          return true;
        }, []() { return static_cast<fint_t>(rrc_hub_pong_timeout_seconds); })
#if defined(RRC_LXMF_BRIDGE)
      // Which rooms outlive a member's connection. Ad-hoc rooms stay ephemeral;
      // these are mirrored to LXMF so a responder who was out of range receives
      // them on their next sync. See docs/RRCRequirements.md 12c.
      .field_bool("Bridge to LXMF", PROV_RRC_BRIDGE_ENABLED, FF_REBOOT_REQUIRED,
        rrc_bridge_enabled,
        [](const Value& v) { rrc_bridge_enabled = v.as_bool(); return true; },
        []() { return rrc_bridge_enabled; })
      .field_string("Bridged Rooms", PROV_RRC_BRIDGE_ROOMS, FF_REBOOT_REQUIRED,
        rrc_bridge_rooms, sizeof(rrc_bridge_rooms)-1,
        [](const Value& v) {
          const std::string& list = v.as_string();
          if (list.size() >= sizeof(rrc_bridge_rooms)) return false;
          snprintf(rrc_bridge_rooms, sizeof(rrc_bridge_rooms), "%s", list.c_str());
          return true;
        },
        []() { return rrc_bridge_rooms; })
      // Lines of catch-up a member gets on first joining a bridged room. This
      // is the difference between a command room someone can walk into and one
      // that only works if you were already there. 0 disables it.
      .field_int("Join History Lines", PROV_RRC_BRIDGE_HISTORY,
        FF_REBOOT_REQUIRED, rrc_bridge_history, 0, RRC_BRIDGE_HISTORY_MAX,
        [](const Value& v) {
          rrc_bridge_history = static_cast<uint8_t>(v.as_int()); return true;
        }, []() { return static_cast<fint_t>(rrc_bridge_history); })
#endif
      .metric_bytes("Destination", PROV_RRC_DESTINATION,
        []() { return rrc_hub_destination_hash(); })
      .metric_bool("Running", PROV_RRC_RUNNING,
        []() { return rrc_hub_running(); })
      .metric_int("Sessions", PROV_RRC_SESSIONS,
        []() { return static_cast<fint_t>(rrc_hub_session_count()); })
      .metric_int("Identified", PROV_RRC_IDENTIFIED,
        []() { return static_cast<fint_t>(rrc_hub_identified_count()); })
      .metric_int("Rooms", PROV_RRC_ROOMS,
        []() { return static_cast<fint_t>(rrc_hub_room_count()); })
      .metric_int("Memberships", PROV_RRC_MEMBERSHIPS,
        []() { return static_cast<fint_t>(rrc_hub_membership_count()); })
      .metric_int("Packets RX", PROV_RRC_RX,
        []() { return static_cast<fint_t>(rrc_hub_rx_count()); })
      .metric_int("Packets TX", PROV_RRC_TX,
        []() { return static_cast<fint_t>(rrc_hub_tx_count()); })
      .metric_int("Accepted", PROV_RRC_ACCEPTED,
        []() { return static_cast<fint_t>(rrc_hub_accepted_count()); })
      .metric_int("Forwarded", PROV_RRC_FORWARDED,
        []() { return static_cast<fint_t>(rrc_hub_forwarded_count()); })
      .metric_int("Rejected", PROV_RRC_REJECTED,
        []() { return static_cast<fint_t>(rrc_hub_rejected_count()); })
      .metric_int("Rate Limited", PROV_RRC_RATE_LIMITED,
        []() { return static_cast<fint_t>(rrc_hub_rate_limited_count()); })
      .metric_int("Malformed", PROV_RRC_MALFORMED,
        []() { return static_cast<fint_t>(rrc_hub_malformed_count()); })
      .metric_int("Identify Timeouts", PROV_RRC_IDENT_TIMEOUTS,
        []() { return static_cast<fint_t>(rrc_hub_identify_timeout_count()); })
      .metric_int("HELLO Timeouts", PROV_RRC_HELLO_TIMEOUTS,
        []() { return static_cast<fint_t>(rrc_hub_hello_timeout_count()); })
      .metric_int("PONG Timeouts", PROV_RRC_PONG_TIMEOUTS,
        []() { return static_cast<fint_t>(rrc_hub_pong_timeout_count()); })
#if defined(RRC_LXMF_BRIDGE)
      // Roster size is the one to watch: a bridged room with no roster
      // delivers nothing and looks identical to a healthy idle one.
      .metric_int("Bridge Roster", PROV_RRC_BRIDGE_MEMBERS,
        []() { return static_cast<fint_t>(rrc_bridge_member_count()); })
      .metric_int("Bridge Queued", PROV_RRC_BRIDGE_QUEUED,
        []() { return static_cast<fint_t>(rrc_bridge_queue_depth()); })
      .metric_int("Bridge Delivered", PROV_RRC_BRIDGE_DELIVERED,
        []() { return static_cast<fint_t>(rrc_bridge_delivered_count()); })
      .metric_int("Bridge Dropped", PROV_RRC_BRIDGE_DROPPED,
        []() { return static_cast<fint_t>(rrc_bridge_dropped_count()); })
      .metric_int("Bridge History Depth", PROV_RRC_BRIDGE_HISTDEPTH,
        []() { return static_cast<fint_t>(rrc_bridge_history_depth()); })
#endif
      .end();
#endif

#ifdef HAS_GPIO
    general
/*
      .field_bool("Relay Enabled", PROV_GENERAL_GPIO0, FF_LIVE_APPLY, false,
        [](const Value& v) { v.as_bool() ? GPIO::setState(GPIO::GPIO0, GPIO::STATE_HIGH) : GPIO::setState(GPIO::GPIO0, GPIO::STATE_LOW); return true; },
        []() { return GPIO::isHigh(GPIO::GPIO0); })
      .metric_bool("Device Ready", PROV_GENERAL_GPIO1,
        []() { return GPIO::isHigh(GPIO::GPIO1); });
*/
      .field_enum("GPIO0", PROV_GENERAL_GPIO0, FF_LIVE_APPLY, GPIO::DISP_INPUT,
          {
            GPIO::DISP_INPUT,
            GPIO::DISP_INPUT_LOW,
            GPIO::DISP_INPUT_HIGH,
            GPIO::DISP_OUTPUT_LOW,
            GPIO::DISP_OUTPUT_HIGH,
          },
          {
            "INPUT",
            "INPUT_LOW",
            "INPUT_HIGH",
            "OUTPUT_LOW",
            "OUTPUT_HIGH",
          },
          [](const Value& v) {
            GPIO::setDisposition(GPIO::GPIO0, static_cast<GPIO::Disposition>(v.as_int())); return true;
          },
          []() {
            return static_cast<fint_t>(GPIO::getDisposition(GPIO::GPIO0));
          }
        )
      .field_enum("GPIO1", PROV_GENERAL_GPIO1, FF_LIVE_APPLY, GPIO::DISP_INPUT,
          {
            GPIO::DISP_INPUT,
            GPIO::DISP_INPUT_LOW,
            GPIO::DISP_INPUT_HIGH,
            GPIO::DISP_OUTPUT_LOW,
            GPIO::DISP_OUTPUT_HIGH,
          },
          {
            "INPUT",
            "INPUT_LOW",
            "INPUT_HIGH",
            "OUTPUT_LOW",
            "OUTPUT_HIGH",
          },
          [](const Value& v) {
            GPIO::setDisposition(GPIO::GPIO1, static_cast<GPIO::Disposition>(v.as_int())); return true;
          },
          []() {
            return static_cast<fint_t>(GPIO::getDisposition(GPIO::GPIO1));
          }
        );
#endif

#if defined(LORA_TRANSPORT)
  if (lora_interface) {
    general
      .field_enum(
          "LoRa Interface Mode", PROV_GENERAL_LORA_MODE, FF_LIVE_APPLY, static_cast<fint_t>(lora_interface.mode()),
          /* values   */ {
            RNS::Type::Interface::MODE_GATEWAY,
            RNS::Type::Interface::MODE_FULL,
            RNS::Type::Interface::MODE_POINT_TO_POINT,
            RNS::Type::Interface::MODE_ACCESS_POINT,
            RNS::Type::Interface::MODE_ROAMING,
            RNS::Type::Interface::MODE_BOUNDARY,
          },
          /* labels   */ {
            "gateway",
            "full",
            "point-to-point",
            "access-point",
            "roaming",
            "boundary" },
          /* setter   */ [](const Value& v) {
            lora_interface.mode(static_cast<RNS::Type::Interface::modes>(v.as_int())); return true;
          },
          /* getter   */ []() {
            return static_cast<fint_t>(lora_interface.mode());
          }
      );
  }
#endif

#if defined(UDP_TRANSPORT)
  if (udp_interface) {
    general
      .field_enum(
          "UDP Interface Mode", PROV_GENERAL_UDP_MODE, FF_LIVE_APPLY, static_cast<fint_t>(udp_interface.mode()),
          /* values   */ {
            RNS::Type::Interface::MODE_GATEWAY,
            RNS::Type::Interface::MODE_FULL,
            RNS::Type::Interface::MODE_POINT_TO_POINT,
            RNS::Type::Interface::MODE_ACCESS_POINT,
            RNS::Type::Interface::MODE_ROAMING,
            RNS::Type::Interface::MODE_BOUNDARY,
          },
          /* labels   */ {
            "gateway",
            "full",
            "point-to-point",
            "access-point",
            "roaming",
            "boundary" },
          /* setter   */ [](const Value& v) {
            udp_interface.mode(static_cast<RNS::Type::Interface::modes>(v.as_int())); return true;
          },
          /* getter   */ []() {
            return static_cast<fint_t>(udp_interface.mode());
          }
      );
  }
#endif

  general
    .end();   // close "General"

#if defined(LORA_TRANSPORT)
  // ----- LoRa access-control namespace -----
  // Changes are deliberately reboot-required. This prevents the two ends of
  // a live radio path from changing key state midway through a Provisioning
  // exchange and stranding the response.
  Provisioner::instance()
    .register_namespace("LoRa Access Control", PROV_NS_IFAC_LORA)
      .field_bool("Enabled", PROV_IFAC_LORA_ENABLED, FF_REBOOT_REQUIRED,
        false,
        [](const Value& v) { lora_ifac_enabled = v.as_bool(); return true; },
        []() { return lora_ifac_enabled; })
      .field_string("Network Name", PROV_IFAC_LORA_NETNAME,
        FF_REBOOT_REQUIRED, "", 64,
        [](const Value& v) { lora_ifac_netname = v.as_string(); return true; },
        []() { return lora_ifac_netname; })
      .field_string("Passphrase", PROV_IFAC_LORA_PASSPHRASE,
        (fflags_t)(FF_REBOOT_REQUIRED | FF_SECRET), "", 128,
        [](const Value& v) { lora_ifac_passphrase = v.as_string(); return true; },
        []() { return lora_ifac_passphrase; })
      .on_commit([](Namespace& ns) {
        validate_ifac_commit(ns, "LoRa");
      })
      .end();
#endif

#if defined(TCP_SERVER_TRANSPORT)
  // ----- TCP RNS access-control namespace -----
  Provisioner::instance()
    .register_namespace("TCP Access Control", PROV_NS_IFAC_TCP)
      .field_bool("Enabled", PROV_IFAC_TCP_ENABLED, FF_REBOOT_REQUIRED,
        false,
        [](const Value& v) { tcp_ifac_enabled = v.as_bool(); return true; },
        []() { return tcp_ifac_enabled; })
      .field_string("Network Name", PROV_IFAC_TCP_NETNAME,
        FF_REBOOT_REQUIRED, "", 64,
        [](const Value& v) { tcp_ifac_netname = v.as_string(); return true; },
        []() { return tcp_ifac_netname; })
      .field_string("Passphrase", PROV_IFAC_TCP_PASSPHRASE,
        (fflags_t)(FF_REBOOT_REQUIRED | FF_SECRET), "", 128,
        [](const Value& v) { tcp_ifac_passphrase = v.as_string(); return true; },
        []() { return tcp_ifac_passphrase; })
      .on_commit([](Namespace& ns) { validate_ifac_commit(ns, "TCP"); })
      .end();
#endif

#if defined(UDP_TRANSPORT)
  // ----- UDP RNS access-control namespace -----
  Provisioner::instance()
    .register_namespace("UDP Access Control", PROV_NS_IFAC_UDP)
      .field_bool("Enabled", PROV_IFAC_UDP_ENABLED, FF_REBOOT_REQUIRED,
        false,
        [](const Value& v) { udp_ifac_enabled = v.as_bool(); return true; },
        []() { return udp_ifac_enabled; })
      .field_string("Network Name", PROV_IFAC_UDP_NETNAME,
        FF_REBOOT_REQUIRED, "", 64,
        [](const Value& v) { udp_ifac_netname = v.as_string(); return true; },
        []() { return udp_ifac_netname; })
      .field_string("Passphrase", PROV_IFAC_UDP_PASSPHRASE,
        (fflags_t)(FF_REBOOT_REQUIRED | FF_SECRET), "", 128,
        [](const Value& v) { udp_ifac_passphrase = v.as_string(); return true; },
        []() { return udp_ifac_passphrase; })
      .on_commit([](Namespace& ns) { validate_ifac_commit(ns, "UDP"); })
      .end();
#endif

  // One switch controls all non-RNS wireless management surfaces. It is
  // intentionally not split into independent toggles: a partially closed
  // posture is too easy to mistake for a private node.
  Provisioner::instance()
    .register_namespace("Secure Node", PROV_NS_SECURE_NODE)
      .field_bool("Enabled", PROV_SECURE_NODE_ENABLED, FF_REBOOT_REQUIRED,
        false,
        [](const Value& v) { secure_node_enabled = v.as_bool(); return true; },
        []() { return secure_node_enabled; })
      .end();

  // ----- Metrics namespace -----
  //
  // The Metrics > Interfaces parent chain is opened unconditionally; the
  // per-interface child namespaces are added only when the corresponding
  // interface object reports it has a live implementation (operator bool
  // on RNS::Interface). Compile-time guards remain only where they need
  // to — UDP's externs aren't declared without UDP_TRANSPORT.
  auto metrics = Provisioner::instance().register_namespace("RNode General Metrics", PROV_NS_METRICS);

  metrics.register_namespace("Device", PROV_NS_METRICS_DEV)
    //.metric_string("transport_identity", PROV_METRICS_DEV_VER, []() { return std::to_string(MAJ_VERS) + "." + std::to_string(MIN_VERS); })
#if MCU_VARIANT == MCU_ESP32
    // Only where it can actually be sampled. loop_stack_free_min keeps its
    // "nothing seen yet" sentinel on platforms without
    // uxTaskGetStackHighWaterMark(), and reporting that sentinel would show a
    // 4 GB free stack -- worse than absent for a metric whose entire purpose is
    // to be trusted. Note the sentinel itself must stay at its maximum value:
    // the running minimum is computed with `<`, so seeding it at 0 would freeze
    // the metric at 0 on the platforms where it does work.
    .metric_int("Loop Stack Free Min", PROV_METRICS_DEV_STACK,
      []() { return (fint_t)loop_stack_free_min; })
    // Memory, over provisioning rather than only on the console.
    //
    // The periodic [heap] line goes to whichever KISS host is attached, so on
    // a node with a client connected -- the normal state for a deployed board
    // -- it is not visible on the USB console at all. That is exactly when
    // memory matters: Rev 1 was found sliding to 3% free and restarting under
    // a client it was serving, and the console said nothing because the client
    // was receiving the evidence.
    //
    // Internal free and the largest internal block are reported separately
    // because they fail differently: a healthy total with a small largest
    // block is fragmentation, which capping tables does not fix. PSRAM free is
    // here to show whether the spill threshold is doing anything at all --
    // 8 MB sitting idle beside an exhausted internal heap is the signature
    // this board has already produced once.
    .metric_int("Heap Internal Free", PROV_METRICS_DEV_HEAP,
      []() { return (fint_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL); })
    .metric_int("Heap Largest Block", PROV_METRICS_DEV_HEAPBLK,
      []() { return (fint_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL); })
    .metric_int("PSRAM Free", PROV_METRICS_DEV_PSRAM,
      []() { return (fint_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM); })
    // Uptime makes a restart visible to anything polling metrics. Without it a
    // board that reboots between two polls is indistinguishable from one that
    // stayed up, which is how Rev 1's restarts went unnoticed for so long.
    .metric_int("Uptime Seconds", PROV_METRICS_DEV_UPTIME,
      []() { return (fint_t)(millis() / 1000); })
#endif
    .metric_float("Battery Voltage", PROV_METRICS_DEV_BATV, []() { return battery_voltage; })
    .metric_float("Battery Percent", PROV_METRICS_DEV_BATP, []() { return battery_percent; })
/*
    .metric_string("Battery State", PROV_METRICS_DEV_BATS, []() {
      switch (battery_state) {
        case BATTERY_STATE_CHARGING:
          return "CHARGING";
        case BATTERY_STATE_CHARGED:
          return "CHARGED";
        case BATTERY_STATE_DISCHARGING:
          return "DISCHARGING";
        case BATTERY_STATE_UNKNOWN:
          return "UNKNOWN";
        return "";
      }
    })
*/
    .end();

  metrics.register_namespace("Addresses", PROV_NS_METRICS_ADDRS)
    .metric_bytes("Transport Identity", PROV_METRICS_TRANS_ID, []() { return RNS::Transport::identity() ? RNS::Transport::identity().hash() : RNS::Bytes{}; })
    .metric_bytes("Probe Destination", PROV_METRICS_PROBE_DST, []() { return RNS::Transport::probe_destination() ? RNS::Transport::probe_destination().hash() : RNS::Bytes{}; })
    .metric_bytes("Mgmt Destination", PROV_METRICS_MGMT_DST, []() { return RNS::Transport::remote_management_destination() ? RNS::Transport::remote_management_destination().hash() : RNS::Bytes{}; })
    .metric_bytes("NomadNet Destination", PROV_METRICS_NOMAD_DST, []() { return nomadnet_destination ? nomadnet_destination.hash() : RNS::Bytes{}; })
    .end();

  auto metrics_ifaces = metrics.register_namespace("Interfaces", PROV_NS_METRICS_IFACE);
#if defined(LORA_TRANSPORT)
  if (lora_interface) {
    metrics_ifaces
      //.register_namespace("LoRa", PROV_NS_IFACE_LORA)
      .register_namespace(lora_interface.name().c_str(), PROV_NS_IFACE_LORA)
        .metric_int("Frequency", PROV_METRICS_LORA_FREQ, []() { return lora_freq; })
        .metric_int("Bandwidth", PROV_METRICS_LORA_BW, []() { return lora_bw; })
        .metric_int("Spreading Factor", PROV_METRICS_LORA_SF, []() { return lora_sf; })
        .metric_int("Coding Rate", PROV_METRICS_LORA_CR, []() { return lora_cr; })
        .metric_int("TX Power", PROV_METRICS_LORA_TXP, []() { return lora_txp; })
        //.metric_int("Current RSSI", PROV_METRICS_LORA_CRSSI, []() { return last_rssi+rssi_offset; })
        .metric_int("Current RSSI", PROV_METRICS_LORA_CRSSI, []() { return current_rssi; })
        .metric_int("Noise Floor", PROV_METRICS_LORA_NF, []() { return (uint16_t)noise_floor; })
        .metric_int("Last RSSI", PROV_METRICS_LORA_LRSSI, []() { return (uint16_t)last_rssi; })
        .metric_int("Last SNR", PROV_METRICS_LORA_LSNR, []() { return (uint16_t)((int8_t)last_snr_raw) / 4.0f; })
        .metric_float("ST Airtime Limit", PROV_METRICS_LORA_STAL, []() { return st_airtime_limit; })
        .metric_float("LT Airtime Limit", PROV_METRICS_LORA_LTAL, []() { return lt_airtime_limit; })
        .end();
  }
#endif
#if defined(UDP_TRANSPORT)
  if (udp_interface) {
    metrics_ifaces
      //.register_namespace("UDP", PROV_NS_IFACE_UDP)
      .register_namespace(udp_interface.name().c_str(), PROV_NS_IFACE_UDP)
        .metric_string("ip_addr", PROV_METRICS_UDP_ADDR, []() { return wr_device_ip.toString().c_str(); })
        .metric_int("udp_port", PROV_METRICS_UDP_PORT, []() { return udp_port; })
        .metric_string("wifi_ssid", PROV_METRICS_WIFI_SSID, []() { return wr_ssid; })
        .end();
  }
#endif
  metrics_ifaces.end(); // close "Interfaces"

  metrics.end();        // close "Metrics"

#if defined(LORA_TRANSPORT)
  // ----- Radio namespace -----
  //
  // Preset enum lists, derived from RADIO_PRESETS so the table in
  // RadioPresets.h stays the single source of truth. "Custom" is offered as a
  // reportable value only -- radio_preset_apply() rejects it, because "custom"
  // describes a configuration rather than selecting one.
  std::vector<fint_t> prov_preset_values;
  std::vector<std::string> prov_preset_labels;
  for (uint8_t i = 0; i < RADIO_PRESET_COUNT; i++) {
    prov_preset_values.push_back((fint_t)i);
    prov_preset_labels.push_back(RADIO_PRESETS[i].name);
  }
  prov_preset_values.push_back((fint_t)RADIO_PRESET_CUSTOM);
  prov_preset_labels.push_back("Custom");

  Provisioner::instance()
    .register_namespace("RNode Radio Config", PROV_NS_RADIO)
      // Named preset covering bandwidth/SF/CR together. Registered first so it
      // reads as the primary control: the individual fields below remain for
      // deliberate off-ladder work, but a fleet should agree by preset name.
      //
      // FF_LIVE_APPLY, not FF_REBOOT_REQUIRED. The reboot-required fields below
      // are applied by this namespace's on_commit hook, which reads their
      // drafts by name and knows nothing about this one -- so a reboot-required
      // preset would be stored and silently never applied. Live apply runs the
      // setter on commit, and the consistency watch in loop() then reprograms
      // the modem and arms the commit-confirm rollback.
      //
      // Note this field reports the value last *written*, like every other
      // config field. The authoritative live view is "preset" on
      // /page/device.mu, which is computed from the running radio and says
      // "Custom" whenever bandwidth/SF/CR were edited individually.
      .field_enum("Radio Preset", PROV_RADIO_PRESET, FF_LIVE_APPLY,
                 (fint_t)radio_preset_current(),
                 prov_preset_values, prov_preset_labels,
                 [](const Value& v) { return radio_preset_apply((uint8_t)v.as_int()); })
      // Sets the mode adopted at boot once a radio config exists. Writes
      // prov_op_mode rather than op_mode directly: op_mode is recomputed during
      // validate_status(), so assigning it here would be discarded on reboot.
      .field_enum("op_mode", PROV_RADIO_OP_MODE, FF_REBOOT_REQUIRED,
                 (fint_t)prov_op_mode,
                 std::vector<fint_t>{ (fint_t)MODE_HOST, (fint_t)MODE_TNC },
                 std::vector<std::string>{ "host", "tnc" },
                 [](const Value& v) { prov_op_mode = (uint8_t)v.as_int(); return true; })
      .field_int("Frequency", PROV_RADIO_FREQ, FF_REBOOT_REQUIRED,
        (fint_t)lora_freq, (fint_t)100000000, (fint_t)1000000000,
        [](const Value& v) { lora_freq = (uint32_t)v.as_int(); return true; })
      .field_int("Bandwidth", PROV_RADIO_BW, FF_REBOOT_REQUIRED,
        (fint_t)lora_bw, (fint_t)7800, (fint_t)500000,
        [](const Value& v) { lora_bw = (uint32_t)v.as_int(); return true; })
      .field_int("Spreading Factor", PROV_RADIO_SF, FF_REBOOT_REQUIRED,
        (fint_t)lora_sf, (fint_t)5, (fint_t)12,
        [](const Value& v) { lora_sf = (int)v.as_int(); return true; })
      .field_int("Coding Rate", PROV_RADIO_CR, FF_REBOOT_REQUIRED,
        (fint_t)lora_cr, (fint_t)5, (fint_t)8,
        [](const Value& v) { lora_cr = (int)v.as_int(); return true; })
      .field_int("TX Power", PROV_RADIO_TXP, FF_REBOOT_REQUIRED,
        (fint_t)lora_txp, (fint_t)-9, (fint_t)22,
        [](const Value& v) { lora_txp = (int)v.as_int(); return true; })
      .field_int("Implicit Length", PROV_RADIO_IMPLICIT, FF_REBOOT_REQUIRED,
        (fint_t)implicit_l, (fint_t)0, (fint_t)255,
        [](const Value& v) { implicit_l = (uint8_t)v.as_int(); return true; })
      .field_float("ST Airtime Limit", PROV_RADIO_STAL, FF_LIVE_APPLY,
        (ffloat_t)st_airtime_limit, (ffloat_t)0, (ffloat_t)1.0,
        [](const Value& v) { st_airtime_limit = (float)v.as_float(); return true; })
      .field_float("LT Airtime Limit", PROV_RADIO_LTAL, FF_LIVE_APPLY,
        (ffloat_t)lt_airtime_limit, (ffloat_t)0, (ffloat_t)1.0,
        [](const Value& v) { lt_airtime_limit = (float)v.as_float(); return true; })
      .on_commit([](Namespace& ns) {
        //TRACE("[provision] Radio commit\n");
        Value v;
        bool dirty = false;
        if (ns.draft(PROV_RADIO_FREQ, v)) {
          lora_freq = (uint32_t)v.as_int();
          //ns.clear_draft(PROV_RADIO_FREQ);
          dirty = true;
        }
        if (ns.draft(PROV_RADIO_BW, v)) {
          lora_bw = (uint32_t)v.as_int();
          //ns.clear_draft(PROV_RADIO_BW);
          dirty = true;
        }
        if (ns.draft(PROV_RADIO_SF, v)) {
          lora_sf = (uint32_t)v.as_int();
          //ns.clear_draft(PROV_RADIO_SF);
          dirty = true;
        }
        if (ns.draft(PROV_RADIO_CR, v)) {
          lora_cr = (uint32_t)v.as_int();
          //ns.clear_draft(PROV_RADIO_CR);
          dirty = true;
        }
        if (ns.draft(PROV_RADIO_TXP, v)) {
          lora_txp = (uint32_t)v.as_int();
          //ns.clear_draft(PROV_RADIO_TXP);
          dirty = true;
        }
        if (dirty) {
          //TRACE("[provision] Writing eeprom\n");
          eeprom_conf_save();
        }
      })
      .end();
#endif

#if defined(UDP_TRANSPORT)
  //if (wifi_mode != WR_WIFI_OFF && udp_interface) {
    Provisioner::instance()
      .register_namespace("RNode Network Config", PROV_NS_NETWORK)
        // Read-only on purpose. The setter here was commented out, so the
        // field accepted an address, answered "reboot required" and discarded
        // it. Reporting the current address honestly is better than pretending
        // it can be set; static addressing is configured over KISS
        // (CMD_CONF_IP) and belongs in a deliberate change, not a stub.
        .metric_string("IP Address", PROV_NET_IP,
          []() { return std::string(wr_device_ip.toString().c_str()); })
        .field_int("UDP Port", PROV_NET_PORT, FF_REBOOT_REQUIRED,
          (fint_t)udp_port, (fint_t)1024, (fint_t)65535,
          [](const Value& v) { udp_port = (uint32_t)v.as_int(); return true; })
        // Persisted to EEPROM, not just to RAM. wifi_remote_init() reloads
        // wr_ssid from EEPROM on every boot, so a setter that touched only the
        // variable accepted the value, reported "reboot required", and then
        // silently reverted -- the field looked settable and was not. This is
        // the same EEPROM path the KISS CMD_CONF_SSID command already uses.
        .field_string("WiFi SSID", PROV_NET_SSID, FF_REBOOT_REQUIRED,
          wr_ssid, 32,
          [](const Value& v) {
            const std::string ssid = v.as_string();
            if (ssid.size() > 32) return false;
            wifi_persist_ssid(ssid.c_str());
            return true;
          })
        // Likewise read-only: the setter accepted anything and did nothing.
        .metric_string("WiFi Mode", PROV_NET_MODE,
          []() { return std::to_string(wifi_mode); })
// Guarded on HAS_WIFI alone. Config.h cannot be included in this translation
        // unit -- its macros collide with the Provisioning headers -- so
        // WIFI_AP_FALLBACK_MS is not visible here. Testing an invisible macro is
        // how these fields silently disappeared the first time.
#if HAS_WIFI == true
        // FF_LIVE_APPLY: these are read on each pass of the fallback state
        // machine, so a change takes effect without a reboot. That matters --
        // the operator most likely to adjust them is standing in a building
        // whose network has just failed, which is the worst moment to ask a
        // node to restart.
        //
        // Lower bounds are deliberate. A fallback delay under 30 s deserts a
        // router that is merely rebooting, and a retry interval under a minute
        // means a node serving residents keeps dropping its AP to go looking
        // for an uplink.
        .field_int("AP Fallback Delay (s)", PROV_NET_AP_FALLBACK_S, FF_LIVE_APPLY,
          (fint_t)(wifi_ap_fallback_ms / 1000), (fint_t)30, (fint_t)3600,
          [](const Value& v) { wifi_ap_fallback_ms = (uint32_t)v.as_int() * 1000UL; return true; },
          []() { return (fint_t)(wifi_ap_fallback_ms / 1000); })
        .field_int("AP Station Retry (s)", PROV_NET_AP_RETRY_S, FF_LIVE_APPLY,
          (fint_t)(wifi_ap_retry_sta_ms / 1000), (fint_t)60, (fint_t)86400,
          [](const Value& v) { wifi_ap_retry_sta_ms = (uint32_t)v.as_int() * 1000UL; return true; },
          []() { return (fint_t)(wifi_ap_retry_sta_ms / 1000); })
        .field_int("AP Max Defer (s)", PROV_NET_AP_MAX_DEFER_S, FF_LIVE_APPLY,
          (fint_t)(wifi_ap_max_defer_ms / 1000), (fint_t)60, (fint_t)86400,
          [](const Value& v) { wifi_ap_max_defer_ms = (uint32_t)v.as_int() * 1000UL; return true; },
          []() { return (fint_t)(wifi_ap_max_defer_ms / 1000); })
        // Read-only state. Without these an operator cannot tell whether a node
        // is on the building network or serving its own, which is the first
        // question to ask about a node that has gone quiet.
        .metric_int("AP Fallback Active", PROV_NET_AP_ACTIVE,
          []() { return (fint_t)(wifi_ap_fallback_active ? 1 : 0); })
        .metric_int("AP Clients", PROV_NET_AP_CLIENTS,
          []() { return (fint_t)wifi_ap_client_count; })
        .metric_string("AP SSID", PROV_NET_AP_SSID,
          []() { return wifi_ap_fallback_active ? std::string(wifi_ap_ssid) : std::string(); })
#endif
      .end();
  //}
#endif

}

// ---------------------------------------------------------------------------
// Bring the Provisioning subsystem up. Loads any persisted MsgPack files
// under /config (built-in Reticulum / Transport namespaces auto-register
// inside begin(); our general namespace is registered above). The
// on_reboot_required callback is wired up but intentionally a no-op —
// the host orchestrates reboots via CMD_RESET.
// ---------------------------------------------------------------------------
void init_provisioning() {
  RNS::Provisioning::Provisioner::instance().on_factory_reset([]() {
    apply_all_ifac_configuration();
    apply_secure_node_configuration();
  });
  RNS::Provisioning::Provisioner::instance().on_reboot_required([]() {
    // Host orchestrates reboot via CMD_RESET. Provisioner::needs_reboot()
    // remains queryable via GetInfo for callers that want to surface
    // pending-reboot state.
  });
  RNS::Provisioning::Provisioner::instance().on_reboot([]() {
    hard_reset();
  });
  register_provisioning_namespaces();
  RNS::Provisioning::Provisioner::instance().begin();
  // begin() has loaded persistent state and replayed its setters. Apply the
  // complete tuples once, before interfaces are registered with Transport.
  apply_all_ifac_configuration();
  apply_secure_node_configuration();
  provisioning_started = true;
}

// ---------------------------------------------------------------------------
// Request / response over KISS
// ---------------------------------------------------------------------------
void on_provision_request(const RNS::Bytes& req) {
  if (!provisioning_started) return;
  RNS::Bytes response = RNS::Provisioning::Provisioner::instance().handle_message(req);
  kiss_indicate_provision_response(response);
}

void kiss_indicate_provision_response(const RNS::Bytes& payload) {
  serial_write(FEND);
  serial_write(CMD_PROVISION_RSP);
  const uint8_t* data = payload.data();
  size_t n = payload.size();
  for (size_t i = 0; i < n; ++i) escaped_serial_write(data[i]);
  serial_write(FEND);
}

// Push the live radio configuration back into the provisioning store.
//
// Needed when the firmware changes the radio behind provisioning's back. The
// only case today is the commit-confirm rollback in RNode_Firmware.ino, which
// restores the last known-good PHY after a change strands the link. Without
// this the store would keep advertising -- and, because these fields are
// re-applied from storage at boot, keep restoring -- the very configuration
// that broke the link, so the node would strand itself again on next reboot.
//
// Deliberately routed through the normal draft+commit path so the registered
// commit hook runs and the values are persisted exactly as an operator-issued
// change would be.
void provisioning_sync_radio_from_runtime() {
  auto& prov = RNS::Provisioning::Provisioner::instance();
  if (!prov.started()) return;
  prov.field(PROV_NS_RADIO, PROV_RADIO_FREQ, RNS::Provisioning::Value((int)lora_freq));
  prov.field(PROV_NS_RADIO, PROV_RADIO_BW, RNS::Provisioning::Value((int)lora_bw));
  prov.field(PROV_NS_RADIO, PROV_RADIO_SF, RNS::Provisioning::Value((int)lora_sf));
  prov.field(PROV_NS_RADIO, PROV_RADIO_CR, RNS::Provisioning::Value((int)lora_cr));
  prov.field(PROV_NS_RADIO, PROV_RADIO_TXP, RNS::Provisioning::Value((int)lora_txp));
  prov.commit(PROV_NS_RADIO);
}

#endif // HAS_PROVISIONING
