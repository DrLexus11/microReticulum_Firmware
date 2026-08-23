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

#include "ROM.h"
#include "Boards.h"

#include <SPI.h>

#ifndef CONFIG_H
	#define CONFIG_H

	#define MAJ_VERS  0x01
	#define MIN_VERS  0x56

	#define MODE_HOST 0x11
	#define MODE_TNC  0x12

	#define CABLE_STATE_DISCONNECTED 0x00
	#define CABLE_STATE_CONNECTED    0x01
	uint8_t cable_state = CABLE_STATE_DISCONNECTED;
	
	#define BT_STATE_NA        0xff
	#define BT_STATE_OFF       0x00
	#define BT_STATE_ON        0x01
	#define BT_STATE_PAIRING   0x02
	#define BT_STATE_CONNECTED 0x03

	// Bluetooth pairing passkey used on boards with no display.
	//
	// The stock behaviour generates a random six-digit passkey and surfaces it
	// two ways: on a display, or over a wired KISS link. A RAD-01 has neither in
	// the field -- no screen, and no accessible UART once it is in a wall or on
	// a roof -- so pairing was simply impossible for a resident. During bring-up
	// on 2026-08-23 the only way to pair at all was to read the passkey off USB
	// serial and type it in by hand.
	//
	// A fixed passkey is not a meaningful loss of security here, and it is worth
	// being precise about why: BLE bonding protects the radio hop between phone
	// and node. It is not what protects the traffic. Reticulum above it is
	// already end-to-end encrypted and identity-authenticated, so a node cannot
	// read its clients' messages whatever the BLE link does. What a fixed
	// passkey concedes is protection against an attacker actively MITM-ing the
	// BLE hop in the pairing window -- who would gain a transport-layer position
	// and still not be able to read anything.
	//
	// 123456 matches the Meshtastic convention, so it is what users already
	// expect to type.
	//
	// Boards WITH a display keep the random passkey: they can show it, so there
	// is no reason to weaken them.
	#ifndef BLE_FIXED_PASSKEY
	#define BLE_FIXED_PASSKEY 123456
	#endif
	uint8_t bt_state = BT_STATE_NA;
	uint32_t bt_ssp_pin = 0;
	bool bt_ready = false;
	bool bt_enabled = false;
	bool bt_allow_pairing = false;

	#define WR_CHANNEL_DEFAULT 1
	#define WR_WIFI_OFF        0x00
	#define WR_WIFI_STA        0x01
	#define WR_WIFI_AP         0x02
	#define WR_STATE_NA        0xff
	#define WR_STATE_OFF       0x00
	#define WR_STATE_ON        0x01
	#define WR_STATE_CONNECTED 0x02
	uint8_t wr_state = WR_STATE_OFF;
	uint8_t wr_channel = WR_CHANNEL_DEFAULT;

	#define M_FRQ_S 27388122
	#define M_FRQ_R 27388061
	bool console_active = false;
	bool modem_installed = false;

	#define MTU   	   508
	#define SINGLE_MTU 255
	#define HEADER_L   1
	#define MIN_L	   1
	#define CMD_L      64

    bool mw_radio_online = false;

	#define eeprom_addr(a) (a+EEPROM_OFFSET)
	#define config_addr(a) (a+CONFIG_OFFSET)

    #if (MODEM == SX1262 || MODEM == SX1280) && defined(NRF52840_XXAA)
        SPIClass spiModem(NRF_SPIM2, pin_miso, pin_sclk, pin_mosi);
    #endif

	// MCU independent configuration parameters
	const long serial_baudrate  = 115200;
	// Interval for the headless heap trend report; see heap_watch() in the .ino.
	#define HEAP_REPORT_INTERVAL_MS 60000
	// How often a headless node re-announces its NomadNet site, so peers can
	// repair a stale or degraded path without a power cycle. Costs LoRa airtime.
	// RNS invalidates a path when delivery fails, and this node runs with
	// transport disabled so it never answers path requests -- the only way a peer
	// relearns the route is the next announce. At 30 minutes that meant up to half
	// an hour of unreachability after a single failed link. Announces are cheap on
	// WiFi/UDP; raise this again if the node is ever LoRa-only, where they are not.
	#define NOMADNET_ANNOUNCE_INTERVAL_MS 300000   // 5 minutes
	// First announce after boot. Must be comfortably later than DHCP + the UDP
	// socket rebind, or it is emitted before the interface can carry it.
	#define NOMADNET_FIRST_ANNOUNCE_MS 60000       // 1 minute
	// Random extra delay (0..this) added to every announce interval, re-rolled
	// each cycle.
	//
	// Without it a fleet transmits in lockstep. That is not a theoretical
	// concern for QuakeMesh: when mains power returns to a block, every node
	// boots within seconds of every other, and each would then announce at
	// exactly NOMADNET_FIRST_ANNOUNCE_MS. Hundreds of simultaneous
	// transmissions on a half-duplex channel with no collision detection means
	// none of them land, and the nodes are least able to recover at precisely
	// the moment recovery matters most.
	//
	// The PRNG is seeded from esp_random() during setup(), so nodes running
	// identical firmware still pick different offsets. Jitter is additive only
	// (never early), so it lengthens the mean interval slightly -- deliberate,
	// since erring toward less airtime is the safe direction.
	#define NOMADNET_ANNOUNCE_JITTER_MS 60000      // up to 1 minute
	// What to do when the modem fails to complete a transmission. Rebooting takes
	// a transport node off the mesh entirely and discards its path/link state to
	// recover from one bad packet; RNS already retries above this layer. Default
	// to dropping the packet and returning to receive. Set to 1 to restore the
	// old reboot behaviour. (The native build has always had this choice, via
	// rnoded.conf's reboot_on_tx_failure; embedded had no say.)
	#define REBOOT_ON_TX_FAILURE 0

	// How long the modem may go without demodulating a single packet before it
	// is torn down and re-initialised.
	//
	// A wedged modem is invisible from every health indicator this firmware
	// exposes: radio_online stays 1, hw_ready stays 1, the configured SF/BW/
	// frequency read back correctly, the noise floor looks plausible, and the
	// transmitter keeps keying happily. Only the receive side is dead, and
	// nothing ever repairs it. Observed 2026-08-22: a board that had been up
	// for days stopped demodulating entirely -- RF from its peer arrived at
	// -44 dBm and was never decoded, in either direction -- and only a reboot
	// brought it back. On a wall plug, with no USB and no WiFi, such a node is
	// simply gone with no way to notice or recover.
	//
	// stopRadio()/startRadio() is a few milliseconds and reprograms every modem
	// register from the live config, so the cost of a false trigger is trivial.
	// A node that legitimately hears nobody re-inits on this interval forever
	// and says so in the log, which is the right trade for an unattended node.
	// Set to 0 to disable.
	#ifndef RADIO_RX_WATCHDOG_MS
	#define RADIO_RX_WATCHDOG_MS 1200000           // 20 minutes
	#endif

	// Commit-confirm window for radio PHY changes ("commit confirmed", as on
	// network gear). LoRa PHY parameters must match on both ends: the instant
	// one node's SF/BW/frequency changes and its peer's does not, the link is
	// gone in both directions -- strong RF arrives and nothing demodulates.
	// For a node reachable ONLY over that radio, this is unrecoverable without
	// physically retrieving it.
	//
	// So: when a config change is applied over a link that was demonstrably
	// working, remember the previous settings and start this timer. Any decoded
	// packet confirms the new config and disarms it. If the window expires in
	// silence, roll back to the last known-good settings and persist them.
	//
	// Must be comfortably longer than the peer's announce interval (see
	// NOMADNET_ANNOUNCE_INTERVAL_MS), or a healthy link gets reverted just
	// because nobody happened to transmit. It must also be long enough to
	// change both ends of a link by hand without the first one reverting
	// while the second is still being edited.
	// Set to 0 to disable.
	#ifndef RADIO_CONFIG_CONFIRM_MS
	#define RADIO_CONFIG_CONFIRM_MS 900000         // 15 minutes
	#endif

	// SX1276 RSSI offset to get dBm value from
	// packet RSSI register
	const int  rssi_offset = 157;

	// Default LoRa settings
	#define PHY_HEADER_LORA_SYMBOLS    20
	#define PHY_CRC_LORA_BITS          16
	#define LORA_PREAMBLE_SYMBOLS_MIN  18
	#define LORA_PREAMBLE_TARGET_MS    24
	#define LORA_PREAMBLE_FAST_DELTA   18
	#define LORA_FAST_THRESHOLD_BPS    30E3
	#define LORA_LIMIT_THRESHOLD_BPS   60E3
	#define LORA_GUARD_THRESHOLD_BPS   14E3
	#define LORA_FAST_GUARD_MS         48
	long lora_preamble_symbols      =  LORA_PREAMBLE_SYMBOLS_MIN;
	long lora_preamble_time_ms      =  0;
	long lora_header_time_ms        =  0;
	float lora_symbol_time_ms       =  0.0;
	float lora_symbol_rate          =  0.0;
	float lora_us_per_byte          =  0.0;
	bool lora_low_datarate          =  false;
	bool lora_limit_rate            =  false;
	bool lora_guard_rate            =  false;

	// CSMA Parameters
	#define CSMA_SIFS_MS               0
	#define CSMA_POST_TX_YIELD_SLOTS   3
	#define CSMA_SLOT_MAX_MS           100
	#define CSMA_SLOT_MIN_MS           24
	#define CSMA_SLOT_MIN_FAST_DELTA   18
	#define CSMA_SLOT_SYMBOLS          12
	#define CSMA_CW_BANDS              4
	#define CSMA_CW_MIN                0
	#define CSMA_CW_PER_BAND_WINDOWS   15
	#define CSMA_BAND_1_MAX_AIRTIME    7
	#define CSMA_BAND_N_MIN_AIRTIME    85
	#define CSMA_INFR_THRESHOLD_DB     11
	#define CSMA_RFENV_RECAL_MS        2500
	#define CSMA_RFENV_RECAL_LIMIT_DB -83
	bool interference_detected      =  false;
	bool avoid_interference         =  true;
	int csma_slot_ms                =  CSMA_SLOT_MIN_MS;
	unsigned long difs_ms           =  CSMA_SIFS_MS + 2*csma_slot_ms;
	unsigned long difs_wait_start   = -1;
	unsigned long cw_wait_start     = -1;
	unsigned long cw_wait_target    = -1;
	unsigned long cw_wait_passed    =  0;
	int csma_cw                     = -1;
	uint8_t cw_band                 =  1;
	uint8_t cw_min                  =  0;
	uint8_t cw_max                  =  CSMA_CW_PER_BAND_WINDOWS;

	// LoRa settings
	int  lora_sf   	                =  0;
	int  lora_cr                    =  5;
	int  lora_txp                   =  0xFF;
	uint32_t lora_bw                =  0;
	uint32_t lora_freq              =  0;
	uint32_t lora_bitrate           =  0;

	// Operational variables
	bool radio_locked  = true;
	bool radio_online  = false;
	bool community_fw  = true;
	bool hw_ready      = false;
	bool radio_error   = false;
	bool disp_ready    = false;
	bool pmu_ready     = false;
	bool promisc       = false;
	bool implicit      = false;
	bool memory_low    = false;
	uint8_t implicit_l = 0;

	uint8_t op_mode   = MODE_HOST;
	// Operating mode this board should adopt once it has a saved radio config.
	// validate_status() used to hardcode MODE_TNC there, so a board could never
	// be put back into host-driven modem mode without a serial cable. Defaults to
	// MODE_TNC, which is exactly the previous behaviour; settable over the air via
	// the Radio provisioning namespace (PROV_RADIO_OP_MODE).
	uint8_t prov_op_mode = MODE_TNC;
	uint8_t model     = 0x00;
	uint8_t hwrev     = 0x00;

	#define NOISE_FLOOR_SAMPLES 128
	int     noise_floor     = -292;
    int     current_rssi    = -292;
	int		last_rssi		= -292;
	uint8_t last_rssi_raw   = 0x00;
	uint8_t last_snr_raw	= 0x80;
	uint8_t seq				= 0xFF;
	uint16_t read_len		= 0;
	uint16_t host_write_len = 0;

	// Incoming packet buffer
	uint8_t pbuf[MTU];

	// KISS command buffer
	uint8_t cmdbuf[CMD_L];

	// LoRa transmit buffer
	uint8_t tbuf[MTU];

	uint32_t stat_rx		= 0;
	uint32_t stat_tx		= 0;

	#define STATUS_INTERVAL_MS 3
	#if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
	  #define DCD_SAMPLES 2500
		#define UTIL_UPDATE_INTERVAL_MS 1000
		#define UTIL_UPDATE_INTERVAL (UTIL_UPDATE_INTERVAL_MS/STATUS_INTERVAL_MS)
		#define AIRTIME_LONGTERM 3600
		#define AIRTIME_LONGTERM_MS (AIRTIME_LONGTERM*1000)
		#define AIRTIME_BINLEN_MS (STATUS_INTERVAL_MS*DCD_SAMPLES)
		#define AIRTIME_BINS ((AIRTIME_LONGTERM*1000)/AIRTIME_BINLEN_MS)
		bool util_samples[DCD_SAMPLES];
		uint16_t airtime_bins[AIRTIME_BINS];
		float longterm_bins[AIRTIME_BINS];
		int dcd_sample = 0;
		float local_channel_util = 0.0;
		float total_channel_util = 0.0;
		float longterm_channel_util = 0.0;
		float airtime = 0.0;
		float longterm_airtime = 0.0;
		#define current_airtime_bin(void) (millis()%AIRTIME_LONGTERM_MS)/AIRTIME_BINLEN_MS
	#endif
	// Airtime limits, as a fraction of the window spent transmitting. 0.0
	// disables a limit entirely.
	//
	// The long-term window is AIRTIME_LONGTERM (3600 s), which is exactly the
	// window EU 868 duty-cycle rules are written against. 867.2 MHz sits in
	// sub-band g3 (867-868.6 MHz), where the limit is 1%.
	//
	// ---------------------------------------------------------------------
	// DISABLED BY DEFAULT -- lab/bench builds only. MUST be enabled to ship.
	// ---------------------------------------------------------------------
	//
	// Enforcement is left off here because it throttles the bench: 1% of an
	// hour is 36 seconds of transmit time, and serving one NomadNet page costs
	// roughly a second at BW250/SF7, so a back-to-back test run exhausts the
	// budget in minutes and every subsequent failure looks like a radio fault
	// rather than a deliberate airtime_lock.
	//
	// For any build that leaves the bench, set the long-term limit to the legal
	// value for the region. At 867.2 MHz that is EU 868 sub-band g3 -- 1%:
	//
	//     -DRADIO_DUTY_CYCLE_LONGTERM=0.01f
	//
	// The #ifndef guards exist precisely so a production environment in
	// platformio.ini can override without touching this file. Exceeding the
	// duty cycle is illegal and degrades the band for every other node, so
	// treat enabling it as a release requirement, not an optimisation.
	//
	// The enforcement machinery itself is always compiled in and always
	// accounting: AIRTIME_LONGTERM is 3600 s (exactly the window the rules are
	// written against) and the [duty] telemetry reports accumulation whether or
	// not a limit is set -- so bench runs still show what a real deployment
	// would have spent.
	#ifndef RADIO_DUTY_CYCLE_SHORTTERM
	#define RADIO_DUTY_CYCLE_SHORTTERM 0.0f        // disabled
	#endif
	#ifndef RADIO_DUTY_CYCLE_LONGTERM
	#define RADIO_DUTY_CYCLE_LONGTERM 0.0f         // disabled -- set 0.01f to ship
	#endif
	float st_airtime_limit = RADIO_DUTY_CYCLE_SHORTTERM;
	float lt_airtime_limit = RADIO_DUTY_CYCLE_LONGTERM;
	bool airtime_lock = false;

	bool stat_signal_detected   = false;
	bool stat_signal_synced     = false;
	bool stat_rx_ongoing        = false;
	bool dcd                    = false;
	bool dcd_led                = false;
	bool dcd_waiting            = false;
	long dcd_wait_until         = 0;
	uint16_t dcd_count          = 0;
	uint16_t dcd_threshold      = 2;

	uint32_t status_interval_ms = STATUS_INTERVAL_MS;
	uint32_t last_status_update = 0;
	uint32_t last_dcd = 0;

    // Power management
    #define BATTERY_STATE_UNKNOWN     0x00
    #define BATTERY_STATE_DISCHARGING 0x01
    #define BATTERY_STATE_CHARGING    0x02
    #define BATTERY_STATE_CHARGED     0x03
    bool battery_installed = false;
    bool battery_indeterminate = false;
    bool external_power = false;
    bool battery_ready = false;
    float battery_voltage = 0.0;
    float battery_percent = 0.0;
    uint8_t battery_state = 0x00;
    uint8_t display_intensity = 0xFF;
    uint8_t display_addr = 0xFF;
    volatile bool display_updating = false;
    bool display_blanking_enabled = false;
    bool display_diagnostics = true;    
    bool device_init_done = false;
    bool eeprom_ok = false;
    bool firmware_update_mode = false;
    bool serial_in_frame = false;

	// Boot flags
	#define START_FROM_BOOTLOADER 0x01
	#define START_FROM_POWERON    0x02
	#define START_FROM_BROWNOUT   0x03
	#define START_FROM_JTAG       0x04

#endif
