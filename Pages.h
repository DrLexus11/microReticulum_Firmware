// Copyright (C) 2026, Chad Attermann

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

#pragma once

#include <Arduino.h>

#include "Config.h"

#include <microReticulum/Interface.h>
#include <microReticulum/Identity.h>
#include <microReticulum/Utilities/OS.h>
#include <microReticulum/Bytes.h>
// The clock page reports how time is being distributed, not only what it says.
#include "TimeSync.h"
#include "TimeBeacon.h"
#if defined(RRC_HUB)
#include "RRCHub.h"
#endif
#ifdef HAS_BME
#include "BME680.h"
#endif
// CBA NOTE Thge following <MsgPack.h> include *MUST* precede the "Utilities.h" include
#include <MsgPack.h>

#include <string>
#include <time.h>

extern RNS::Interface lora_interface;
#if defined(TCP_SERVER_TRANSPORT)
extern RNS::Interface tcp_server_interface;
// Mirror of the default in TCPServerInterface.h. A -D override reaches both,
// so the page cannot report a port the interface is not listening on.
#ifndef TCP_SERVER_PORT
#define TCP_SERVER_PORT 4242
#endif
#endif
extern uint32_t lora_phy_hash();
extern const char* radio_preset_name();
#if defined(BLE_PEER_TRANSPORT)
bool ble_peer_started();
uint32_t ble_peer_mtu();
uint32_t ble_peer_identity_writes();
uint32_t ble_peer_keepalives();
uint32_t ble_peer_packets_in();
uint32_t ble_peer_packets_out();
uint32_t ble_peer_dropped();
uint32_t ble_peer_last_in();
uint32_t ble_peer_last_out();
uint32_t ble_peer_frag_start();
uint32_t ble_peer_frag_lone();
#endif

#if HAS_WIFI && defined(UDP_TRANSPORT)
extern RNS::Interface udp_interface;
extern IPAddress wr_device_ip;
extern uint16_t udp_port;
extern uint8_t wifi_mode;
extern char wr_ssid[];
#endif
#if HAS_WIFI == true && defined(ESPNOW_TRANSPORT)
extern RNS::Interface espnow_interface;
bool espnow_started();
uint32_t espnow_channel();
uint32_t espnow_peer_count();
uint32_t espnow_packets_in();
uint32_t espnow_packets_out();
uint32_t espnow_discoveries_in();
uint32_t espnow_rx_dropped();
uint32_t espnow_tx_dropped();
uint32_t espnow_send_failures();
uint32_t espnow_reassembly_timeouts();
uint32_t espnow_last_peer_phy_hash();
const char* espnow_recovery_state_name();
uint32_t espnow_recovery_channel();
uint32_t espnow_recovery_scans();
uint32_t espnow_recovery_successes();
uint32_t espnow_recovery_failures();
uint32_t espnow_recovery_proof_failures();
uint32_t espnow_recovery_channel_errors();
uint32_t espnow_accepted_packets_in();
uint32_t espnow_accepted_from_selected();
const char* espnow_recovery_peer_mac();
extern uint8_t wifi_espnow_recovery_mode;
extern uint32_t wifi_espnow_scan_budget_ms;
extern uint8_t wifi_espnow_rendezvous_channel;
bool espnow_peer_has_upstream();
bool espnow_local_has_upstream();
#endif
extern RNS::Destination nomadnet_destination;

inline std::string wall_time_iso8601() {
  if (!RNS::Utilities::OS::wall_time_known()) return "unknown";
  const time_t seconds = (time_t)(RNS::Utilities::OS::wall_time_millis() / 1000ULL);
  struct tm utc{};
  if (gmtime_r(&seconds, &utc) == nullptr) return "invalid";
  char text[24];
  snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
           utc.tm_hour, utc.tm_min, utc.tm_sec);
  return text;
}

// Pages that report device internals (heap, flash, interfaces, transport and
// peer metrics). Reticulum's request policies are ALLOW_NONE, ALLOW_ALL and
// ALLOW_LIST, with nothing in between: there is no "any peer that identified"
// tier. ALLOW_LIST is also refused inside Destination before serve_page ever
// runs, so the client sees no response at all -- indistinguishable from a node
// that is switched off, which is how it kept presenting. These pages are
// therefore registered ALLOW_ALL and gated here, where a peer that has not
// identified can be told so.
inline bool page_requires_identity(const RNS::Bytes& path) {
#ifdef NOMADNET_PAGES_ALLOW_ALL
  // Diagnostic fixtures publish everything to anyone on the mesh.
  (void)path;
  return false;
#else
  return path == "/page/stack.mu"
      || path == "/page/device.mu"
#if HAS_WIFI == true && defined(ESPNOW_TRANSPORT)
      || path == "/page/espnow.mu"
#endif
#if defined(BLE_PEER_TRANSPORT)
      || path == "/page/ble.mu"
#endif
      ;
#endif
}

void add_interface_details(RNS::Bytes& content, const RNS::Interface& interface) {
  content << "    \"mode\": \"";
  switch (interface.mode()) {
    case RNS::Type::Interface::MODE_ACCESS_POINT:
      content << "ACCESS_POINT";
      break;
    case RNS::Type::Interface::MODE_BOUNDARY:
      content << "BOUNDARY";
      break;
    case RNS::Type::Interface::MODE_FULL:
      content << "FULL";
      break;
    case RNS::Type::Interface::MODE_GATEWAY:
      content << "GATEWAY";
      break;
    case RNS::Type::Interface::MODE_POINT_TO_POINT:
      content << "POINT_TO_POINT";
      break;
    case RNS::Type::Interface::MODE_ROAMING:
      content << "ROAMING";
      break;
    case RNS::Type::Interface::MODE_NONE:
      content << "NONE";
      break;
    default:
      break;
  }
  content << "\",\n";
  content << "    \"bitrate\": " << std::to_string(interface.bitrate()) << ",\n";
  content << "    \"packets_sent\": " << std::to_string(interface.tx()) << ",\n";
  content << "    \"packets_received\": " << std::to_string(interface.rx()) << ",\n";
  content << "    \"bytes_sent\": " << std::to_string(interface.txbytes()) << ",\n";
  content << "    \"bytes_received\": " << std::to_string(interface.rxbytes()) << ",\n";
  content << "    \"queued_announces\": " << std::to_string(interface.announce_queue().size()) << ",\n";
}

// Request handler for NomadNet pages. Signature is fixed by
// RNS::RequestHandler::response_generator (see src/Destination.h).
//
// The Link::request contract is that the return value must already be
// msgpack-encoded bytes — Link::handle_request splices it verbatim into
// the response envelope (Link.cpp:994). Python RNS auto-encodes
// arbitrary return values; in C++ we encode manually here.
RNS::Bytes serve_page(
	const RNS::Bytes& path,
	const RNS::Bytes& data,
	const RNS::Bytes& request_id,
	const RNS::Bytes& link_id,
	const RNS::Identity& remote_identity,
	double requested_at
) {

	std::string category;
	{
		MsgPack::Unpacker u;
		u.feed(data.data(), data.size());
		std::map<String, String> params;
		if (u.isMap()) {
			u.deserialize(params);
			for (const auto& [key, value] : params) {
				TRACEF("Param: key=%s, value=%s", key.c_str(), value.c_str());
			}
		}

		auto iter = params.find("var_c");
		if (iter != params.end()) {
			category = (*iter).second.c_str();
		}
	}

	VERBOSEF("Serving page %s with category \"%s\" to link <%s> with identity <%s>", path.toString().c_str(), category.c_str(), link_id.toHex().c_str(), (remote_identity ? remote_identity.hash().toHex().c_str() : RNS::Bytes{}.toHex().c_str()));
	// Plain printf so it survives a low RNS_LOG_LEVEL: shows whether a browsing
	// client identified at all, and with which hash. ALLOW_LIST pages are denied
	// to unidentified peers no matter what the list contains, so "identity=<none>"
	// and "identity=<hash not in list>" are very different problems.
	printf("[page] %s cat=%s identity=%s\n", path.toString().c_str(),
	       category.empty() ? "-" : category.c_str(),
	       remote_identity ? remote_identity.hash().toHex().c_str() : "<NONE - client did not identify>");
	MsgPack::Packer packer;
  {
    RNS::Bytes content;
    if (page_requires_identity(path) && !remote_identity) {
      content = "> Identification Required\n\n";
      content << "This page reports device internals, so it is served only to a peer that has identified itself on the link. Any identity is accepted -- it does not have to be on this node's remote-management allow list.\n\n";
      content << "Your client reached us without identifying. Enable identification in its settings and open this page again.\n\n";
      content << "`!`[• Back to the index`:/page/index.mu]`\n";
      packer.packBinary(content.data(), content.size());
      return RNS::Bytes(packer.data(), packer.size());
    }
    if (path == "/page/index.mu") {
      content = "> microReticulum Stats\n\n";
      content << ">> Memory\n";
      content << "`!`[• Heap Memory`:/page/stack.mu`c=heap]`\n";
      content << "`!`[• Memory Pools`:/page/stack.mu`c=pool]`\n";
      content << "`!`[• Memory Allocators`:/page/stack.mu`c=alloc]`\n";
      content << ">> Storage\n";
      content << "`!`[• Flash Memory`:/page/stack.mu`c=flash]`\n";
      content << "`!`[• Object Stores`:/page/stack.mu`c=store]`\n";
      content << "`!`[• Transport Metrics`:/page/stack.mu`c=metrics]`\n";
      content << ">> Device\n";
      content << "`!`[• General`:/page/device.mu`c=general]`\n";
      content << "`!`[• Interface`:/page/device.mu`c=interfaces]`\n";
      content << "`!`[• Time Status`:/page/time.mu]`\n";
      content << "`!`[• Airtime`:/page/airtime.mu]`\n";
#if HAS_WIFI == true && defined(ESPNOW_TRANSPORT)
      content << "`!`[• ESP-NOW Recovery`:/page/espnow.mu]`\n";
#endif
#if defined(BLE_PEER_TRANSPORT)
      content << "`!`[• BLE Peers`:/page/ble.mu]`\n";
#endif
#if defined(RRC_HUB)
      content << "`!`[• RRC Hub`:/page/device.mu`c=rrc]`\n";
#endif
#ifdef HAS_BME
      if (BME680::bme_installed) {
        content << "`!`[• Telemetry`:/page/telemetry.mu]`\n";
      }
#endif
      if (remote_identity) content << "\n🛡️ Verified identity: " << remote_identity.hash().toHex() << "\n";
      else content << "\n⚠️ Unknown identity. Identity must be provided for access to this site.\n";
    }
    else if (path == "/page/airtime.mu") {
      // Duty-cycle state was only ever visible on the serial console, which is
      // redirected the moment a KISS host attaches -- so on a deployed node it
      // was effectively unobservable. Exceeding a duty cycle is illegal and
      // degrades the band for everyone, so it needs to be readable from the
      // mesh like everything else.
      extern float airtime;
      extern float longterm_airtime;
      extern float st_airtime_limit;
      extern float lt_airtime_limit;
      extern bool airtime_lock;
      extern bool airtime_pressure;
      extern volatile uint32_t tx_deferred_routine;
      extern volatile uint8_t queue_height;
      char pct[16];
      content = "> Airtime\n\n";
      snprintf(pct, sizeof(pct), "%.4f", (double)longterm_airtime);
      content << "Long-term   : " << pct << "\n";
      snprintf(pct, sizeof(pct), "%.5f", (double)lt_airtime_limit);
      content << "Limit       : " << pct << (lt_airtime_limit == 0.0f ? "  (unenforced)" : "") << "\n";
      // The compiled-in regulatory ceiling, shown beside the value actually in
      // force. They should agree; on 2026-09-04 they did not, which is how the
      // discrepancy below was found and why this line stays.
      snprintf(pct, sizeof(pct), "%.5f", (double)RADIO_DUTY_CYCLE_LONGTERM);
      content << "Built-in cap: " << pct << "\n";
      extern float boot_lt_airtime_limit;
      extern volatile uint32_t lt_alock_writes;
      extern float lt_alock_last_requested;
      snprintf(pct, sizeof(pct), "%.5f", (double)boot_lt_airtime_limit);
      content << "At boot     : " << pct << "\n";
      content << "Host writes : " << std::to_string(lt_alock_writes) << "\n";
      snprintf(pct, sizeof(pct), "%.5f", (double)lt_alock_last_requested);
      content << "Host asked  : " << pct << "\n";
      snprintf(pct, sizeof(pct), "%.4f", (double)airtime);
      content << "Short-term  : " << pct << "\n";
      content << "Locked      : " << (airtime_lock ? "yes" : "no") << "\n";
      content << "Pressure    : " << (airtime_pressure ? "yes" : "no") << "\n";
      content << "Routine dropped: " << std::to_string(tx_deferred_routine) << "\n";
      content << "Queued      : " << std::to_string(queue_height) << "\n\n";
      content << "Under pressure, routine traffic (announces) stands aside so interactive traffic still has budget. Locked means the limit is reached and nothing is sent. A limit of zero means enforcement is off and only accounting is running.\n";
    }
    else if (path == "/page/time.mu") {
      using OS = RNS::Utilities::OS;
      const bool known = OS::wall_time_known();
      const uint64_t monotonic_ms = OS::monotonic_time_millis();
      const uint64_t adopted_ms = OS::wall_time_adopted_at();
      const uint64_t sync_age_ms = known && monotonic_ms >= adopted_ms
          ? monotonic_ms - adopted_ms : 0;
      content = "> Wall Time\n\n";
      content << "Status      : " << (known ? "known" : "unknown") << "\n";
      content << "UTC         : " << wall_time_iso8601() << "\n";
      content << "Unix ms     : " << std::to_string(OS::wall_time_millis()) << "\n";
      content << "Source      : " << OS::wall_time_source_name(OS::wall_time_source()) << "\n";
      content << "Last source : " << OS::wall_time_source_name(OS::wall_time_last_live_source()) << "\n";
      const uint64_t verified_at = OS::wall_time_verified_at();
      const uint64_t verified_age_ms = known && monotonic_ms >= verified_at
          ? monotonic_ms - verified_at : 0;
      content << "Stratum     : " << (known ? std::to_string(OS::wall_time_stratum()) : "-") << "\n";
      content << "Adopted     : " << std::to_string(sync_age_ms / 1000ULL) << " s ago\n";
      // The number that says whether to believe the clock. "Adopted" only says
      // when it last changed -- a node whose checks keep agreeing never adopts,
      // so that figure grows without bound while the clock is perfect.
      content << "Verified    : " << std::to_string(verified_age_ms / 1000ULL) << " s ago\n";
      content << "Last advance: " << std::to_string(OS::wall_time_last_correction()) << " ms\n";
      if (OS::wall_time_source() == OS::WallTimeSource::PERSISTED) {
        content << "\n`!`⚠️ Restored from storage and not verified since. This is a\n";
        content << "lower bound only: it cannot account for time spent powered off,\n";
        content << "and drifts further behind on every restart until a live source\n";
        content << "corrects it.`\n";
      }
      content << "\n";
      // Distribution, not just the value. A node that is hearing assertions but
      // refusing them looks exactly like one that is hearing nothing, and the
      // difference is the whole diagnosis: no authority provisioned, a key that
      // does not match, or a relay sitting on stale assertions.
      {
        const TimeBeaconStats& tb = time_beacon_stats();
        content << ">> Signed assertions\n";
        content << "Authorities : " << std::to_string(time_sync_authorities.size()) << "\n";
        content << "Originating : " << (time_beacon_enabled ? "yes" : "no");
        if (time_beacon_enabled) {
          content << " (" << std::to_string(tb.emitted) << " emitted, every "
                  << std::to_string(time_beacon_interval_s) << " s)";
        }
        content << "\n";
        content << "Heard       : " << std::to_string(tb.heard) << "\n";
        content << "Verified    : " << std::to_string(tb.verified) << "\n";
        content << "Adopted     : " << std::to_string(tb.adopted) << "\n";
        content << "Refused     : " << std::to_string(tb.refused_unlisted) << " not an authority, "
                << std::to_string(tb.refused_signature) << " bad signature, "
                << std::to_string(tb.refused_stale) << " stale, "
                << std::to_string(tb.refused_rules) << " unsafe\n";
        content << "\n";
      }
      content << ">> Clock-domain check\n";
      content << "Monotonic ms: " << std::to_string(monotonic_ms) << "\n";
      content << "Reload this page: both clocks must advance normally. Adopting UTC must not reset links or make monotonic time jump.\n";
    }
    else if (path == "/page/stack.mu") {
  	  if (category == "heap") {
        content = "{\n";
        content << "  \"heap_size\": " << std::to_string(RNS::Utilities::Memory::heap_size()) << ",\n";
        content << "  \"heap_free\": " << std::to_string(RNS::Utilities::Memory::heap_available()) << ",\n";
        content << "  \"heap_freepct\": " << std::to_string((unsigned)((double)RNS::Utilities::Memory::heap_available() / (double)RNS::Utilities::Memory::heap_size() * 100.0)) << ",\n";
#if defined(ESP32)
        content << "  \"heap_minfree\": " << std::to_string(ESP.getMinFreeHeap()) << ",\n";
        content << "  \"heap_maxalloc\": " << std::to_string(ESP.getMaxAllocHeap()) << ",\n";
        content << "  \"heap_fragmented\": " << std::to_string((unsigned)(100.0 - (double)ESP.getMaxAllocHeap() / (double)ESP.getFreeHeap() * 100.0)) << ",\n";
        content << "  \"psram_size\": " << std::to_string(ESP.getPsramSize()) << ",\n";
        content << "  \"psram_free\": " << std::to_string(ESP.getFreePsram()) << ",\n";
        content << "  \"psram_freepct\": " << std::to_string((ESP.getPsramSize() > 0) ? (unsigned)((double)ESP.getFreePsram() / (double)ESP.getPsramSize() * 100.0) : 0) << ",\n";
        content << "  \"psram_minfree\": " << std::to_string(ESP.getMinFreePsram()) << ",\n";
        content << "  \"psram_maxalloc\": " << std::to_string(ESP.getMaxAllocPsram()) << ",\n";
        content << "  \"psram_fragmented\": " << std::to_string((ESP.getFreePsram() > 0) ? (unsigned)(100.0 - (double)ESP.getMaxAllocPsram() / (double)ESP.getFreePsram() * 100.0) : 0) << ",\n";
#elif defined(ARDUINO_ARCH_NRF52) || defined(ARDUINO_NRF52_ADAFRUIT)
        //HEAD("Heap Stats", LOG_TRACE);
        //if (loglevel() == LOG_TRACE) {
        //	dbgMemInfo();
        //}
#endif
      	content << "}";
	    }
      else if (category == "flash") {
        size_t flash_size = RNS::Utilities::OS::storage_size();
        size_t flash_free = RNS::Utilities::OS::storage_available();
        uint8_t flash_freepct = 0;
        if (flash_size > 0) flash_freepct = (uint8_t)((double)flash_free / (double)flash_size * 100.0);
        //size_t fs_size = RNS::Utilities::OS::get_filesystem().storageSize();
        //size_t fs_free = RNS::Utilities::OS::get_filesystem().storageAvailable();
        //uint8_t fs_freepct = 0;
        //if (fs_size > 0) fs_freepct = (uint8_t)((double)fs_free / (double)fs_size * 100.0);

        content = "{\n";
        content << "  \"flash_size\": " << std::to_string(flash_size) << ",\n";
        content << "  \"flash_free\": " << std::to_string(flash_free) << ",\n";
        content << "  \"flash_freepct\": " << std::to_string(flash_freepct) << ",\n";
        //content << "  \"fs_size\": " << std::to_string(fs_size) << ",\n";
        //content << "  \"fs_free\": " << std::to_string(fs_free) << ",\n";
        //content << "  \"fs_freepct\": " << std::to_string(fs_freepct) << ",\n";
      	content << "}";
      }
      else if (category == "pool") {
        // The tuning signal for a pooled allocator. alloc_fault counts
        // allocations the arena could not serve, which spilled to the system
        // heap instead of failing -- non-zero means the pool is undersized,
        // which is a size to change rather than an outage. "largest_free" is
        // the number that actually matters: a pool with plenty free but no
        // large contiguous block will start refusing the allocations a link
        // setup needs, while looking healthy on free bytes alone.
        using Mem = RNS::Utilities::Memory;
        content = "{\n";
        content << "  \"heap_pool_size\": " << std::to_string(Mem::heap_pool_size()) << ",\n";
        content << "  \"heap_pool_free\": " << std::to_string(Mem::heap_pool_free()) << ",\n";
        content << "  \"heap_pool_largest_free\": " << std::to_string(Mem::heap_pool_largest_free()) << ",\n";
        content << "  \"heap_pool_fragmented\": " << std::to_string(Mem::heap_pool_fragmented()) << ",\n";
        content << "  \"heap_pool_alloc_fault\": " << std::to_string(Mem::heap_pool_alloc_fault()) << ",\n";
        content << "  \"psram_pool_size\": " << std::to_string(Mem::psram_pool_size()) << ",\n";
        content << "  \"psram_pool_free\": " << std::to_string(Mem::psram_pool_free()) << ",\n";
        content << "  \"psram_pool_fragmented\": " << std::to_string(Mem::psram_pool_fragmented()) << ",\n";
        content << "}\n";
      }
      else if (category == "alloc") {
        using Mem = RNS::Utilities::Memory;
        content = "{\n";
        content << "  \"default_alloc\": " << std::to_string(Mem::default_allocator_alloc()) << ",\n";
        content << "  \"default_free\": " << std::to_string(Mem::default_allocator_free()) << ",\n";
        content << "  \"container_alloc\": " << std::to_string(Mem::container_allocator_alloc()) << ",\n";
        content << "  \"container_free\": " << std::to_string(Mem::container_allocator_free()) << ",\n";
        content << "}\n";
      }
      else if (category == "store") {
        uint32_t destination_path_responses = 0;
        for (auto& [destination_hash, destination] : RNS::Transport::destinations()) {
          destination_path_responses += destination.path_responses().size();
        }
        uint32_t interface_announces = 0;
        for (auto& interface : RNS::Transport::get_interfaces()) {
          interface_announces += interface.announce_queue().size();
        }

        content = "{\n";
        content << "  \"paths\": " << std::to_string(RNS::Transport::new_path_table().size()) << ",\n";
        content << "  \"destinations\": " << std::to_string(RNS::Transport::destinations().size()) << ",\n";
        content << "  \"announces\": " << std::to_string(RNS::Transport::announce_table().size()) << ",\n";
        content << "  \"held_announces\": " << std::to_string(RNS::Transport::held_announces().size()) << ",\n";

        content << "  \"path_requests\": " << std::to_string(RNS::Transport::path_requests().size()) << ",\n";
        content << "  \"discovery_path_requests\": " << std::to_string(RNS::Transport::discovery_path_requests().size()) << ",\n";
        content << "  \"pending_local_path_requests\": " << std::to_string(RNS::Transport::pending_local_path_requests().size()) << ",\n";
        content << "  \"discovery_pr_tags\": " << std::to_string(RNS::Transport::discovery_pr_tags().size()) << ",\n";
        content << "  \"control_destinations\": " << std::to_string(RNS::Transport::control_destinations().size()) << ",\n";
        content << "  \"control_hashes\": " << std::to_string(RNS::Transport::control_hashes().size()) << ",\n";

        content << "  \"packet_hashes\": " << std::to_string(RNS::Transport::packet_hashlist().size()) << ",\n";
        content << "  \"reverse_hashes\": " << std::to_string(RNS::Transport::reverse_table().size()) << ",\n";
        content << "  \"receipts\": " << std::to_string(RNS::Transport::receipts().size()) << ",\n";

        content << "  \"links\": " << std::to_string(RNS::Transport::link_table().size()) << ",\n";
        content << "  \"pending_links\": " << std::to_string(RNS::Transport::pending_links().size()) << ",\n";
        content << "  \"active_links\": " << std::to_string(RNS::Transport::active_links().size()) << ",\n";
        content << "  \"tunnels\": " << std::to_string(RNS::Transport::tunnels().size()) << ",\n";

        content << "  \"known_destinations\": " << std::to_string(RNS::Identity::known_destinations().size()) << ",\n";
        content << "  \"destination_path_responses\": " << std::to_string(destination_path_responses) << ",\n";
        content << "  \"queued_announces\": " << std::to_string(interface_announces) << ",\n";

        content << "}";
      }
      else if (category == "metrics") {
        content = "{\n";
        content << "  \"packets_sent\": " << std::to_string(RNS::Transport::packets_sent()) << ",\n";
        content << "  \"packets_received\": " << std::to_string(RNS::Transport::packets_received()) << ",\n";
        content << "  \"paths_added\": " << std::to_string(RNS::Transport::paths_added()) << ",\n";
        content << "  \"paths_updated\": " << std::to_string(RNS::Transport::paths_updated()) << ",\n";
        content << "  \"paths_failed\": " << std::to_string(RNS::Transport::paths_failed()) << ",\n";
      	content << "}";
      }
      else {
        content = "CATEGORY NOT FOUND\n";
      }
    }
    else if (path == "/page/device.mu") {
  	  if (category == "general") {
        content = "{\n";
        content << "  \"firmware_version\": \"" << std::to_string(MAJ_VERS) << "." << std::to_string(MIN_VERS) << "\",\n";
        content << "  \"battery_voltage\": " << std::to_string((float)((uint16_t)(battery_voltage*100)) / 100.0) << ",\n";
        content << "  \"battery_percent\": " << std::to_string(((uint8_t)battery_percent)) << ",\n";
        content << "  \"battery_state\": \"";
        switch (battery_state) {
          case BATTERY_STATE_CHARGING:
            content << "CHARGING";
            break;
          case BATTERY_STATE_CHARGED:
            content << "CHARGED";
            break;
          case BATTERY_STATE_DISCHARGING:
            content << "DISCHARGING";
            break;
          case BATTERY_STATE_UNKNOWN:
            content << "UNKNOWN";
            break;
          default:
            break;
        }
        content << "\",\n";
        content << "  \"transport_identity\": \"" << (RNS::Transport::identity() ? RNS::Transport::identity().hash().toHex() : RNS::Bytes{}.toHex()) << "\",\n";
        content << "  \"probe_destination\": \"" << (RNS::Transport::probe_destination() ? RNS::Transport::probe_destination().hash().toHex() : RNS::Bytes{}.toHex()) << "\",\n";
        content << "  \"mgmt_destination\": \"" << (RNS::Transport::remote_management_destination() ? RNS::Transport::remote_management_destination().hash().toHex() : RNS::Bytes{}.toHex()) << "\",\n";
        content << "  \"nomadnet_destination\": \"" << (nomadnet_destination ? nomadnet_destination.hash().toHex() : RNS::Bytes{}.toHex()) << "\",\n";
      	content << "}";
      }
      else if (category == "interfaces") {
        content = "{\n";
#if defined(LORA_TRANSPORT)
        content << "  \"" << lora_interface.name().c_str() << "\": {\n";
        content << "    \"frequency\": " << std::to_string(lora_freq) << ",\n";
        content << "    \"bandwidth\": " << std::to_string(lora_bw) << ",\n";
        content << "    \"tx_power\": " << std::to_string(lora_txp) << ",\n";
        content << "    \"spreading_factor\": " << std::to_string(lora_sf) << ",\n";
        content << "    \"coding_rate\": " << std::to_string(lora_cr) << ",\n";
        // Fingerprint of the parameters that must match for two nodes to hear
        // each other. Comparing this between nodes reachable by any other route
        // turns "the radios are mysteriously deaf" into a one-line diagnosis.
        {
          char phybuf[16];
          snprintf(phybuf, sizeof(phybuf), "%08lx", (unsigned long)lora_phy_hash());
          content << "    \"phy_hash\": \"" << phybuf << "\",\n";
        }
        // The named preset this configuration corresponds to, or "Custom".
        // Reading it beside phy_hash answers both fleet questions at once:
        // "are these two nodes on the same settings" and "which settings".
        content << "    \"preset\": \"" << radio_preset_name() << "\",\n";
        content << "    \"current_rssi\": " << std::to_string(current_rssi) << ",\n";
        content << "    \"noise_floor\": " << std::to_string((int16_t)noise_floor) << ",\n";
        content << "    \"last_rssi\": " << std::to_string((int16_t)last_rssi) << ",\n";
        content << "    \"last_snr\": " << std::to_string((int8_t)((int8_t)last_snr_raw) / 4.0f) << ",\n";
        add_interface_details(content, lora_interface);
      	content << "  },\n";
#endif
#if defined(TCP_SERVER_TRANSPORT)
        if (tcp_server_interface) {
          content << "  \"" << tcp_server_interface.name().c_str() << "\": {\n";
          content << "    \"listen_port\": " << std::to_string(TCP_SERVER_PORT) << ",\n";
          add_interface_details(content, tcp_server_interface);
          content << "  },\n";
        }
#endif
#if defined(UDP_TRANSPORT)
        if (wifi_mode != WR_WIFI_OFF && udp_interface) {
          content << "  \"" << udp_interface.name().c_str() << "\": {\n";
          content << "    \"ip_addr\": \"" << wr_device_ip.toString().c_str() << "\",\n";
          content << "    \"udp_port\": " << std::to_string(udp_port) << ",\n";
          content << "    \"wifi_ssid\": \"" << wr_ssid << "\",\n";
          add_interface_details(content, udp_interface);
      	  content << "  },\n";
        }
#endif
      	content << "}";
      }
#if defined(RRC_HUB)
      else if (category == "rrc") {
        const RNS::Bytes hash = rrc_hub_destination_hash();
        content = "{\n";
        content << "  \"enabled\": " << (rrc_hub_enabled ? "true" : "false") << ",\n";
        content << "  \"running\": " << (rrc_hub_running() ? "true" : "false") << ",\n";
        content << "  \"destination\": \"" << hash.toHex() << "\",\n";
        content << "  \"announce_interval_seconds\": " << std::to_string(rrc_hub_announce_interval_seconds) << ",\n";
        content << "  \"max_sessions\": " << std::to_string(rrc_hub_max_sessions) << ",\n";
        content << "  \"max_rooms_per_session\": " << std::to_string(rrc_hub_max_rooms_per_session) << ",\n";
        content << "  \"max_body_bytes\": " << std::to_string(rrc_hub_max_body_bytes) << ",\n";
        content << "  \"rate_per_minute\": " << std::to_string(rrc_hub_rate_per_minute) << ",\n";
        content << "  \"ping_interval_seconds\": " << std::to_string(rrc_hub_ping_interval_seconds) << ",\n";
        content << "  \"pong_timeout_seconds\": " << std::to_string(rrc_hub_pong_timeout_seconds) << ",\n";
        content << "  \"sessions\": " << std::to_string(rrc_hub_session_count()) << ",\n";
        content << "  \"identified_sessions\": " << std::to_string(rrc_hub_identified_count()) << ",\n";
        content << "  \"rooms\": " << std::to_string(rrc_hub_room_count()) << ",\n";
        content << "  \"memberships\": " << std::to_string(rrc_hub_membership_count()) << ",\n";
        content << "  \"packets_received\": " << std::to_string(rrc_hub_rx_count()) << ",\n";
        content << "  \"packets_sent\": " << std::to_string(rrc_hub_tx_count()) << ",\n";
        content << "  \"accepted\": " << std::to_string(rrc_hub_accepted_count()) << ",\n";
        content << "  \"forwarded\": " << std::to_string(rrc_hub_forwarded_count()) << ",\n";
        content << "  \"rejected\": " << std::to_string(rrc_hub_rejected_count()) << ",\n";
        content << "  \"rate_limited\": " << std::to_string(rrc_hub_rate_limited_count()) << ",\n";
        content << "  \"malformed\": " << std::to_string(rrc_hub_malformed_count()) << ",\n";
        content << "  \"identify_timeouts\": " << std::to_string(rrc_hub_identify_timeout_count()) << ",\n";
        content << "  \"hello_timeouts\": " << std::to_string(rrc_hub_hello_timeout_count()) << ",\n";
        content << "  \"pong_timeouts\": " << std::to_string(rrc_hub_pong_timeout_count()) << "\n";
        content << "}";
      }
#endif
      else {
        content = "CATEGORY NOT FOUND\n";
      }
    }
#if defined(BLE_PEER_TRANSPORT)
    // Served over the mesh rather than the console, because a BLE peer link is
    // exactly the situation where the console is unavailable: on a deployed
    // node the serial output goes to whichever KISS host is attached, and on
    // this fixture attaching to serial at all used to reset the board. The
    // counters below are what distinguished a link that never formed from one
    // that formed and was torn down, which took a day to tell apart by hand.
    else if (path == "/page/ble.mu") {
      content = "> BLE Peers\n\n";
      content << "Service     : " << (ble_peer_started() ? "up" : "down") << "\n";
      content << "Negotiated MTU: " << std::to_string(ble_peer_mtu()) << "\n";
      content << "Identity writes: " << std::to_string(ble_peer_identity_writes()) << "\n";
      content << "Keepalives  : " << std::to_string(ble_peer_keepalives()) << "\n\n";

      content << ">> Traffic\n";
      content << "Packets in  : " << std::to_string(ble_peer_packets_in()) << "\n";
      content << "Packets out : " << std::to_string(ble_peer_packets_out()) << "\n";
      content << "Dropped     : " << std::to_string(ble_peer_dropped()) << "\n";
      content << "Last in     : " << std::to_string(ble_peer_last_in()) << " B\n";
      content << "Last out    : " << std::to_string(ble_peer_last_out()) << " B\n\n";

      // A single-fragment packet must go out as START, never LONE: the client
      // declares LONE and never emits it, and its reassembler drops types it
      // does not recognise, so emitting LONE makes the whole outbound direction
      // vanish while inbound keeps working. A non-zero LONE count here means
      // that regression is back. See BLEPeerProtocol.h.
      content << ">> Fragments\n";
      content << "START       : " << std::to_string(ble_peer_frag_start()) << "\n";
      content << "LONE (must be 0): " << std::to_string(ble_peer_frag_lone()) << "\n";
    }
#endif
#if HAS_WIFI == true && defined(ESPNOW_TRANSPORT)
    else if (path == "/page/espnow.mu") {
      char local_phy[16];
      char peer_phy[16];
#if defined(LORA_TRANSPORT)
      snprintf(local_phy, sizeof(local_phy), "%08lx", (unsigned long)lora_phy_hash());
#else
      snprintf(local_phy, sizeof(local_phy), "none");
#endif
      snprintf(peer_phy, sizeof(peer_phy), "%08lx", (unsigned long)espnow_last_peer_phy_hash());
      content = "> ESP-NOW Recovery\n\n";
      content << "State       : " << espnow_recovery_state_name() << "\n";
      content << "Interface   : " << (espnow_started() ? "up" : "down") << "\n";
      content << "WiFi channel: " << std::to_string(espnow_channel()) << "\n";
      content << "Peers recent: " << std::to_string(espnow_peer_count()) << "\n";
      content << "Local PHY   : " << local_phy << "\n";
      content << "Last peer PHY: " << peer_phy << "\n\n";
      content << ">> Recovery policy\n";
      content << "Mode        : " << (wifi_espnow_recovery_mode == 1 ? "scan-before-softap" : "off") << "\n";
      content << "Scan budget : " << std::to_string(wifi_espnow_scan_budget_ms / 1000) << " s\n";
      content << "Rendezvous  : channel " << std::to_string(wifi_espnow_rendezvous_channel) << "\n";
      content << "Selected    : " << espnow_recovery_peer_mac() << "\n";
      content << "Parent upstream: " << (espnow_peer_has_upstream() ? "yes" : "no") << "\n";
      content << "We are upstream: " << (espnow_local_has_upstream() ? "yes" : "no") << "\n";
      content << "Relaying    : " << (RNS::Reticulum::transport_enabled() ? "yes" : "no") << "\n";
      content << "Pinned chan : " << std::to_string(espnow_recovery_channel()) << "\n";
      content << "Scans       : " << std::to_string(espnow_recovery_scans()) << "\n";
      content << "Succeeded   : " << std::to_string(espnow_recovery_successes()) << "\n";
      content << "Failed      : " << std::to_string(espnow_recovery_failures()) << "\n";
      content << "Proof fails : " << std::to_string(espnow_recovery_proof_failures()) << "\n";
      content << "Channel errors: " << std::to_string(espnow_recovery_channel_errors()) << "\n\n";
      content << ">> Traffic and health\n";
      content << "Packets in  : " << std::to_string(espnow_packets_in()) << "\n";
      content << "Packets out : " << std::to_string(espnow_packets_out()) << "\n";
      content << "IFAC accepted: " << std::to_string(espnow_accepted_packets_in()) << "\n";
      content << "From selected: " << std::to_string(espnow_accepted_from_selected()) << "\n";
      content << "Discoveries : " << std::to_string(espnow_discoveries_in()) << "\n";
      content << "RX drops    : " << std::to_string(espnow_rx_dropped()) << "\n";
      content << "TX drops    : " << std::to_string(espnow_tx_dropped()) << "\n";
      content << "Send failures: " << std::to_string(espnow_send_failures()) << "\n";
      content << "Reassembly timeouts: " << std::to_string(espnow_reassembly_timeouts()) << "\n\n";
      content << "Discovery PHY data remains advisory. Channel search is never run while station WiFi is connected.\n\n";
      content << "A node attaches to a peer that can reach the mesh, and adopts one that cannot only after the scan budget expires with nothing better. A node with no way out of its own stops relaying while it has a parent, because every announce it repeats reaches the hub one hop longer than the copy the hub already heard, and it starts again the moment the parent is lost.\n";
    }
#endif
#ifdef HAS_BME
    else if (path == "/page/telemetry.mu") {
      if (!BME680::bme.performReading()) {
        content = "> Telemetry\n\n`!`❌ Failed to perform BME680 reading.`\n";
      } else {
        content = "> Environmental Telemetry\n\n";
        content << "🌡️ Temperature: " << std::to_string((float)BME680::bme.temperature) << " °C\n";
        content << "💧 Humidity: " << std::to_string((float)BME680::bme.humidity) << " %\n";
        content << "⏲️ Pressure: " << std::to_string((float)(BME680::bme.pressure / 100.0)) << " hPa\n";
        content << "💨 Gas Resistance: " << std::to_string((float)(BME680::bme.gas_resistance / 1000.0)) << " KOhms\n";
      }
    }
#endif
    else {
      content = "PATH NOT FOUND\n";
    }
    packer.packBinary(content.data(), content.size());
  }
	return RNS::Bytes(packer.data(), packer.size());
}
