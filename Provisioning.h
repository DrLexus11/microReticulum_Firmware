// Copyright (C) 2026, Chad Attermann

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.


#pragma once

#ifdef HAS_PROVISIONING

#include <microReticulum/Bytes.h>

#include <stddef.h>
#include <stdint.h>

// Per-platform cap on a single inbound CMD_PROVISION_REQ payload. Sized
// to admit the largest plausible host request (SetState across all radio
// + general fields) without giving up too much RAM on tight nRF52 builds.
#if MCU_VARIANT == MCU_NRF52
  #define PROVISION_RX_BUF_MAX 512
#else
  #define PROVISION_RX_BUF_MAX 2048
#endif

// ---------------------------------------------------------------------------
// Provisioning namespace + field IDs.
//
// Namespace IDs 1-2 are RNS built-ins (Reticulum, Transport); 100-199
// are the official app range. PROV_NS_RADIO and its field IDs are kept
// here as a reference for the (currently disabled) radio namespace —
// EEPROM (driven by rnodeconf) remains the source of truth for radio
// configuration. See register_provisioning_namespaces() below.
//
// NOTE: **NEVER** change these values once they are in production. Only additions can be made.
// ---------------------------------------------------------------------------
#define PROV_NS_GENERAL         100
#define PROV_NS_RADIO           101
#define PROV_NS_NETWORK         102
#define PROV_NS_METRICS         103
#define PROV_NS_METRICS_IFACE   104
#define PROV_NS_IFACE_LORA      105
#define PROV_NS_IFACE_UDP       106
#define PROV_NS_METRICS_ADDRS   107
#define PROV_NS_METRICS_DEV     108
#define PROV_NS_IFAC_LORA       109
#define PROV_NS_IFAC_TCP        110
#define PROV_NS_IFAC_UDP        111
#define PROV_NS_SECURE_NODE     112
#define PROV_NS_RRC             113
#define PROV_NS_LXMF            115
#define PROV_NS_BLUETOOTH       114

#define PROV_BT_ENABLED            1
#define PROV_BT_PAIR               2

#define PROV_BT_STATE             32
#define PROV_BT_PASSKEY           33
#define PROV_BT_DEVNAME           34
#define PROV_BT_BONDS             35
// Bytes the attached BLE host has written to us. Distinguishes "the phone sent
// nothing" from "the phone sent it and we lost it downstream".
#define PROV_BT_RXBYTES           36
// BLE peer interface (the node-as-peer path, not the RNode/KISS one). Says
// whether a single fragment has ever crossed the peer link, which no console
// line can answer without resetting the board and destroying the evidence.
#define PROV_BT_PEER_IN           37
#define PROV_BT_PEER_OUT          38
#define PROV_BT_PEER_DROPPED      39
#define PROV_BT_PEER_UP           40
// Packet-level diagnostics. Rev1's USB CDC resets the board when a console is
// attached, so the console cannot be read without destroying the very session
// being diagnosed. These carry the same information over provisioning, which
// reads without resetting.
#define PROV_BT_PEER_LASTIN       41
#define PROV_BT_PEER_LASTIN_HEX   42
#define PROV_BT_PEER_LASTOUT      43
#define PROV_BT_PEER_MTU          44
#define PROV_BT_PEER_KEEPALIVES   45
#define PROV_BT_PEER_IDENTITY_RX  46
#define PROV_BT_PEER_FRAGHDR      47
#define PROV_BT_PEER_FRAG_LONE    49
#define PROV_BT_PEER_FRAG_START   50

#define PROV_GENERAL_KISS_LOG        1
#define PROV_GENERAL_LORA_MODE       2
#define PROV_GENERAL_UDP_MODE        3
#define PROV_GENERAL_NOMADNET_ENABLE 4
#define PROV_GENERAL_NOMADNET_NAME   5
#define PROV_GENERAL_GPIO0           6
#define PROV_GENERAL_GPIO1           7

#define PROV_METRICS_TRANS_ID   1
#define PROV_METRICS_PROBE_DST  2
#define PROV_METRICS_MGMT_DST   3
#define PROV_METRICS_NOMAD_DST  4

#define PROV_METRICS_DEV_VER    1
#define PROV_METRICS_DEV_BATV   2
#define PROV_METRICS_DEV_BATP   3
#define PROV_METRICS_DEV_BATS   4
// Smallest free stack ever observed on the Arduino loop task, in bytes. The
// whole Reticulum stack runs there, so this is the number that goes to zero
// before the board panics in the allocator with no other warning.
#define PROV_METRICS_DEV_STACK  5
#define PROV_METRICS_DEV_HEAP     6
#define PROV_METRICS_DEV_HEAPBLK  7
#define PROV_METRICS_DEV_PSRAM    8
#define PROV_METRICS_DEV_UPTIME   9
#define PROV_METRICS_DEV_RESETRC 10
#define PROV_METRICS_DEV_RESETNM 11
#define PROV_METRICS_DEV_PREVUP  12
#define PROV_METRICS_DEV_BOOTS   13
// LXMF peer sync visibility. On this hardware the console is not a reliable
// pipe -- printf output is redirected to a KISS TCP host when one is connected
// (Utilities.h), and attaching USB resets the board -- so peering state is
// exposed here, where it can be read without disturbing anything.
#define PROV_METRICS_DEV_PEERS      14
#define PROV_METRICS_DEV_PNSTORE    15
#define PROV_METRICS_DEV_ANNPROP    16
#define PROV_METRICS_DEV_ANNANY     17
#define PROV_METRICS_DEV_SYNCATT    18
#define PROV_METRICS_DEV_SYNCLINK   19
#define PROV_METRICS_DEV_SYNCOFFER  20
#define PROV_METRICS_DEV_SYNCRESP   21
#define PROV_METRICS_DEV_SYNCSENT   22
#define PROV_METRICS_DEV_SYNCERR    23
#define PROV_METRICS_DEV_SYNCRSZ    24
#define PROV_METRICS_DEV_SYNCOUT    25
// Which part of loop() was running when the task watchdog last fired, and the
// slowest phase since boot. See LoopPhase.h and CarriedIssues.md §1.
#define PROV_METRICS_DEV_WDTPHASE   26
#define PROV_METRICS_DEV_SLOWPHASE  27
#define PROV_METRICS_DEV_SLOWMS     28
// Windowed equivalents. LoopPhase.h rolls the window on a timer; these metrics
// are pure reads of the last completed window and remain unchanged until the
// next window completes.
#define PROV_METRICS_DEV_RECENTPH   29
#define PROV_METRICS_DEV_RECENTMS   30

#define PROV_METRICS_LORA_FREQ  1
#define PROV_METRICS_LORA_BW    2
#define PROV_METRICS_LORA_SF    3
#define PROV_METRICS_LORA_CR    4
#define PROV_METRICS_LORA_TXP   5
#define PROV_METRICS_LORA_CRSSI 6  
#define PROV_METRICS_LORA_NF    7
#define PROV_METRICS_LORA_LRSSI 8
#define PROV_METRICS_LORA_LSNR  9
#define PROV_METRICS_LORA_STAL  10
#define PROV_METRICS_LORA_LTAL  11
// Where a host's packets actually get to. tx_calls counts modem keyings and
// queue_height counts frames waiting; together they say whether traffic from an
// attached KISS host reaches the radio queue, reaches the modem, or never
// arrives. Only readable as metrics -- the console equivalent resets the board.
#define PROV_METRICS_LORA_TXCALLS 12
#define PROV_METRICS_LORA_QUEUE   13

#define PROV_METRICS_UDP_ADDR   1
#define PROV_METRICS_UDP_PORT   2
#define PROV_METRICS_WIFI_SSID  3

#define PROV_RADIO_OP_MODE      1
#define PROV_RADIO_FREQ         2
#define PROV_RADIO_BW           3
#define PROV_RADIO_SF           4
#define PROV_RADIO_CR           5
#define PROV_RADIO_TXP          6
#define PROV_RADIO_IMPLICIT     7
#define PROV_RADIO_STAL         8
#define PROV_RADIO_LTAL         9
#define PROV_RADIO_PRESET      10

#define PROV_NET_IP             1
#define PROV_NET_PORT           2
#define PROV_NET_SSID           3
#define PROV_NET_MODE           4
// SoftAP fallback timing, in SECONDS on the wire. The firmware keeps
// milliseconds internally; an operator choosing how long a node waits before
// deserting a rebooting router should not have to think in milliseconds.
#define PROV_NET_AP_FALLBACK_S  5
#define PROV_NET_AP_RETRY_S     6
#define PROV_NET_AP_MAX_DEFER_S 7
// Read-only: is the node currently serving its own AP, and to how many clients.
#define PROV_NET_AP_ACTIVE      32
#define PROV_NET_AP_CLIENTS     33
#define PROV_NET_AP_SSID        34

#define PROV_IFAC_LORA_ENABLED     1
#define PROV_IFAC_LORA_NETNAME     2
#define PROV_IFAC_LORA_PASSPHRASE  3

#define PROV_IFAC_TCP_ENABLED      1
#define PROV_IFAC_TCP_NETNAME      2
#define PROV_IFAC_TCP_PASSPHRASE   3

#define PROV_IFAC_UDP_ENABLED      1
#define PROV_IFAC_UDP_NETNAME      2
#define PROV_IFAC_UDP_PASSPHRASE   3

#define PROV_SECURE_NODE_ENABLED   1

#define PROV_RRC_ENABLED           1
#define PROV_RRC_NAME              2
#define PROV_RRC_ANNOUNCE_INTERVAL 3
#define PROV_RRC_MAX_SESSIONS      4
#define PROV_RRC_MAX_ROOMS_SESSION 5
#define PROV_RRC_MAX_BODY_BYTES    6
#define PROV_RRC_RATE_PER_MINUTE   7
#define PROV_RRC_PING_INTERVAL     8
#define PROV_RRC_PONG_TIMEOUT      9
#define PROV_RRC_BRIDGE_ENABLED    10
#define PROV_RRC_BRIDGE_ROOMS      11
#define PROV_RRC_BRIDGE_HISTORY    12

#define PROV_RRC_DESTINATION       32
#define PROV_RRC_RUNNING           33
#define PROV_RRC_SESSIONS          34
#define PROV_RRC_IDENTIFIED        35
#define PROV_RRC_ROOMS             36
#define PROV_RRC_MEMBERSHIPS       37
#define PROV_RRC_RX                38
#define PROV_RRC_TX                39
#define PROV_RRC_ACCEPTED          40
#define PROV_RRC_FORWARDED         41
#define PROV_RRC_REJECTED          42
#define PROV_RRC_RATE_LIMITED      43
#define PROV_RRC_MALFORMED         44
#define PROV_RRC_IDENT_TIMEOUTS    45
#define PROV_RRC_HELLO_TIMEOUTS    46
#define PROV_RRC_PONG_TIMEOUTS     47
#define PROV_RRC_BRIDGE_MEMBERS    48
#define PROV_RRC_BRIDGE_QUEUED     49
#define PROV_RRC_BRIDGE_DELIVERED  50
#define PROV_RRC_BRIDGE_DROPPED    51
#define PROV_RRC_BRIDGE_HISTDEPTH  52

// Set true once Provisioning::Provisioner::begin() has run.
extern bool provisioning_started;

// Buffer for an in-flight CMD_PROVISION_REQ frame. Bytes are un-escaped
// into here by the serial_callback() byte-accumulator branch and handed
// to on_provision_request() at frame-end.
extern RNS::Bytes provision_rx_buf;

// Bring the Provisioning subsystem up. Must be called after the
// filesystem has been registered with RNS::Utilities::OS so storage
// reads can resolve.
void init_provisioning();

// Dispatch one un-escaped MsgPack envelope to the Provisioning Provisioner
// and emit the framed MsgPack response back over KISS.
void on_provision_request(const RNS::Bytes& req);

// Emit a CMD_PROVISION_RSP KISS frame carrying the given payload bytes.
void kiss_indicate_provision_response(const RNS::Bytes& payload);

#endif // HAS_PROVISIONING

// --- LXMF propagation peering (ns115) ---------------------------------------
#define PROV_LXMF_STATIC_PEER   1
