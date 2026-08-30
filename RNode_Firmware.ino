// Copyright (C) 2024-2026, Mark Qvist and Chad Attermann

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

// CBA Reticulum includes must come before local to avoid collision with local defines
#ifdef HAS_RNS
#include <microReticulum.h>
#include "Provisioning.h"
#include "RadioPresets.h"
#if defined(RRC_HUB)
#include "RRCHub.h"
#endif
#if defined(LXMF_PROPAGATION_NODE)
#include "LXMFPropagation.h"
#include "LXMFPeerSync.h"
// Accessors for Provisioning.cpp. Defined here because that translation unit
// cannot see the LXMF headers' types.
uint32_t lxmf_peer_count();
uint32_t lxmf_pn_store_count();
uint32_t lxmf_announces_propagation();
uint32_t lxmf_announces_any();
#endif
#if defined(LORA_TRANSPORT)
#include "LoRaInterface.h"
#endif
#if defined(UDP_TRANSPORT)
#include "UDPInterface.h"
#endif
// Not nested in the UDP guard: the BLE peer interface has nothing to do with
// UDP, and the portable build has no UDP at all -- so nesting it there silently
// dropped the include and the class with it.
#if defined(BLE_PEER_TRANSPORT)
#include "BLEPeerInterface.h"
#endif
#if defined(TCP_SERVER_TRANSPORT)
#include "TCPServerInterface.h"
#endif
#ifdef URTN_STATS_PAGES
#include "Pages.h"
#endif
#endif // HAS_RNS
#ifdef HAS_GPIO
#include "GPIO.h"
#endif
#ifdef HAS_BME
#include "BME680.h"
#endif

#include <Arduino.h>
#include <SPI.h>
#include "Utilities.h"
#include "DeviceUID.h"
#include "Platform.h"
#include "WebSocketConsole.h"

#if MODEM == MODEM_RUNTIME
#include "native/LoRaFactory.h"
#include "native/PinMap.h"
#include "native/config.h"
#endif

// CBA SD
#if HAS_SDCARD
#include <SD.h>
SPIClass SDSPI(HSPI);
#endif

#if MCU_VARIANT == MCU_ESP32
  #include <esp_task_wdt.h>
#endif

// WDT timeout
#define WDT_TIMEOUT 60  // seconds

FIFOBuffer serialFIFO;
uint8_t serialBuffer[CONFIG_UART_BUFFER_SIZE+1];

// Inbound byte sink used by WebSocketConsole (and any other non-polled
// transport). Drops the byte if the FIFO is full — same behavior as the
// existing buffer_serial() code paths for the polled sources.
extern "C" void serial_fifo_push(uint8_t byte) {
  if (!fifo_isfull(&serialFIFO)) fifo_push(&serialFIFO, byte);
}

FIFOBuffer16 packet_starts;
uint16_t packet_starts_buf[CONFIG_QUEUE_MAX_LENGTH+1];

FIFOBuffer16 packet_lengths;
uint16_t packet_lengths_buf[CONFIG_QUEUE_MAX_LENGTH+1];

uint8_t packet_queue[CONFIG_QUEUE_SIZE];

volatile uint8_t queue_height = 0;
volatile uint16_t queued_bytes = 0;
volatile uint16_t queue_cursor = 0;
volatile uint16_t current_packet_start = 0;
volatile bool serial_buffering = false;
#if HAS_BLUETOOTH || HAS_BLE == true
  bool bt_init_ran = false;
#endif

#if PLATFORM == PLATFORM_NATIVE
bool kiss_framed_logs = false;
#else
bool kiss_framed_logs = true;
#endif
bool nomadnet_enabled = true;
RNS::Destination nomadnet_destination = {RNS::Type::NONE};
char nomadnet_name[64];
// Reason for the most recent reset, captured in setup() before the filesystem is
// up and appended to ./bootlog.txt once it is. Persisted because replugging USB
// power-cycles the board, which would otherwise overwrite the evidence with a
// fresh POWERON.
const char* boot_reset_reason = "UNKNOWN";
#if defined(ESP32)
  // esp_reset_reason() reports POWERON for an EN-pin reset as well as a genuine
  // power cycle. RTC memory narrows that down, but not as far as first assumed:
  // pulling EN low powers down the RTC domain too, so a button/RTS reset clears
  // it exactly like a real power loss. What this actually distinguishes is a
  // SOFTWARE reset (ESP.restart -- RTC survives, so the previous run's length is
  // carried across) from a HARDWARE one (EN pin, brownout, or the rail dropping,
  // which are indistinguishable from each other). Still worth having: it says
  // whether a silent restart came from firmware or from the power/reset path.
  #define RTC_BOOT_MAGIC 0x52414431UL   // 'RAD1'
  RTC_NOINIT_ATTR uint32_t rtc_boot_magic;
  RTC_NOINIT_ATTR uint32_t rtc_last_uptime_s;
  // Boots since the rail was last lost. Survives a software reset and not a
  // power cycle, which is exactly the distinction wanted: a board that restarts
  // itself climbs this while a board that was unplugged starts over.
  RTC_NOINIT_ATTR uint32_t rtc_boot_count;
  // Not static: Provisioning.cpp reads these to expose them as metrics, and
  // internal linkage would leave the one question they answer unanswerable
  // without attaching a console -- which resets the board and destroys it.
  bool     boot_rail_lost   = true;
  uint32_t boot_prev_uptime = 0;
  uint32_t boot_count       = 0;
#endif

#if HAS_CONSOLE
  #include "Console.h"
#endif

#if PLATFORM == PLATFORM_ESP32 || PLATFORM == PLATFORM_NRF52 || PLATFORM == PLATFORM_NATIVE
  #define MODEM_QUEUE_SIZE 8
  typedef struct {
          size_t len;
          int rssi;
          int snr_raw;
          uint8_t data[];
  } modem_packet_t;
  static xQueueHandle modem_packet_queue = NULL;
#endif

char sbuf[128];

#if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
  bool packet_ready = false;
#endif

#if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
void update_csma_parameters();
#endif

// CBA Forward function declarations for CPP compatibility
void serial_interrupt_init();
void validate_status();
void update_radio_lock();
void transmit(uint16_t size);
void update_airtime();
void update_modem_status();
void buffer_serial();
void serial_poll();
void work_while_waiting();

#ifdef HAS_RNS
/*
// CBA LoRa interface
class LoRaInterface : public RNS::InterfaceImpl {
public:
	LoRaInterface(const char *name) : RNS::InterfaceImpl(name) {
		_IN = true;
		_OUT = true;
		_HW_MTU = 508;
    _bitrate = lora_bitrate;
	}
	LoRaInterface() : LoRaInterface("LoRaInterface") {}
	virtual ~LoRaInterface() {
		_name = "deleted";
	}
protected:
	virtual void handle_incoming(const RNS::Bytes& data) {
    TRACEF("LoRaInterface.handle_incoming: (%u bytes) data: %s", data.size(), data.toHex().c_str());
    TRACE("LoRaInterface.handle_incoming: sending packet to rns...");
    try {
      InterfaceImpl::handle_incoming(data);
    }
    catch (const std::bad_alloc&) {
      ERROR("LoRaInterface::handle_incoming: bad_alloc - out of memory");
    }
    catch (std::exception& e) {
      ERRORF("LoRaInterface::handle_incoming: %s", e.what());
    }
  }
	virtual bool send_outgoing(const RNS::Bytes& data) {
    // CBA NOTE header will be addded later by transmit function
    TRACEF("LoRaInterface.send_outgoing: (%u bytes) data: %s", data.size(), data.toHex().c_str());
    try {
      TRACE("LoRaInterface.send_outgoing: adding packet to outgoing queue...");
      for (size_t i = 0; i < data.size(); i++) {
          if (queue_height < CONFIG_QUEUE_MAX_LENGTH && queued_bytes < CONFIG_QUEUE_SIZE) {
              queued_bytes++;
              packet_queue[queue_cursor++] = data.data()[i];
              if (queue_cursor == CONFIG_QUEUE_SIZE) queue_cursor = 0;
          }
      }
      if (!fifo16_isfull(&packet_starts) && queued_bytes < CONFIG_QUEUE_SIZE) {
          uint16_t s = current_packet_start;
          int16_t e = queue_cursor-1; if (e == -1) e = CONFIG_QUEUE_SIZE-1;
          uint16_t l;

          if (s != e) {
              l = (s < e) ? e - s + 1 : CONFIG_QUEUE_SIZE - s + e + 1;
          } else {
              l = 1;
          }

          if (l >= MIN_L) {
              queue_height++;

              fifo16_push(&packet_starts, s);
              fifo16_push(&packet_lengths, l);

              current_packet_start = queue_cursor;
          }

      }
      // Perform post-send housekeeping
      InterfaceImpl::handle_outgoing(data);
    }
    catch (const std::bad_alloc&) {
      ERROR("LoRaInterface::send_outgoing: bad_alloc - out of memory");
      return false;
    }
    catch (std::exception& e) {
      ERRORF("LoRaInterface::send_outgoing: %s", e.what());
      return false;
    }
    return true;
  }
};
*/

// CBA RNS
RNS::Reticulum reticulum(RNS::Type::NONE);
RNS::Interface lora_interface(RNS::Type::NONE);
#if defined(UDP_TRANSPORT)
RNS::Interface udp_interface(RNS::Type::NONE);
#endif
#if defined(BLE_PEER_TRANSPORT)
// Accessors for Provisioning.cpp, which cannot see this translation unit's
// types. Null-safe so they are readable before the interface has started.
uint32_t ble_peer_packets_in();
uint32_t ble_peer_packets_out();
uint32_t ble_peer_dropped();
bool     ble_peer_started();
uint32_t ble_peer_last_in();
const char* ble_peer_last_in_hex();
uint32_t ble_peer_last_out();
uint32_t ble_peer_mtu();
uint32_t ble_peer_keepalives();
uint32_t ble_peer_identity_writes();
const char* ble_peer_last_frag_hdr();
uint32_t ble_peer_frag_lone();
uint32_t ble_peer_frag_start();
// The node as a BLE peer. Distinct from BLESerial, which is the RNode/KISS
// modem role: this one joins a phone to the mesh through this node instead of
// handing it the radio. See BLEPeerInterface.h.
RNS::Interface ble_peer_interface(RNS::Type::NONE);
BLEPeerInterface* ble_peer_impl = nullptr;
uint32_t ble_peer_packets_in()  { return ble_peer_impl ? ble_peer_impl->packets_in()  : 0; }
uint32_t ble_peer_packets_out() { return ble_peer_impl ? ble_peer_impl->packets_out() : 0; }
uint32_t ble_peer_dropped()     { return ble_peer_impl ? ble_peer_impl->fragments_dropped() : 0; }
bool     ble_peer_started()     { return ble_peer_impl ? ble_peer_impl->started() : false; }
uint32_t ble_peer_last_in()     { return ble_peer_impl ? ble_peer_impl->last_in_size() : 0; }
const char* ble_peer_last_in_hex() { return ble_peer_impl ? ble_peer_impl->last_in_hex() : ""; }
uint32_t ble_peer_last_out()    { return ble_peer_impl ? ble_peer_impl->last_out_size() : 0; }
uint32_t ble_peer_mtu()         { return ble_peer_impl ? ble_peer_impl->last_mtu() : 0; }
uint32_t ble_peer_keepalives()  { return ble_peer_impl ? ble_peer_impl->keepalives_in() : 0; }
uint32_t ble_peer_identity_writes() { return ble_peer_impl ? ble_peer_impl->identity_writes() : 0; }
const char* ble_peer_last_frag_hdr() { return ble_peer_impl ? ble_peer_impl->last_frag_hdr() : ""; }
uint32_t ble_peer_frag_lone()   { return ble_peer_impl ? ble_peer_impl->frag_lone() : 0; }
uint32_t ble_peer_frag_start()  { return ble_peer_impl ? ble_peer_impl->frag_start() : 0; }
#endif

// Accessors for Provisioning.cpp, which cannot see the LXMF headers' types.
// Guarded on the propagation node, NOT on BLE_PEER_TRANSPORT: under the BLE
// guard these vanished from Rev 2 and every board without BLE, which still run
// a propagation node and still register these provisioning fields.
#if defined(LXMF_PROPAGATION_NODE)
char lxmf_static_peer[33] = {0};
uint32_t lxmf_peer_count()            { return (uint32_t)lxmf_peers().size(); }
uint32_t lxmf_pn_store_count()        { return (uint32_t)lxmf_store_index.size(); }
uint32_t lxmf_announces_propagation() { return lxmf_peer_announces_filtered(); }
uint32_t lxmf_announces_any()         { return lxmf_peer_announces_any(); }
uint32_t lxmf_sync_attempt_count()    { return lxmf_sync_attempts(); }
uint32_t lxmf_sync_link_count()       { return lxmf_sync_links_up(); }
uint32_t lxmf_sync_offer_count()      { return lxmf_sync_offers(); }
uint32_t lxmf_sync_response_count()   { return lxmf_sync_responses(); }
uint32_t lxmf_sync_sent_count()       { return lxmf_sync_sent(); }
uint32_t lxmf_sync_last_error()       { return lxmf_sync_error_byte(); }
uint32_t lxmf_sync_last_resp_size()   { return lxmf_sync_resp_size(); }
uint32_t lxmf_sync_last_outcome()     { return lxmf_sync_outcome(); }
#endif // LXMF_PROPAGATION_NODE
#if defined(TCP_SERVER_TRANSPORT)
RNS::Interface tcp_server_interface(RNS::Type::NONE);
#endif
#if defined(LXMF_PROPAGATION_NODE)
RNS::Destination lxmf_propagation_destination(RNS::Type::NONE);
std::vector<LXMFEntry> lxmf_store_index;
#endif
#if defined(RNS_USE_FS)
  // CBA microStore
  #if MCU_VARIANT == MCU_ESP32
    #if defined(USTORE_USE_SD)
      #include <microStore/Adapters/SDFileSystem.h>
      microStore::FileSystem filesystem{microStore::Adapters::SDFileSystem(SDCARD_SCLK, SDCARD_MISO, SDCARD_MOSI, SDCARD_CS)};
    #else
      //#include <microStore/Adapters/SPIFFSFileSystem.h>
      //microStore::FileSystem filesystem{microStore::Adapters::SPIFFSFileSystem()};
      //#include <microStore/Adapters/LittleFSFileSystem.h>
      //microStore::FileSystem filesystem{microStore::Adapters::LittleFSFileSystem()};
      #include <microStore/Adapters/PosixFileSystem.h>
      microStore::FileSystem filesystem{microStore::Adapters::PosixFileSystem()};
    #endif
  #elif MCU_VARIANT == MCU_NRF52
    #include <microStore/Adapters/InternalFSFileSystem.h>
    #include <microStore/Adapters/FlashFSFileSystem.h>
    microStore::FileSystem filesystem{microStore::Adapters::InternalFSFileSystem()};
  #else
    #include <microStore/Adapters/PosixFileSystem.h>
    microStore::FileSystem filesystem{microStore::Adapters::PosixFileSystem()};
  #endif
  #else // RNS_USE_FS
    #include <microStore/Adapters/NoopFileSystem.h>
    microStore::FileSystem filesystem{microStore::Adapters::NoopFileSystem()};
  #endif // RNS_USE_FS
#endif  // HAS_RNS
// CBA logger callback
void on_log(const char* msg, RNS::LogLevel level) {
  if (kiss_framed_logs) {
    // Compose "<timestamp> [<level>] <msg>" into a stack buffer to avoid
    // String heap allocation. 256 bytes covers the longest practical line.
    char line[256];
    int n = snprintf(line, sizeof(line), "%s [%s] %s",
                     RNS::getTimeString(),
                     RNS::getLevelName(level),
                     msg);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(line)) n = sizeof(line) - 1;
    kiss_indicate_log(line, (size_t)n);
  }
  else {
    // Using individual Serial.print statements to avoid memory allocation for String
    Serial.print(RNS::getTimeString());
    Serial.print(" [");
    Serial.print(RNS::getLevelName(level));
    Serial.print("] ");
    Serial.println(msg);
    Serial.flush();
  }

#ifdef HAS_SDCARD
	File file = SD.open("/logfile.txt", FILE_APPEND);
	if (file) {
    file.write((uint8_t*)msg, strlen(msg));
    file.close();
  }
#endif  // HAS_SDCARD
}

// CBA receive packet callback
void on_receive_packet(const RNS::Bytes& raw, const RNS::Interface& interface) {
#ifdef HAS_SDCARD
  TRACE("Logging receive packet to SD");
  String line = RNS::getTimeString() + String(" recv: ") + String(raw.toHex().c_str()) + "\n";
	File file = SD.open("./tracefile.txt", FILE_APPEND);
	if (file) {
    file.write((uint8_t*)line.c_str(), line.length());
    file.close();
  }
	RNS::Packet packet(raw);
	if (packet.unpack()) {
    String line = RNS::getTimeString() + String(" recv: ") + String(packet.dumpString().c_str()) + "\n";
    File file = SD.open("./tracedetails.txt", FILE_APPEND);
    if (file) {
      file.write((uint8_t*)line.c_str(), line.length());
      file.close();
    }
	}
#endif  // HAS_SDCARD
#if PLATFORM == PLATFORM_NATIVE
  String line = RNS::getTimeString() + String(" RECV: ") + String(raw.toHex().c_str()) + "\n";
	microStore::File file = filesystem.open("./tracefile.txt", microStore::File::ModeAppend);
	if (file) {
    file.write((uint8_t*)line.c_str(), line.length());
    file.close();
  }
	RNS::Packet packet(raw);
	if (packet.unpack()) {
    String line = RNS::getTimeString() + String(" RECV: ") + String(packet.dumpString().c_str()) + "\n";
  	microStore::File file = filesystem.open("./tracedetails.txt", microStore::File::ModeAppend);
    if (file) {
      file.write((uint8_t*)line.c_str(), line.length());
      file.close();
    }
	}
#endif
}

// CBA transmit packet callback
void on_transmit_packet(const RNS::Bytes& raw, const RNS::Interface& interface) {
#ifdef HAS_SDCARD
  TRACE("Logging transmit packet to SD");
  String line = RNS::getTimeString() + String(" send: ") + String(raw.toHex().c_str()) + "\n";
	File file = SD.open("/tracefile.txt", FILE_APPEND);
	if (file) {
    file.write((uint8_t*)line.c_str(), line.length());
    file.close();
  }
	RNS::Packet packet(raw);
	if (packet.unpack()) {
    String line = RNS::getTimeString() + String(" send: ") + String(packet.dumpString().c_str()) + "\n";
    File file = SD.open("/tracedetails.txt", FILE_APPEND);
    if (file) {
      file.write((uint8_t*)line.c_str(), line.length());
      file.close();
    }
	}
#endif  // HAS_SDCARD
#if PLATFORM == PLATFORM_NATIVE
  String line = RNS::getTimeString() + String(" SEND: ") + String(raw.toHex().c_str()) + "\n";
	microStore::File file = filesystem.open("./tracefile.txt", microStore::File::ModeAppend);
	if (file) {
    file.write((uint8_t*)line.c_str(), line.length());
    file.close();
  }
	RNS::Packet packet(raw);
	if (packet.unpack()) {
    String line = RNS::getTimeString() + String(" SEND: ") + String(packet.dumpString().c_str()) + "\n";
  	microStore::File file = filesystem.open("./tracedetails.txt", microStore::File::ModeAppend);
    if (file) {
      file.write((uint8_t*)line.c_str(), line.length());
      file.close();
    }
	}
#endif
}

// For redirecting stdout to KISS framed logs
#if MCU_VARIANT == MCU_ESP32

#include <esp_vfs.h>
#include <sys/errno.h>

static ssize_t kiss_vfs_write(int fd, const void* data, size_t size) {
  if (kiss_framed_logs) {
    kiss_indicate_log((const char*)data, size);
  } else {
    Serial.write((const uint8_t*)data, size);
    Serial.flush();
  }
  return (ssize_t)size;
}

static int kiss_vfs_open(const char* path, int flags, int mode) { return 0; }
static int kiss_vfs_close(int fd) { return 0; }
static int kiss_vfs_fstat(int fd, struct stat* st) {
  memset(st, 0, sizeof(*st));
  st->st_mode = S_IFCHR;
  return 0;
}

static void install_kiss_stdout(void) {
  static const esp_vfs_t kiss_vfs = {
    .flags = ESP_VFS_FLAG_DEFAULT,
    .write = &kiss_vfs_write,
    .open  = &kiss_vfs_open,
    .close = &kiss_vfs_close,
    .fstat = &kiss_vfs_fstat,
  };
  if (esp_vfs_register("/dev/kiss", &kiss_vfs, NULL) != ESP_OK) return;
  FILE* f = fopen("/dev/kiss/0", "w");
  if (!f) return;
  setvbuf(f, NULL, _IONBF, 0);   // no buffering — one printf → one KISS frame
  stdout = f;
  // Optional: also redirect stderr and any ESP_LOG* output.
  stderr = f;
  esp_log_set_vprintf(&vprintf);  // ensures ESP_LOG* also goes through stdout
}

#elif MCU_VARIANT == MCU_NRF52

// The Adafruit nRF52 core ships its own strong `_write` in main.cpp (retargets
// newlib's printf() to Serial). Defining a second strong `_write` here would
// produce a "multiple definition" link error, so instead we ask the linker to
// wrap every reference to `_write` and route it at __wrap__write below.
// Requires `-Wl,--wrap=_write` in the link flags (set in platformio.ini for
// nRF52 envs).
extern "C" int __wrap__write(int file, const void *ptr, size_t len) {
  (void)file;
  if (kiss_framed_logs) {
    kiss_indicate_log((const char*)ptr, len);
    return (int)len;
  }
  int n = Serial.write((const uint8_t*)ptr, len);
  Serial.flush();
  return n;
}

static void install_kiss_stdout(void) {}

#else

extern "C" int _write(int file, char *ptr, int len) {
  size_t wrote = 0;
  if (kiss_framed_logs) {
    kiss_indicate_log(ptr, len);
    wrote = len;
  }
  else {
    wrote = Serial.write(ptr, len);
    Serial.flush();
  }
  return wrote;
}

static void install_kiss_stdout(void) {}

#endif

#if defined(RNS_USE_FS)
void dump_filesystem(const char* basepath, uint8_t level = 0, uint8_t max_level = 0) {
  if (max_level > 0 && level > max_level) return;
  char prefix[17] = "";
  for (uint8_t index = 0; index < level && index < 8; index++) {
    prefix[index*2] = ' ';
    prefix[index*2+1] = ' ';
    prefix[index*2+2] = '\0';
  }
  filesystem.listDirectory(basepath, [&](const char* name) -> void {
    // Adapter callbacks receive bare basenames — join with basepath before
    // re-querying or recursing, and avoid emitting "//" when basepath is "/".
    // Skip "." and ".." directories
    if (name[0] == '.') return;
    char fullpath[96];
    const bool root = (basepath[0] == '/' && basepath[1] == '\0');
    if (root) snprintf(fullpath, sizeof(fullpath), "/%s", name);
    else snprintf(fullpath, sizeof(fullpath), "%s/%s", basepath, name);
    if (filesystem.isDirectory(fullpath)) {
      TRACEF("%s%s:", prefix, name);
      dump_filesystem(fullpath, level + 1, max_level);
    }
    else {
      TRACEF("%s%s", prefix, name);
    }
  });
}
#endif

// The Reticulum stack runs inside loop(), so every Link callback -- RRC's
// packet handling, LXMF's store writes, NomadNet page rendering -- executes on
// the Arduino loop task and then descends through Packet, Destination, sha256
// and malloc. The 8 KB default was not enough: answering an RRC HELLO overflowed
// it and panicked the board in the allocator roughly once a minute, which looked
// like a hub that could be discovered but never joined.
#if MCU_VARIANT == MCU_ESP32
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
#endif

// Smallest free stack seen on the loop task. Sampled every pass, so it records
// the worst case across every Reticulum callback rather than a quiet moment.
// A stack overflow here presents as a panic inside malloc with no indication of
// the real cause, so this number is worth watching before it reaches zero.
// Sentinel is the maximum on purpose: the running minimum is computed with `<`,
// so seeding this at 0 would mean no sample is ever smaller and the metric would
// report 0 for ever. It is only exposed on platforms that can sample it.
uint32_t loop_stack_free_min = 0xFFFFFFFF;

static void sample_loop_stack() {
#if MCU_VARIANT == MCU_ESP32
  const uint32_t free_bytes = uxTaskGetStackHighWaterMark(NULL);
  if (free_bytes < loop_stack_free_min) loop_stack_free_min = free_bytes;
#endif
}

void setup() {

  // Initialise serial communication
  memset(serialBuffer, 0, sizeof(serialBuffer));
  fifo_init(&serialFIFO, serialBuffer, CONFIG_UART_BUFFER_SIZE);

  #if MCU_VARIANT == MCU_ESP32
    // Must precede Serial.begin(): HardwareSerial refuses to resize the RX
    // buffer once the port is running, which silently leaves it at the Arduino
    // default of 256 bytes. Only visible on builds where Serial is a real UART
    // (ARDUINO_USB_CDC_ON_BOOT=0); the USB CDC class has no such restriction.
    Serial.setRxBufferSize(CONFIG_UART_BUFFER_SIZE);
  #endif

  Serial.begin(serial_baudrate);

  // Redirect stdout to KISS framed logs
  install_kiss_stdout();

  // CBA Safely wait for serial initialization
  while (!Serial) {
    if (millis() > 2000) {
      break;
    }
    delay(10);
  }
  // Native USB opens reset the Tracker V2. Keep its startup short enough to
  // answer rnodeconf's EEPROM request before the utility times out.
  #if BOARD_MODEL != BOARD_HELTEC_TRACKER_V2
    delay(2000);
  #endif

#if defined(ESP32)
  // Why did we just boot? On a headless node this is the only cheap way to tell
  // a clean power-up from a panic, a watchdog bite, a brownout, or the
  // RNS_LOW_MEMORY_REBOOT path (which calls ESP.restart() -> ESP_RST_SW when
  // free heap drops to <=2%). Printed unconditionally, not behind a log level.
  {
    const char* rr;
    switch (esp_reset_reason()) {
      case ESP_RST_POWERON:  rr = "POWERON";              break;
      case ESP_RST_SW:       rr = "SW (ESP.restart)";     break;
      case ESP_RST_PANIC:    rr = "PANIC (exception)";    break;
      case ESP_RST_INT_WDT:  rr = "INT_WDT";              break;
      case ESP_RST_TASK_WDT: rr = "TASK_WDT";             break;
      case ESP_RST_WDT:      rr = "WDT (other)";          break;
      case ESP_RST_BROWNOUT: rr = "BROWNOUT";             break;
      case ESP_RST_DEEPSLEEP:rr = "DEEPSLEEP";            break;
      case ESP_RST_EXT:      rr = "EXT (reset pin)";      break;
      case ESP_RST_SDIO:     rr = "SDIO";                 break;
      // ESP_RST_UNKNOWN is what a host-initiated USB-JTAG reset reports on the
      // S3; it is not an error, and is distinct from BROWNOUT/PANIC/SW.
      default:               rr = "UNKNOWN";              break;
    }
    boot_rail_lost   = (rtc_boot_magic != RTC_BOOT_MAGIC);
    boot_prev_uptime = boot_rail_lost ? 0 : rtc_last_uptime_s;
    rtc_boot_count   = boot_rail_lost ? 1 : (rtc_boot_count + 1);
    boot_count       = rtc_boot_count;
    rtc_boot_magic    = RTC_BOOT_MAGIC;
    rtc_last_uptime_s = 0;
    if (boot_rail_lost) {
      printf("[boot] reset reason: %s (%d), RTC cleared -> hardware reset "
             "(EN pin, brownout or power loss)\n", rr, (int)esp_reset_reason());
    } else {
      printf("[boot] reset reason: %s (%d), software reset, previous run %lus\n",
             rr, (int)esp_reset_reason(), (unsigned long)boot_prev_uptime);
    }
    boot_reset_reason = rr;
  }
#endif
#ifdef HAS_RNS
  printf("Total SRAM:  %7u bytes\n", RNS::Utilities::Memory::heap_size());
  printf("Free SRAM:   %7u bytes\n", RNS::Utilities::Memory::heap_available());
#endif
#if defined(ESP32)
	printf("Total PSRAM: %7u bytes\n", ESP.getPsramSize());
  #if defined(BOARD_HAS_PSRAM) && defined(PSRAM_MALLOC_THRESHOLD)
    // The framework ships CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096, so only
    // allocations >= 4 KB ever reach PSRAM. The RNS stack's pressure is
    // thousands of *small* allocations -- Bytes buffers, packets, map nodes,
    // strings -- all of which stay on internal heap and eventually trigger the
    // OOM restarts documented in the handoff (§11). RNS_PSRAM_ALLOCATOR only
    // covers a handful of STL containers, not these.
    //
    // Lower the threshold so ordinary malloc spills to PSRAM. Drivers needing
    // DMA- or ISR-safe memory request it explicitly via heap_caps_* and are
    // unaffected. PSRAM is slower than internal SRAM, so this trades some
    // throughput for headroom; raise the threshold if that matters.
    if (ESP.getPsramSize() > 0) {
      heap_caps_malloc_extmem_enable(PSRAM_MALLOC_THRESHOLD);
      printf("[psram] malloc threshold set to %u bytes\n", (unsigned)PSRAM_MALLOC_THRESHOLD);
    }
  #endif
#endif
	//printf("Total flash: %zu bytes\n", RNS::Utilities::OS::storage_size());

  device_uid_init();
  INFOF("Device UID:  %s", device_uid_str);

#ifdef HAS_GPIO
  GPIO::init();
  INFO("GPIO control initialized");
#endif

#ifdef HAS_BME
  BME680::init();
#endif

  // Configure WDT
  #if MCU_VARIANT == MCU_ESP32
    #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
      esp_task_wdt_config_t wdt_config = {
          .timeout_ms     = WDT_TIMEOUT * 1000,
          .idle_core_mask = 0,
          .trigger_panic  = true,
      };
      // In IDF 5.x, the framework initializes TWDT before setup(); reconfigure
      // it with our timeout rather than calling init() (which would fail with
      // "TWDT already initialized").  Fall back to init() if not yet started.
      if (esp_task_wdt_reconfigure(&wdt_config) == ESP_ERR_INVALID_STATE) {
          esp_task_wdt_init(&wdt_config);
      }
    #else
      esp_task_wdt_init(WDT_TIMEOUT, true); // enable panic so ESP32 restarts
    #endif
    esp_task_wdt_add(NULL);               // add current thread to WDT watch
  #elif MCU_VARIANT == MCU_NRF52
    NRF_WDT->CONFIG         = 0x01;           // Configure WDT to run when CPU is asleep
    NRF_WDT->CRV            = WDT_TIMEOUT * 32768 + 1; // set timeout
    NRF_WDT->RREN           = 0x01;           // Enable the RR[0] reload register
    NRF_WDT->TASKS_START    = 1;              // Start WDT
  #endif

  #if MCU_VARIANT == MCU_ESP32
    boot_seq();
    EEPROM.begin(EEPROM_SIZE);

    #if BOARD_MODEL == BOARD_TDECK
      pinMode(pin_poweron, OUTPUT);
      digitalWrite(pin_poweron, HIGH);

      pinMode(SD_CS, OUTPUT);
      pinMode(DISPLAY_CS, OUTPUT);
      digitalWrite(SD_CS, HIGH);
      digitalWrite(DISPLAY_CS, HIGH);

      pinMode(DISPLAY_BL_PIN, OUTPUT);
    #endif
  #endif

  #if MCU_VARIANT == MCU_NRF52
    #if BOARD_MODEL == BOARD_TECHO
      delay(200);
      pinMode(PIN_VEXT_EN, OUTPUT);
      digitalWrite(PIN_VEXT_EN, HIGH);
      pinMode(pin_btn_usr1, INPUT_PULLUP);
      pinMode(pin_btn_touch, INPUT_PULLUP);
      pinMode(PIN_LED_RED, OUTPUT);
      pinMode(PIN_LED_GREEN, OUTPUT);
      pinMode(PIN_LED_BLUE, OUTPUT);
      delay(200);
    #endif

    if (!eeprom_begin()) { Serial.write("EEPROM initialisation failed.\r\n"); }
  #endif

  // Seed the PRNG for CSMA R-value selection
  #if MCU_VARIANT == MCU_ESP32
    // On ESP32, get the seed value from the
    // hardware RNG
    unsigned long seed_val = (unsigned long)esp_random();
  #elif MCU_VARIANT == MCU_NRF52
    // On nRF, get the seed value from the
    // hardware RNG
    unsigned long seed_val = get_rng_seed();
  #else
    // Otherwise, get a pseudo-random seed
    // value from an unconnected analog pin
    //
    // CAUTION! If you are implementing the
    // firmware on a platform that does not
    // have a hardware RNG, you MUST take
    // care to get a seed value with enough
    // entropy at each device reset!
    unsigned long seed_val = analogRead(0);
  #endif
  randomSeed(seed_val);

  #if HAS_NP
    led_init();
  #endif

  #if MCU_VARIANT == MCU_NRF52 && HAS_NP == true
    boot_seq();
  #endif

  #if BOARD_MODEL != BOARD_RAK4631 && BOARD_MODEL != BOARD_RAK3401 && BOARD_MODEL != BOARD_HELTEC_T114 && BOARD_MODEL != BOARD_TECHO && BOARD_MODEL != BOARD_T3S3 && BOARD_MODEL != BOARD_TBEAM_S_V1 && BOARD_MODEL != BOARD_HELTEC32_V4 && BOARD_MODEL != BOARD_HELTEC_TRACKER_V2 && BOARD_MODEL != BOARD_RAD01_REV1 && BOARD_MODEL != BOARD_RAD01_REV2
    // Some boards need to wait until the hardware UART is set up before booting
    // the full firmware. In the case of the RAK4631, RAK3401, and Heltec T114,
    // the line below will wait until a serial connection is actually established
    // with a master. Thus, it is disabled on this platform.
    while (!Serial);
  #endif

  serial_interrupt_init();

  // Configure input and output pins
  #if HAS_INPUT
    input_init();
  #endif

  #if HAS_NP == false
    // -1 = "no LED" — skip pinMode rather than pass -1 (which Portduino's
    // pin_size_t-cast turns into 255 and asserts > NUM_GPIOS).
    if (pin_led_rx >= 0) pinMode(pin_led_rx, OUTPUT);
    if (pin_led_tx >= 0) pinMode(pin_led_tx, OUTPUT);
  #endif

  #if HAS_TCXO == true
    if (pin_tcxo_enable != -1) {
        pinMode(pin_tcxo_enable, OUTPUT);
        digitalWrite(pin_tcxo_enable, HIGH);
    }
  #endif

  // Initialise buffers
  memset(pbuf, 0, sizeof(pbuf));
  memset(cmdbuf, 0, sizeof(cmdbuf));
  
  memset(packet_queue, 0, sizeof(packet_queue));

  memset(packet_starts_buf, 0, sizeof(packet_starts_buf));
  fifo16_init(&packet_starts, packet_starts_buf, CONFIG_QUEUE_MAX_LENGTH);
  
  memset(packet_lengths_buf, 0, sizeof(packet_starts_buf));
  fifo16_init(&packet_lengths, packet_lengths_buf, CONFIG_QUEUE_MAX_LENGTH);

  #if PLATFORM == PLATFORM_ESP32 || PLATFORM == PLATFORM_NRF52 || PLATFORM == PLATFORM_NATIVE
    modem_packet_queue = xQueueCreate(MODEM_QUEUE_SIZE, sizeof(modem_packet_t*));
  #endif

  // LoRa modem init — gated so [env:native-macos] (which removes
  // LORA_TRANSPORT via build_unflags) launches with no radio, no
  // SPI activity, and modem_installed stays false (default from Config.h).
  // Downstream `if (modem_installed)` checks handle the no-radio case.
  #if defined(LORA_TRANSPORT)
  // Set chip select, reset and interrupt
  // pins for the LoRa module
  #if MODEM == MODEM_RUNTIME
  // Native target: factory instantiates the runtime-selected driver and
  // performs its driver-native setPins() with the right arity using the
  // pin_* globals already populated by native_pinmap::apply().
  LoRa = native_lora::create_radio(current_modem);
  // Apply rnoded.conf SX126x overrides before preInit() / begin(). Both
  // setters are virtual no-ops on non-SX126x drivers, but we still gate
  // on current_modem so the byte conversion only runs when relevant.
  if (LoRa != nullptr && current_modem == SX1262) {
    float v = native_config::g_config.dio3_tcxo_voltage;
    if (v > 0.0f) {
      // Snap to nearest of the 8 discrete MODE_TCXO_* bytes the SX1262
      // accepts (see sx126x.cpp MODE_TCXO_*_6X defines).
      uint8_t mode;
      if      (v >= 3.15f) mode = 0x07; // 3.3 V
      else if (v >= 2.30f) mode = 0x06; // 2.4 / 2.7 / 3.0 V
      else if (v >= 2.00f) mode = 0x03; // 2.2 V
      else if (v >= 1.75f) mode = 0x02; // 1.8 V
      else if (v >= 1.65f) mode = 0x01; // 1.7 V
      else                 mode = 0x00; // 1.6 V
      LoRa->setTcxoVoltage(mode);
    }
    LoRa->setDio2AsRfSwitch(native_config::g_config.dio2_as_rf_switch);
  }
  #elif MODEM == SX1276 || MODEM == SX1278
  LoRa->setPins(pin_cs, pin_reset, pin_dio, pin_busy);
  #elif MODEM == SX1262
  LoRa->setPins(pin_cs, pin_reset, pin_dio, pin_busy, pin_rxen, pin_txen);
  #elif MODEM == SX1280
  LoRa->setPins(pin_cs, pin_reset, pin_dio, pin_busy, pin_rxen, pin_txen);
  #endif

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    init_channel_stats();

    #if BOARD_MODEL == BOARD_T3S3
      #if MODEM == SX1280
        delay(300);
        LoRa->reset();
        delay(100);
      #endif
    #endif

    #if BOARD_MODEL == BOARD_XIAO_S3
      // Improve wakeup from sleep
      delay(300);
      LoRa->reset();
      delay(100);
    #endif

    // Check installed transceiver chip and
    // probe boot parameters.
    #if MCU_VARIANT == MCU_NATIVE
      // Power up any external supplies the chip needs before preInit()
      // can read sync-word registers. Without this, a HAT with an EN
      // line (e.g. RAK13302) sees no power and the probe returns "No
      // radio module found". Leave the pins asserted after a successful
      // preInit so the chip stays powered through startRadio(); only
      // deassert on probe failure.
      native_pinmap::assert_radio_enable_pins();
      // Pulse NRESET before the probe. On embedded targets pinMode()
      // defaults reset-line GPIOs to OUTPUT-HIGH at boot, so the chip is
      // already out of reset by the time preInit() runs; under libgpiod
      // the line stays in high-Z INPUT until reset() drives it, so the
      // chip can be stuck in reset when preInit() reads syncword regs.
      // Same pattern as the BOARD_T3S3 / BOARD_XIAO_S3 blocks above.
      delay(10);
      LoRa->reset();
      delay(10);
    #endif
    if (LoRa->preInit()) {
      modem_installed = true;

      #if HAS_INPUT
        // Skip quick-reset console activation
      #else
        uint32_t lfr = LoRa->getFrequency();
        if (lfr == 0) {
          // Normal boot
        } else if (lfr == M_FRQ_R) {
          // Quick reboot
          #if HAS_CONSOLE
            if (rtc_get_reset_reason(0) == POWERON_RESET) {
              // Deliberate: a quick power cycle is the gesture for "give me the
              // console". But it also fires after a UART flash, because the
              // radio keeps its register contents through download mode and the
              // following power-on looks identical to a double tap. The radio is
              // then held off for the whole session, which presents as a node
              // that joins the network over WiFi and is simply deaf on RF. Say
              // so, rather than leaving it to be inferred from a dead LED.
              console_active = true;
              printf("[boot] quick-reboot detected: console activated, radio held "
                     "OFF for this session (full power-down clears this)\r\n");
            }
          #endif
        } else {
          // Unknown boot
        }
        LoRa->setFrequency(M_FRQ_S);
      #endif

    } else {
      modem_installed = false;
      #if MCU_VARIANT == MCU_NATIVE
        native_pinmap::deassert_radio_enable_pins();
      #endif
    }
  #else
    // Older variants only came with SX1276/78 chips,
    // so assume that to be the case for now.
    modem_installed = true;
  #endif
  #endif // defined(LORA_TRANSPORT)

  #if HAS_DISPLAY
    #if HAS_EEPROM
    if (EEPROM.read(eeprom_addr(ADDR_CONF_DSET)) != CONF_OK_BYTE) {
    #elif MCU_VARIANT == MCU_NRF52
    if (eeprom_read(eeprom_addr(ADDR_CONF_DSET)) != CONF_OK_BYTE) {
    #endif
      eeprom_update(eeprom_addr(ADDR_CONF_DSET), CONF_OK_BYTE);
      #if BOARD_MODEL == BOARD_TECHO
        eeprom_update(eeprom_addr(ADDR_CONF_DINT), 0x03);
      #else
        eeprom_update(eeprom_addr(ADDR_CONF_DINT), 0xFF);
      #endif
    }
    #if BOARD_MODEL == BOARD_TECHO
      display_add_callback(work_while_waiting);
    #endif

    display_unblank();
    disp_ready = display_init();
    update_display();
  #endif

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    #if HAS_PMU == true
      pmu_ready = init_pmu();
    #endif

    // Seed only erased Tracker V2 settings. Explicit user choices (including
    // disabling BLE) are represented by non-0xFF values and remain intact.
    // Radio parameters intentionally remain unset because frequency and power
    // require a region-appropriate choice during onboarding.
    #if BOARD_MODEL == BOARD_HELTEC_TRACKER_V2 && HAS_EEPROM
      if (EEPROM.read(eeprom_addr(ADDR_CONF_BT)) == 0xFF) {
        eeprom_update(eeprom_addr(ADDR_CONF_BT), BT_ENABLE_BYTE);
      }
      if (EEPROM.read(eeprom_addr(ADDR_CONF_WIFI)) == 0xFF) {
        eeprom_update(eeprom_addr(ADDR_CONF_WIFI), WR_WIFI_OFF);
      }
    #endif

    #if HAS_BLUETOOTH || HAS_BLE == true
      bt_init();
      bt_init_ran = true;
    #endif

    if (console_active) {
      #if HAS_CONSOLE
        console_start();
      #else
        kiss_indicate_reset();
      #endif
    } else {
      #if HAS_WIFI
        wifi_mode = EEPROM.read(eeprom_addr(ADDR_CONF_WIFI));
        if (wifi_mode == WR_WIFI_STA || wifi_mode == WR_WIFI_AP) { wifi_remote_init(); }
      #endif
      kiss_indicate_reset();
    }
  #endif

  #if defined(ENABLE_WEBSOCKETS) && __has_include(<WiFi.h>)
    // KISS-over-WebSocket on port 81, alongside HTTP on 80. The browser
    // page served by `server` connects back to this with `new WebSocket(
    // "ws://" + location.hostname + ":81")`. Single client at a time —
    // same model as Remote.h's KISS-over-TCP.
    // Provisioning-backed builds start this only after the secure-node policy
    // has loaded. Otherwise an enabled secure posture would still expose KISS
    // briefly during boot.
    #if !defined(HAS_PROVISIONING)
      ws_console::init(81);
    #endif
  #endif

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    #if MODEM == MODEM_RUNTIME
      // Native runtime selection: SX1280 (2.4 GHz) skips interference avoidance.
      if (current_modem == SX1280) {
        avoid_interference = false;
      } else {
        #if HAS_EEPROM
          uint8_t ia_conf = EEPROM.read(eeprom_addr(ADDR_CONF_DIA));
          if (ia_conf == 0x00) { avoid_interference = true; }
          else                 { avoid_interference = false; }
        #else
          avoid_interference = false;
        #endif
      }
    #elif MODEM == SX1280
      avoid_interference = false;
    #else
      #if HAS_EEPROM
        uint8_t ia_conf = EEPROM.read(eeprom_addr(ADDR_CONF_DIA));
        if (ia_conf == 0x00) { avoid_interference = true; }
        else                 { avoid_interference = false; }
      #elif MCU_VARIANT == MCU_NRF52
        uint8_t ia_conf = eeprom_read(eeprom_addr(ADDR_CONF_DIA));
        if (ia_conf == 0x00) { avoid_interference = true; }
        else                 { avoid_interference = false; }
      #endif
    #endif
  #endif

  // Support force-disable of interference avoidance
#ifdef DISABLE_IA
  avoid_interference = false;
#endif

  // Validate board health, EEPROM and config
  validate_status();

  #if defined(LORA_TRANSPORT)
  if (op_mode != MODE_TNC) LoRa->setFrequency(0);
  #endif

// CBA SD
#ifdef HAS_SDCARD
  pinMode(SDCARD_MISO, INPUT_PULLUP);
  SDSPI.begin(SDCARD_SCLK, SDCARD_MISO, SDCARD_MOSI, SDCARD_CS);
  if (!SD.begin(SDCARD_CS, SDSPI)) {
      printf("setupSDCard FAIL\n");
  } else {
      uint32_t cardSize = SD.cardSize() / (1024 * 1024);
      printf("setupSDCard PASS . SIZE = %u GB\n", cardSize / 1024.0);
      SD.remove("/logfile");
      SD.remove("/logfile.txt");
      SD.remove("/tracefile");
      SD.remove("/tracedetails");
      SD.remove("/tracefile.txt");
      SD.remove("/tracedetails.txt");
      printf("DIR: /\n");
      File root = SD.open("/");
      File file = root.openNextFile();
      while(file){
          printf("  FILE: %s\n", file.name());
          file = root.openNextFile();
      }
  }
  delay(3000);
#endif

#ifdef HAS_RNS

  // Set sane default memory limits based on hardware-specific availability
  // (note these may be adjusted dynamically based on detected hardware below)
  RNS::Transport::path_table_maxsize(URTN_PATH_TABLE_MAX_RECS);
  RNS::Transport::announce_table_maxsize(50);
  RNS::Transport::hashlist_maxsize(50);
  RNS::Identity::known_destinations_maxsize(50);
  RNS::Transport::max_pr_tags(50);
  RNS::Reticulum::clean_interval(60*15); // 60 minutes
  //RNS::Reticulum::clean_interval(60*15); // 15 minutes
  RNS::Reticulum::persist_interval(60*60); // 60 minutes
  //RNS::Reticulum::persist_interval(60*10); // 10 minutes
  //RNS::Reticulum::persist_interval(60); // 1 minute

  // Configure callbacks
  RNS::set_log_callback(&on_log);
  RNS::Transport::set_receive_packet_callback(on_receive_packet);
  RNS::Transport::set_transmit_packet_callback(on_transmit_packet);

  try {
    // CBA Init filesystem
    HEAD("Initializing filesystem...", RNS::LOG_TRACE);
#if BOARD_MODEL == BOARD_RAK4631 || BOARD_MODEL == BOARD_RAK3401
    bool init_success = false;
    // Attempt to initialize RAK15001 flash
    {
      TRACE("Looking for RAK15001 flash...");
      static const SPIFlash_Device_t device = RAK15001;
      // CBA NOTE: RAK base boards generally *share* the same chip select (CS/SS) across all module slots.
      // SS below is expected to be configured as the "external" SPI bus chip select.
      filesystem = microStore::Adapters::FlashFSFileSystem(&device, SS);
      if (filesystem.init()) {
        TRACE("Initialized RAK15001 flash");
        init_success = true;
        // Raise path store limits to account for larger external flash size
        RNS::Transport::path_table_maxsize(500);
        RNS::Transport::path_store_segment_size(24576);
        RNS::Transport::path_store_segment_count(8);
      }
    }
    // Attempt to initialize W25Q128 flash
    if (!init_success) {
      TRACE("Looking for W25Q128 flash...");
      static const SPIFlash_Device_t device = W25Q128;
      // CBA NOTE: RAK base boards generally *share* the same chip select (CS/SS) across all module slots.
      // This particular module is expected to be on an *alternate* chip select gpio WB_IO1.
      filesystem = microStore::Adapters::FlashFSFileSystem(&device, WB_IO1);
      if (filesystem.init()) {
        TRACE("Initialized W25Q128 flash");
        init_success = true;
        // Raise path store limits to account for larger external flash size
        RNS::Transport::path_table_maxsize(500);
        RNS::Transport::path_store_segment_size(24576);
        RNS::Transport::path_store_segment_count(8);
      }
    }
    // If no other initialize attempts succeeded then fallback to internal flash
    if (!init_success) {
      TRACE("Using internal flash...");
      filesystem = microStore::Adapters::InternalFSFileSystem();
      if (!filesystem.init()) WARNING("Failed to initialize filesystem!");
      else TRACE("Initialized internal flash");
    }
#else
    if (!filesystem.init()) WARNING("Failed to initialize filesystem!");
#endif

    // Persist the reset reason. A headless node that dies on a wall plug tells
    // you nothing when you replug it into a host, because that power-cycles it
    // and esp_reset_reason() then reports POWERON. This file keeps the history:
    // repeated BROWNOUT entries mean a power-margin problem, PANIC means a
    // crash, SW means something called ESP.restart() (RNS_LOW_MEMORY_REBOOT).
    {
      const char* bootlog = "./bootlog.txt";
      if (filesystem.exists(bootlog) && filesystem.size(bootlog) > 4096) {
        filesystem.remove(bootlog);   // keep it bounded; oldest history is least useful
      }
      microStore::File bl = filesystem.open(bootlog, microStore::File::ModeAppend, true);
      if (bl) {
        char line[96];
        if (boot_rail_lost) {
          snprintf(line, sizeof(line), "boot reason=%s prev=hw-reset\n",
                   boot_reset_reason);
        } else {
          snprintf(line, sizeof(line), "boot reason=%s prev=%lus\n",
                   boot_reset_reason, (unsigned long)boot_prev_uptime);
        }
        bl.write(line);
        bl.close();
      }
      // Echo the whole history on every boot. Without this the log is written
      // but unreadable without a file-transfer path -- and the entire point is
      // to see it after replugging a board that died in the field.
      microStore::File rd = filesystem.open(bootlog, microStore::File::ModeRead);
      if (rd) {
        // Read into a buffer and emit whole lines. stdout is unbuffered and
        // KISS-framed, so a putchar loop would emit one frame per character and
        // arrive shredded. The buffer must cover the writer's whole 4096-byte
        // cap plus one line: a smaller one echoes the OLDEST entries and
        // silently hides the newest, which are the only ones that matter when
        // you are reading this after a board died in the field.
        static char rdbuf[4353];
        size_t n = rd.read((uint8_t*)rdbuf, sizeof(rdbuf) - 1);
        rd.close();
        if (n > 0 && n != (size_t)-1) {
          rdbuf[n] = 0;
          printf("[boot] --- bootlog history (%u bytes) ---\n", (unsigned)n);
          char* line = rdbuf;
          while (line && *line) {
            char* nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (*line) printf("[boot] %s\n", line);
            line = nl ? nl + 1 : nullptr;
          }
          printf("[boot] --- end bootlog ---\n");
        }
      }
    }

    // Remove legacy files
    filesystem.remove("./destination_table");
    filesystem.remove("./path_store_index.dat");
    filesystem.remove("./path_store_0.dat");
    filesystem.remove("./path_store_1.dat");
    filesystem.remove("./path_store_2.dat");
    filesystem.remove("./path_store_3.dat");
    filesystem.remove("./path_store_4.dat");
    filesystem.remove("./path_store_5.dat");
    filesystem.remove("./path_store_6.dat");
    filesystem.remove("./path_store_7.dat");
    if (filesystem.isDirectory("./cache")) {
      filesystem.listDirectory("./cache", [&](const char* name) -> void {
        char rmpath[64];
        snprintf(rmpath, sizeof(rmpath), "./cache/%s", name);
        if (filesystem.isDirectory(rmpath)) filesystem.rmdir(rmpath);
        else                                filesystem.remove(rmpath);
      });
      filesystem.rmdir("./cache");
    }

#if PLATFORM != PLATFORM_NATIVE
    // If filesystem is essentially full then clear all path store files
    if (filesystem.storageAvailable() < 1024) {
      WARNING("FileSystem is full, clearing space...");
      // CBA Delete the path store index file to force a rebuild
      filesystem.remove("/path_store/index.dat");
      // CBA Remove all path store data files
      filesystem.remove("/path_store/seg0.dat");
      filesystem.remove("/path_store/seg1.dat");
      filesystem.remove("/path_store/seg2.dat");
      filesystem.remove("/path_store/seg3.dat");
      filesystem.remove("/path_store/seg4.dat");
      filesystem.remove("/path_store/seg5.dat");
      filesystem.remove("/path_store/seg6.dat");
      filesystem.remove("/path_store/seg7.dat");
    }
#endif

    TRACE("Registering filesystem...");
    RNS::Utilities::OS::register_filesystem(filesystem);

#if defined(RNS_USE_FS)
#if 0
    filesystem.format();
#endif
#if 1
    TRACE("Listing filesystem...");
    dump_filesystem("./", 1, 2);
#endif
#endif // RNS_USE_FS

    // CBA Start RNS
    //if (hw_ready) {
    if (true) {

#if defined(LORA_TRANSPORT)
      lora_interface = new LoRaInterface();
      // Provisioning default
      lora_interface.mode(RNS::Type::Interface::MODE_GATEWAY);
#endif
#if HAS_WIFI && defined(UDP_TRANSPORT)
      if (wifi_mode != WR_WIFI_OFF) {
        udp_interface = new UDPInterface();
        // Provisioning default
        udp_interface.mode(RNS::Type::Interface::MODE_GATEWAY);
      }
#endif
#if defined(BLE_PEER_TRANSPORT)
      ble_peer_impl = new BLEPeerInterface();
      ble_peer_interface = ble_peer_impl;
      // A peer over BLE joins the mesh as a peer, exactly as one over TCP or
      // LoRa does -- the BLE link is a transport, not a client port.
      //
      // This was MODE_ACCESS_POINT, which looks reasonable and is fatal:
      // Transport::outbound blocks every announce broadcast on an AP-mode
      // interface ("Blocking announce broadcast ... due to AP mode"). The
      // phone therefore learned no paths, so it saw no announces, could reach
      // no pages, and could deliver no messages, while the link itself looked
      // perfectly healthy.
      ble_peer_interface.mode(RNS::Type::Interface::MODE_GATEWAY);
#endif
#if HAS_WIFI && defined(TCP_SERVER_TRANSPORT)
      // Serves attached clients (residents' phones on the SoftAP, or hosts on
      // the LAN). Created whenever WiFi is on in either mode -- SoftAP is the
      // disaster case and STA the everyday one, and the interface does not care
      // which. The listener itself binds later, from poll(), once WiFi is up.
      if (wifi_mode != WR_WIFI_OFF) {
        tcp_server_interface = new TCPServerInterface();
        tcp_server_interface.mode(RNS::Type::Interface::MODE_GATEWAY);
      }
#endif

      // Provisioning default
      reticulum.transport_enabled(true);
      // Provisioning default
      reticulum.probe_destination_enabled(true);
      // Provisioning default
      reticulum.remote_management_enabled(true);

#ifdef URTN_STATS_PAGES
      // Provisioning default
      snprintf(nomadnet_name, sizeof(nomadnet_name), "microReticulum Node [%s]", device_uid_str);
#endif

#ifdef HAS_PROVISIONING
      // Bring the Provisioning subsystem up. Loads persisted MsgPack files
      // (including the radio + general namespaces registered here) and fires
      // FF_LIVE_APPLY setters. FF_REBOOT_REQUIRED setters only fire if the
      // disk value differs from the declared default — so on a fresh device
      // the lora_* globals stay at their Config.h defaults until either
      // eeprom_conf_load() runs or a Provisioning SetState arrives.
      // CBA NOTE: All app-default-values must be set *before* calling init_provisioning so that they take effect for fresh installs
      HEAD("Initializing Provisioning subsystem...", RNS::LOG_TRACE);
      init_provisioning();
      auto& prov = RNS::Provisioning::Provisioner::instance();
#endif

      //reticulum.clear_caches();

      HEAD("Starting RNS...\r\n", RNS::LOG_VERBOSE);
#if defined(RNS_MEM_LOG)
      RNS::loglevel(RNS::LOG_MEM);
#else
      RNS::loglevel(RNS::LOG_TRACE);
#endif

#if defined(LORA_TRANSPORT)
      HEAD("Registering LoRA Interface...", RNS::LOG_TRACE);
      RNS::Transport::register_interface(lora_interface);
      TRACEF("LoRaInterface hash: %s", lora_interface.get_hash().toHex().c_str());
#endif
#if HAS_WIFI && defined(UDP_TRANSPORT)
      if (wifi_mode != WR_WIFI_OFF) {
        HEAD("Registering UDP Interface...", RNS::LOG_TRACE);
        RNS::Transport::register_interface(udp_interface);
        TRACEF("UDPInterface hash: %s", udp_interface.get_hash().toHex().c_str());
      }
#endif
#if defined(BLE_PEER_TRANSPORT)
      HEAD("Registering BLE Peer Interface...", RNS::LOG_TRACE);
      RNS::Transport::register_interface(ble_peer_interface);
      TRACEF("BLEPeerInterface hash: %s", ble_peer_interface.get_hash().toHex().c_str());
#endif
#if HAS_WIFI && defined(TCP_SERVER_TRANSPORT)
      if (wifi_mode != WR_WIFI_OFF && tcp_server_interface) {
        HEAD("Registering TCP Server Interface...", RNS::LOG_TRACE);
        RNS::Transport::register_interface(tcp_server_interface);
        TRACEF("TCPServerInterface hash: %s", tcp_server_interface.get_hash().toHex().c_str());
      }
#endif

      HEAD("Creating Reticulum instance...", RNS::LOG_TRACE);
      reticulum = RNS::Reticulum();
      // CBA NOTE: `transport_enabled` needs to always be overridden to false when op_mode is not MODE_TNC
printf("[init] hw_ready: %u\n", hw_ready);
printf("[init] op_mode: %U\n", op_mode);
      if (op_mode != MODE_TNC) {
        INFO("Not in TNC mode, transport will be disabled");
        reticulum.transport_enabled(false);
      }
      reticulum.start();

      // Set loop callback only after the Reticulum instance is started
      // (to avoid looping without a completely initialized instance)
      RNS::Utilities::OS::set_loop_callback(&loop);

      // CBA load/create local destination for admin node
#if 0
      RNS::Identity identity = {RNS::Type::NONE};
      std::string local_identity_path = RNS::Reticulum::_storagepath + "/local_identity";
      if (RNS::Utilities::OS::file_exists(local_identity_path.c_str())) {
        identity = RNS::Identity::from_file(local_identity_path.c_str());
      }
      if (!identity) {
        RNS::verbose("No valid local identity in storage, creating...");
        identity = RNS::Identity();
        identity.to_file(local_identity_path.c_str());
      }
      else {
        RNS::verbose("Loaded local identity from storage");
      }
      RNS::Destination destination(identity, RNS::Type::Destination::IN, RNS::Type::Destination::SINGLE, "rnstransport", "local");
#endif
      //RNS::Destination destination(RNS::Transport::identity(), RNS::Type::Destination::IN, RNS::Type::Destination::SINGLE, "rnstransport", "local");

#ifdef URTN_STATS_PAGES
      if (nomadnet_enabled) {
        // Create an IN/SINGLE destination on the NomadNet aspect, so
        // clients (like a Python NomadNet browser) can find
        // us by aspect/announce and open a Link.
        nomadnet_destination = RNS::Destination(
          RNS::Transport::identity(),
          RNS::Type::Destination::IN,
          RNS::Type::Destination::SINGLE,
          "nomadnetwork",
          "node"
        );

        // Register the page handler. ALLOW_ALL because page browsing is open
        // to anyone who can reach the node, just like a Python NomadNet
        // node's default policy.
        //nomadnet_destination.register_request_handler("/page/index.mu", serve_page, RNS::Type::Destination::ALLOW_LIST, RNS::Transport::remote_management_allowed());
        nomadnet_destination.register_request_handler("/page/index.mu", serve_page, RNS::Type::Destination::ALLOW_ALL);

#if defined(LXMF_PROPAGATION_NODE)
        // LXMF propagation node: store-and-forward so a message survives its
        // recipient being asleep. See LXMFPropagation.h for why this is worth
        // doing on a microcontroller at all.
        lxmf_propagation_destination = RNS::Destination(
          RNS::Transport::identity(),
          RNS::Type::Destination::IN,
          RNS::Type::Destination::SINGLE,
          LXMF_APP_NAME,
          LXMF_PN_ASPECT
        );
        // ALLOW_ALL matches Python: a propagation node accepts offers from
        // anyone. It cannot read what it stores, so there is nothing to gate --
        // abuse is bounded by the stamp cost and the store limits instead.
        lxmf_propagation_destination.register_request_handler(
          LXMF_OFFER_PATH, lxmf_offer_request, RNS::Type::Destination::ALLOW_ALL);
        lxmf_propagation_destination.register_request_handler(
          LXMF_GET_PATH, lxmf_message_get_request, RNS::Type::Destination::ALLOW_ALL);
        // Outbound half: listen for other propagation nodes so we can offer
        // them what we hold. Serving /offer alone never makes two nodes
        // converge, because neither ever initiates.
        lxmf_peer_sync_begin();
        printf("[lxmf] propagation node destination <%s>\n",
               lxmf_propagation_destination.hash().toHex().c_str());
        lxmf_propagation_destination.set_link_established_callback(lxmf_link_established);
        lxmf_store_load();
#endif
        // These pages expose device telemetry (heap, flash, interfaces, transport
        // metrics). Gated to the remote-management allow list by default; a peer
        // that has not identified is refused before serve_page is ever called,
        // which is indistinguishable from the request never arriving. Define
        // NOMADNET_PAGES_ALLOW_ALL to open them to any peer on the mesh -- useful
        // for diagnosis, but it publishes device internals to everyone.
#ifdef NOMADNET_PAGES_ALLOW_ALL
        nomadnet_destination.register_request_handler("/page/stack.mu", serve_page, RNS::Type::Destination::ALLOW_ALL);
        nomadnet_destination.register_request_handler("/page/device.mu", serve_page, RNS::Type::Destination::ALLOW_ALL);
#else
        nomadnet_destination.register_request_handler("/page/stack.mu", serve_page, RNS::Type::Destination::ALLOW_LIST, RNS::Transport::remote_management_allowed());
        nomadnet_destination.register_request_handler("/page/device.mu", serve_page, RNS::Type::Destination::ALLOW_LIST, RNS::Transport::remote_management_allowed());
#endif
#ifdef HAS_BME
        if (BME680::bme_installed) {
          nomadnet_destination.register_request_handler("/page/telemetry.mu", serve_page, RNS::Type::Destination::ALLOW_ALL);
        }
#endif

        // Announce once at startup so a client that's already listening can
        // discover us immediately. The node name is sent as the announce
        // app_data (plain UTF-8 bytes), matching nomadnet/Node.py:217-222 —
        // this is what other NomadNet clients show in their site listing.
        {
          NOTICEF("Announcing NomadNet site \"%s\" at destination <%s>", nomadnet_name, nomadnet_destination.hash().toHex().c_str());
          nomadnet_destination.announce(nomadnet_name);
        }
      }
#endif // URTN_STATS_PAGES

#if defined(RRC_HUB)
      // RRC is a separate Reticulum service from NomadNet pages and LXMF.
      // It intentionally shares the persistent transport identity so the
      // rrc.hub destination hash survives firmware updates and reboots.
      rrc_hub_begin(RNS::Transport::identity());
#endif

      HEAD("RNS is READY!", RNS::LOG_TRACE);
      if (op_mode == MODE_TNC) {
        HEAD("RNS transport mode is ENABLED", RNS::LOG_TRACE);
        TRACEF("Frequency: %d Hz", lora_freq);
        TRACEF("Bandwidth: %d Hz", lora_bw);
        TRACEF("Spreading Factor: %d", lora_sf);
        TRACEF("Coding Rate: %d", lora_cr);
        TRACEF("TX Power: %d dBm", lora_txp);
        HEAD("RNS Transport is READY!", RNS::LOG_TRACE);
      }
      else {
        HEAD("RNS transport mode is DISABLED", RNS::LOG_INFO);
        HEAD("Configure TNC mode with radio configuration to enable RNS transport", RNS::LOG_INFO);
      }
    }
    else {
      HEAD("RNS is inoperable because hardware is not ready!", RNS::LOG_ERROR);
      HEAD("Check firmware signature and eeprom provisioning", RNS::LOG_ERROR);
      // CBA Clear cached files just in case cached files are responsible for failure
  		//reticulum.clear_caches();
    }
  }
  catch (const std::bad_alloc&) {
    ERROR("RNS startup failed: bad_alloc - out of memory");
  }
  catch (std::exception& e) {
    ERRORF("RNS startup failed: %s", e.what());
  }
#endif  // HAS_RNS
}

void lora_receive() {
  if (!implicit) {
    LoRa->receive();
  } else {
    LoRa->receive(implicit_l);
  }
}


// Does a host get to reconfigure this node's radio?
//
// MODE_HOST means the host owns the modem -- classic RNode, the board is a
// dumb radio. MODE_TNC means the node owns it: it runs its own Reticulum
// stack, its parameters come from provisioning, and a client attached to it is
// a client of the *node*, not the owner of its radio.
//
// That distinction was only half honoured. The setter was skipped in TNC mode,
// but the shadow variable was written anyway, so a host's value sat there and
// was applied at the next startRadio() -- silently retuning a node minutes or
// hours later. It is how a phone running an ordinary RNode client took Rev 1
// off the mesh: nothing failed, nothing logged, the node was simply gone.
//
// Refusing the write and answering with the value actually in force is both
// safer and more honest: a well-behaved host sees it did not take.
static bool host_may_set_radio() {
  if (op_mode == MODE_HOST) return true;
  return false;
}

// A host setting a parameter to the value it already has is not a change, and
// refusing it breaks every well-behaved RNode client.
//
// Those clients configure the radio and then read it back to validate. With a
// flat refusal the readback reports the node's own value, the comparison fails,
// and the client aborts with "Radio configuration validation failed" -- which
// is what happened to a correctly-configured phone whose only discrepancy was
// 171 Hz of SX1276 PLL granularity between 867,200,000 as requested and
// 867,199,829 as held.
//
// So an idempotent set is allowed through: it changes nothing, it lets a client
// that already agrees with the node complete its handshake, and a client that
// disagrees is still refused. The frequency tolerance covers synthesiser
// rounding only, not a genuinely different channel.
static bool radio_set_is_noop(uint32_t requested, uint32_t current,
                              uint32_t tolerance) {
  const uint32_t difference = (requested > current) ? (requested - current)
                                                    : (current - requested);
  return difference <= tolerance;
}

static void radio_config_refused(const char* what) {
  static uint32_t last_refusal = 0;
  // Rate-limited: an RNode client re-sends its whole configuration on every
  // connect, and a reconnect loop would otherwise fill the log.
  if (millis() - last_refusal < 2000) return;
  last_refusal = millis();
  printf("[radio] refused host %s: this node is in TNC mode and owns its "
         "radio config\n", what);
}

inline void kiss_write_packet() {

#if defined(HAS_RNS) && defined(LORA_TRANSPORT)
  if (host_write_len > 0) {
    printf("[radio] Received %d byte packet", host_write_len);
    // CBA send packet received over LoRa to RNS in addition to connected client
    RNS::Bytes data(pbuf, host_write_len);
    lora_interface.r_stat_rssi(last_rssi);
    lora_interface.r_stat_snr(((int8_t)last_snr_raw) / 4.0f);
    lora_interface.r_stat_q(get_quality());
    lora_interface.handle_incoming(data);
  }
#endif

  serial_write(FEND);
  serial_write(CMD_DATA);
  
  for (uint16_t i = 0; i < host_write_len; i++) {
    #if MCU_VARIANT == MCU_NRF52
      portENTER_CRITICAL();
      uint8_t byte = pbuf[i];
      portEXIT_CRITICAL();
    #else
      uint8_t byte = pbuf[i];
    #endif

    if (byte == FEND) { serial_write(FESC); byte = TFEND; }
    if (byte == FESC) { serial_write(FESC); byte = TFESC; }
    serial_write(byte);
  }

  serial_write(FEND);
  host_write_len = 0;

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    packet_ready = false;
  #endif

  #if MCU_VARIANT == MCU_ESP32
    #if HAS_BLE
      bt_flush();
    #endif
  #endif
}

inline void getPacketData(uint16_t len) {
  #if MCU_VARIANT != MCU_NRF52
    while (len-- && read_len < MTU) {
      pbuf[read_len++] = LoRa->read();
    }  
  #else
    BaseType_t int_mask = taskENTER_CRITICAL_FROM_ISR();
    while (len-- && read_len < MTU) {
      pbuf[read_len++] = LoRa->read();
    }
    taskEXIT_CRITICAL_FROM_ISR(int_mask);
  #endif
}

void ISR_VECT receive_callback(int packet_size) {
  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
    BaseType_t int_mask;
  #endif

  bool    ready    = false;
  if (!promisc) { // Not in promiscuous mode
    // The standard operating mode allows large
    // packets with a payload up to 500 bytes,
    // by combining two raw LoRa packets.
    // We read the 1-byte header and extract
    // packet sequence number and split flags
    uint8_t header   = LoRa->read(); packet_size--;
    uint8_t sequence = packetSequence(header);

    if (isSplitPacket(header) && seq == SEQ_UNSET) {
      // This is the first part of a split
      // packet, so we set the seq variable
      // and add the data to the buffer
      #if MCU_VARIANT == MCU_NRF52
        int_mask = taskENTER_CRITICAL_FROM_ISR(); read_len = 0; taskEXIT_CRITICAL_FROM_ISR(int_mask);
      #else
        read_len = 0;
      #endif
      
      seq = sequence;

      #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52 && MCU_VARIANT != MCU_NATIVE
        last_rssi = LoRa->packetRssi();
        last_snr_raw = LoRa->packetSnrRaw();
      #endif

      getPacketData(packet_size);

    } else if (isSplitPacket(header) && seq == sequence) {
      // This is the second part of a split
      // packet, so we add it to the buffer
      // and set the ready flag.
      #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52 && MCU_VARIANT != MCU_NATIVE
        last_rssi = (last_rssi+LoRa->packetRssi())/2;
        last_snr_raw = (last_snr_raw+LoRa->packetSnrRaw())/2;
      #endif

      getPacketData(packet_size);
      seq = SEQ_UNSET;
      ready = true;

    } else if (isSplitPacket(header) && seq != sequence) {
      // This split packet does not carry the
      // same sequence id, so we must assume
      // that we are seeing the first part of
      // a new split packet.
      #if MCU_VARIANT == MCU_NRF52
        int_mask = taskENTER_CRITICAL_FROM_ISR(); read_len = 0; taskEXIT_CRITICAL_FROM_ISR(int_mask);
      #else
        read_len = 0;
      #endif
      seq = sequence;

      #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52 && MCU_VARIANT != MCU_NATIVE
        last_rssi = LoRa->packetRssi();
        last_snr_raw = LoRa->packetSnrRaw();
      #endif

      getPacketData(packet_size);

    } else if (!isSplitPacket(header)) {
      // This is not a split packet, so we
      // just read it and set the ready
      // flag to true.

      if (seq != SEQ_UNSET) {
        // If we already had part of a split
        // packet in the buffer, we clear it.
        #if MCU_VARIANT == MCU_NRF52
          int_mask = taskENTER_CRITICAL_FROM_ISR(); read_len = 0; taskEXIT_CRITICAL_FROM_ISR(int_mask);
        #else
          read_len = 0;
        #endif
        seq = SEQ_UNSET;
      }

      #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52 && MCU_VARIANT != MCU_NATIVE
        last_rssi = LoRa->packetRssi();
        last_snr_raw = LoRa->packetSnrRaw();
      #endif

      getPacketData(packet_size);
      ready = true;
    }
  } else { // In promiscuous mode
    // In promiscuous mode, raw packets are
    // output directly to the host
    read_len = 0;

    #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52 && MCU_VARIANT != MCU_NATIVE
      last_rssi = LoRa->packetRssi();
      last_snr_raw = LoRa->packetSnrRaw();
      getPacketData(packet_size);

      // We first signal the RSSI of the
      // recieved packet to the host.
      kiss_indicate_stat_rssi();
      kiss_indicate_stat_snr();

      // And then write the entire packet
      kiss_write_packet();

    #else
      getPacketData(packet_size);
      packet_ready = true;
    #endif
  }

  if (ready) {
    #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52 && MCU_VARIANT != MCU_NATIVE
      // We first signal the RSSI of the
      // recieved packet to the host.
      kiss_indicate_stat_rssi();
      kiss_indicate_stat_snr();

      // And then write the entire packet
      host_write_len = read_len;
      kiss_write_packet(); read_len = 0;

    #else
      // Allocate packet struct, but abort if there
      // is not enough memory available.
      modem_packet_t *modem_packet = (modem_packet_t*)malloc(sizeof(modem_packet_t) + read_len);
      if(!modem_packet) { memory_low = true; return; }

      // Get packet RSSI and SNR
      #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NATIVE
        modem_packet->snr_raw = LoRa->packetSnrRaw();
        modem_packet->rssi = LoRa->packetRssi(modem_packet->snr_raw);
      #endif

      // Send packet to event queue, but free the
      // allocated memory again if the queue is
      // unable to receive the packet.
      modem_packet->len = read_len;
      memcpy(modem_packet->data, pbuf, read_len); read_len = 0;
      if (!modem_packet_queue || xQueueSendFromISR(modem_packet_queue, &modem_packet, NULL) != pdPASS) {
          free(modem_packet);
      }
    #endif
  }
}

bool startRadio() {
  update_radio_lock();
  if (!radio_online && !console_active) {
    if (!radio_locked && hw_ready) {
      #if MCU_VARIANT == MCU_NATIVE
        // Drive any configured radio_enable_pins to their active level
        // before the modem comes up so external rails (LDOs, TCXO supply,
        // PA bias) are stable before the SX126x probes them.
        native_pinmap::assert_radio_enable_pins();
      #endif
      if (!LoRa->begin(lora_freq)) {
        // The radio could not be started.
        // Indicate this failure over both the
        // serial port and with the onboard LEDs
        #if MCU_VARIANT == MCU_NATIVE
          native_pinmap::deassert_radio_enable_pins();
        #endif
        radio_error = true;
        kiss_indicate_error(ERROR_INITRADIO);
        // Bounded, not forever. led_indicate_error(0) never returns, so this
        // function could not report failure to its caller and the node simply
        // stopped -- main loop, BLE, watchdog and all -- displaying one LED
        // pattern until it was power-cycled.
        //
        // That defeats the recovery radio_rx_watchdog() was written for: it
        // reinitialises the modem when nothing demodulates and is explicitly
        // built to tolerate a start that keeps failing ("a permanently
        // unstartable radio retries on the normal interval"). It never got the
        // chance, because startRadio() hung before returning.
        //
        // A host-tethered modem can defensibly stop and wait to be noticed. A
        // standalone node on a battery cannot: observed in the field as a node
        // that meshed briefly, went quiet, and then wedged with a repeating LED
        // pattern and no BLE, needing a power cycle to recover.
        led_indicate_error(5);
        return false;
      } else {
        radio_online = true;
        // Clear the sticky failure flag. It gated the display's error state and
        // nothing ever reset it, so a node that recovered on a watchdog retry
        // kept reporting a radio fault it no longer had.
        radio_error = false;
        printf("[radio] startRadio OK at %lums\n", (unsigned long)millis());

        init_channel_stats();

        setTXPower();
        setBandwidth();
        setSpreadingFactor();
        setCodingRate();
        getFrequency();

        LoRa->enableCrc();
        LoRa->onReceive(receive_callback);
        lora_receive();

        // Flash an info pattern to indicate
        // that the radio is now on
        kiss_indicate_radiostate();
        led_indicate_info(3);
        return true;
      }

    } else {
      // Flash a warning pattern to indicate
      // that the radio was locked, and thus
      // not started
      printf("[radio] startRadio BLOCKED locked=%d hwr=%d at %lums\n",
             (int)radio_locked, (int)hw_ready, (unsigned long)millis());
      radio_online = false;
      kiss_indicate_radiostate();
      led_indicate_warning(3);
      return false;
    }
  } else {
    // If radio is already on, we silently
    // ignore the request.
    kiss_indicate_radiostate();
    return true;
  }
}

void stopRadio() {
  // Idempotent: LoRa->end() calls SPI.end() which nulls Portduino's
  // spiChip on native. The main loop's `else { stopRadio(); }` branch
  // fires every iteration while radio_online is false, so we must not
  // re-end an already-stopped radio — otherwise the next SPI access
  // (e.g. lora_receive() at the tail of flush_queue) asserts on a null
  // spiChip.
  #if defined(LORA_TRANSPORT)
  if (radio_online) {
    printf("[radio] stopRadio: shutting down a RUNNING radio at %lums\n",
           (unsigned long)millis());
    LoRa->end();
  }
  #endif
  radio_online = false;
  #if MCU_VARIANT == MCU_NATIVE
    // De-assert after LoRa->end() so SPI cleanup completes while supply
    // rails are still up — avoids transients on power-down.
    native_pinmap::deassert_radio_enable_pins();
  #endif
}

void update_radio_lock() {
  if (lora_freq != 0 && lora_bw != 0 && lora_txp != 0xFF && lora_sf != 0) {
    radio_locked = false;
  } else {
    radio_locked = true;
  }
}

bool queue_full() { return (queue_height >= CONFIG_QUEUE_MAX_LENGTH || queued_bytes >= CONFIG_QUEUE_SIZE); }

volatile bool queue_flushing = false;
void flush_queue(void) {
  if (!queue_flushing) {
    queue_flushing = true;
    led_tx_on();

    #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    while (!fifo16_isempty(&packet_starts)) {
    #else
    while (!fifo16_isempty_locked(&packet_starts)) {
    #endif

      uint16_t start = fifo16_pop(&packet_starts);
      uint16_t length = fifo16_pop(&packet_lengths);

      if (length >= MIN_L && length <= MTU) {
        for (uint16_t i = 0; i < length; i++) {
          uint16_t pos = (start+i)%CONFIG_QUEUE_SIZE;
          tbuf[i] = packet_queue[pos];
        }

        transmit(length);
      }
    }

    lora_receive(); led_tx_off();
  }

  queue_height = 0;
  queued_bytes = 0;

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    update_airtime();
  #endif

  queue_flushing = false;

  #if HAS_DISPLAY
    display_tx = true;
  #endif
}

void pop_queue() {
  if (!queue_flushing) {
    queue_flushing = true; led_tx_on();

    #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    if (!fifo16_isempty(&packet_starts)) {
    #else
    if (!fifo16_isempty_locked(&packet_starts)) {
    #endif

      uint16_t start = fifo16_pop(&packet_starts);
      uint16_t length = fifo16_pop(&packet_lengths);
      if (length >= MIN_L && length <= MTU) {
        for (uint16_t i = 0; i < length; i++) {
          uint16_t pos = (start+i)%CONFIG_QUEUE_SIZE;
          tbuf[i] = packet_queue[pos];
        }

        transmit(length);
      }
      queue_height -= 1;
      queued_bytes -= length;
    }

    lora_receive(); led_tx_off();
  }

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    update_airtime();
  #endif

  queue_flushing = false;

  #if HAS_DISPLAY
    display_tx = true;
  #endif
}

void add_airtime(uint16_t written) {
  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    float lora_symbols = 0;
    float packet_cost_ms = 0.0;
    int ldr_opt = 0; if (lora_low_datarate) ldr_opt = 1;

    #if MODEM == MODEM_RUNTIME
      if (current_modem == SX1276 || current_modem == SX1278) {
        lora_symbols += (8*written + PHY_CRC_LORA_BITS - 4*lora_sf + 8 + PHY_HEADER_LORA_SYMBOLS);
        lora_symbols /=                          4*(lora_sf-2*ldr_opt);
        lora_symbols *= lora_cr;
        lora_symbols += lora_preamble_symbols + 0.25 + 8;
        packet_cost_ms += lora_symbols * lora_symbol_time_ms;
      } else { // SX1262 / SX1280
        if (lora_sf < 7) {
          lora_symbols += (8*written + PHY_CRC_LORA_BITS - 4*lora_sf + PHY_HEADER_LORA_SYMBOLS);
          lora_symbols /=                              4*lora_sf;
          lora_symbols *= lora_cr;
          lora_symbols += lora_preamble_symbols + 2.25 + 8;
          packet_cost_ms += lora_symbols * lora_symbol_time_ms;
        } else {
          lora_symbols += (8*written + PHY_CRC_LORA_BITS - 4*lora_sf + 8 + PHY_HEADER_LORA_SYMBOLS);
          lora_symbols /=                         4*(lora_sf-2*ldr_opt);
          lora_symbols *= lora_cr;
          lora_symbols += lora_preamble_symbols + 0.25 + 8;
          packet_cost_ms += lora_symbols * lora_symbol_time_ms;
        }
      }
    #elif MODEM == SX1276 || MODEM == SX1278
      lora_symbols += (8*written + PHY_CRC_LORA_BITS - 4*lora_sf + 8 + PHY_HEADER_LORA_SYMBOLS);
      lora_symbols /=                          4*(lora_sf-2*ldr_opt);
      lora_symbols *= lora_cr;
      lora_symbols += lora_preamble_symbols + 0.25 + 8;
      packet_cost_ms += lora_symbols * lora_symbol_time_ms;

    #elif MODEM == SX1262 || MODEM == SX1280
      if (lora_sf < 7) {
        lora_symbols += (8*written + PHY_CRC_LORA_BITS - 4*lora_sf + PHY_HEADER_LORA_SYMBOLS);
        lora_symbols /=                              4*lora_sf;
        lora_symbols *= lora_cr;
        lora_symbols += lora_preamble_symbols + 2.25 + 8;
        packet_cost_ms += lora_symbols * lora_symbol_time_ms;

      } else {
        lora_symbols += (8*written + PHY_CRC_LORA_BITS - 4*lora_sf + 8 + PHY_HEADER_LORA_SYMBOLS);
        lora_symbols /=                         4*(lora_sf-2*ldr_opt);
        lora_symbols *= lora_cr;
        lora_symbols += lora_preamble_symbols + 0.25 + 8;
        packet_cost_ms += lora_symbols * lora_symbol_time_ms;
      }

    #endif

    uint16_t cb = current_airtime_bin();
    uint16_t nb = cb+1; if (nb == AIRTIME_BINS) { nb = 0; }
    airtime_bins[cb] += packet_cost_ms;
    airtime_bins[nb] = 0;

  #endif
}

void update_airtime() {
  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    uint16_t cb = current_airtime_bin();
    uint16_t pb = cb-1; if (cb-1 < 0) { pb = AIRTIME_BINS-1; }
    uint16_t nb = cb+1; if (nb == AIRTIME_BINS) { nb = 0; }
    airtime_bins[nb] = 0; airtime = (float)(airtime_bins[cb]+airtime_bins[pb])/(2.0*AIRTIME_BINLEN_MS);

    uint32_t longterm_airtime_sum = 0;
    for (uint16_t bin = 0; bin < AIRTIME_BINS; bin++) { longterm_airtime_sum += airtime_bins[bin]; }
    longterm_airtime = (float)longterm_airtime_sum/(float)AIRTIME_LONGTERM_MS;

    float longterm_channel_util_sum = 0.0;
    for (uint16_t bin = 0; bin < AIRTIME_BINS; bin++) { longterm_channel_util_sum += longterm_bins[bin]; }
    longterm_channel_util = (float)longterm_channel_util_sum/(float)AIRTIME_BINS;

    #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
      update_csma_parameters();
    #endif

    kiss_indicate_channel_stats();
  #endif
}

volatile uint32_t tx_calls = 0;
volatile uint32_t tx_blocked_csma = 0;
void transmit(uint16_t size) {
  if (radio_online) {
    tx_calls++;
    if (!promisc) {
      uint16_t  written = 0;
      uint8_t header  = random(256) & 0xF0;
      if (size > SINGLE_MTU - HEADER_L) { header = header | FLAG_SPLIT; }

      LoRa->beginPacket();
      LoRa->write(header); written++;

      for (uint16_t i=0; i < size; i++) {
        LoRa->write(tbuf[i]); written++;

        if (written == 255 && isSplitPacket(header)) {
          if (!LoRa->endPacket()) {
            kiss_indicate_error(ERROR_MODEM_TIMEOUT);
            kiss_indicate_error(ERROR_TXFAILED);
            led_indicate_error(5);
            #if MCU_VARIANT == MCU_NATIVE
              if (native_config::g_config.reboot_on_tx_failure) { hard_reset(); }
              else { LoRa->receive(); return; }
            #elif REBOOT_ON_TX_FAILURE
              hard_reset();
            #else
              LoRa->receive(); return;   // drop the packet, keep the node up
            #endif
          }

          add_airtime(written);
          LoRa->beginPacket();
          LoRa->write(header);
          written = 1;
          printf("[radio] Sent %d byte packet (split)", written);
        }
      }

      if (!LoRa->endPacket()) {
        kiss_indicate_error(ERROR_MODEM_TIMEOUT);
        kiss_indicate_error(ERROR_TXFAILED);
        led_indicate_error(5);
        #if MCU_VARIANT == MCU_NATIVE
          if (native_config::g_config.reboot_on_tx_failure) { hard_reset(); }
          else { LoRa->receive(); return; }
        #elif REBOOT_ON_TX_FAILURE
          hard_reset();
        #else
          LoRa->receive(); return;   // drop the packet, keep the node up
        #endif
      }

      add_airtime(written);
      printf("[radio] Sent %d byte packet", written);

    } else {
      led_tx_on(); uint16_t written = 0;
      if (size > SINGLE_MTU) { size = SINGLE_MTU; }
      if (!implicit) { LoRa->beginPacket(); }
      else           { LoRa->beginPacket(size); }
      for (uint16_t i=0; i < size; i++) { LoRa->write(tbuf[i]); written++; }
      LoRa->endPacket(); add_airtime(written);
      printf("[radio] Sent %d byte packet", written);
    }

  } else { kiss_indicate_error(ERROR_TXFAILED); led_indicate_error(5); }
}

void serial_callback(uint8_t sbyte) {
  if (IN_FRAME && sbyte == FEND && command == CMD_DATA) {
    IN_FRAME = false;

    if (!fifo16_isfull(&packet_starts) && queued_bytes < CONFIG_QUEUE_SIZE) {
        uint16_t s = current_packet_start;
        int16_t e = queue_cursor-1; if (e == -1) e = CONFIG_QUEUE_SIZE-1;
        uint16_t l;

        if (s != e) { l = (s < e) ? e - s + 1 : CONFIG_QUEUE_SIZE - s + e + 1; }
        else        { l = 1; }

        if (l >= MIN_L) {
            queue_height++;
            fifo16_push(&packet_starts, s);
            fifo16_push(&packet_lengths, l);
            current_packet_start = queue_cursor;
        }
    }

#ifdef HAS_PROVISIONING
  } else if (IN_FRAME && sbyte == FEND && command == CMD_PROVISION_REQ) {
    IN_FRAME = false;
    on_provision_request(provision_rx_buf);
    provision_rx_buf.clear();
#endif
  } else if (sbyte == FEND) {
    IN_FRAME = true;
    command = CMD_UNKNOWN;
    frame_len = 0;
  } else if (IN_FRAME && frame_len < MTU) {
    // Have a look at the command byte first
    if (frame_len == 0 && command == CMD_UNKNOWN) {
        command = sbyte;
    } else if (command == CMD_DATA) {
        if (bt_state != BT_STATE_CONNECTED) {
          cable_state = CABLE_STATE_CONNECTED;
        }
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (queue_height < CONFIG_QUEUE_MAX_LENGTH && queued_bytes < CONFIG_QUEUE_SIZE) {
              queued_bytes++;
              packet_queue[queue_cursor++] = sbyte;
              if (queue_cursor == CONFIG_QUEUE_SIZE) queue_cursor = 0;
            }
        }
#ifdef HAS_PROVISIONING
    } else if (command == CMD_PROVISION_REQ) {
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (provision_rx_buf.size() < PROVISION_RX_BUF_MAX) {
                provision_rx_buf.append(sbyte);
            }
        }
#endif
    } else if (command == CMD_FREQUENCY) {
      if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 4) {
          uint32_t freq = (uint32_t)cmdbuf[0] << 24 | (uint32_t)cmdbuf[1] << 16 | (uint32_t)cmdbuf[2] << 8 | (uint32_t)cmdbuf[3];

          if (freq == 0) {
            kiss_indicate_frequency();
          } else {
            if (host_may_set_radio()) { lora_freq = freq; setFrequency(); }
            // 1 kHz covers PLL rounding on both SX127x and SX126x; a real
            // channel change is orders of magnitude larger.
            else if (!radio_set_is_noop(freq, lora_freq, 1000)) { radio_config_refused("frequency"); }
            kiss_indicate_frequency();
          }
        }
    } else if (command == CMD_BANDWIDTH) {
      if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 4) {
          uint32_t bw = (uint32_t)cmdbuf[0] << 24 | (uint32_t)cmdbuf[1] << 16 | (uint32_t)cmdbuf[2] << 8 | (uint32_t)cmdbuf[3];

          if (bw == 0) {
            kiss_indicate_bandwidth();
          } else {
            if (host_may_set_radio()) { lora_bw = bw; setBandwidth(); }
            else if (!radio_set_is_noop(bw, lora_bw, 0)) { radio_config_refused("bandwidth"); }
            kiss_indicate_bandwidth();
          }
        }
    } else if (command == CMD_TXPOWER) {
      if (sbyte == 0xFF) {
        kiss_indicate_txpower();
      } else {
        int txp = sbyte;
        #if MODEM == MODEM_RUNTIME
          if (current_modem == SX1262) {
            if (txp > 22) txp = 22;
          } else if (current_modem == SX1280) {
            if (txp > 13) txp = 13;
          } else {
            if (txp > 17) txp = 17;
          }
        #elif MODEM == SX1262
          #if HAS_LORA_PA
            if (txp > PA_MAX_OUTPUT) txp = PA_MAX_OUTPUT;
          #else
            if (txp > 22) txp = 22;
          #endif
        #elif MODEM == SX1280
          #if HAS_PA
            if (txp > 20) txp = 20;
          #else
            if (txp > 13) txp = 13;
          #endif
        #else
          if (txp > 17) txp = 17;
        #endif

        if (host_may_set_radio()) { lora_txp = txp; setTXPower(); }
        else if (!radio_set_is_noop(txp, lora_txp, 0)) { radio_config_refused("tx power"); }
        kiss_indicate_txpower();
      }
    } else if (command == CMD_SF) {
      if (sbyte == 0xFF) {
        kiss_indicate_spreadingfactor();
      } else {
        int sf = sbyte;
        if (sf < 5) sf = 5;
        if (sf > 12) sf = 12;

        if (host_may_set_radio()) { lora_sf = sf; setSpreadingFactor(); }
        else if (!radio_set_is_noop(sf, lora_sf, 0)) { radio_config_refused("spreading factor"); }
        kiss_indicate_spreadingfactor();
      }
    } else if (command == CMD_CR) {
      if (sbyte == 0xFF) {
        kiss_indicate_codingrate();
      } else {
        int cr = sbyte;
        if (cr < 5) cr = 5;
        if (cr > 8) cr = 8;

        if (host_may_set_radio()) { lora_cr = cr; setCodingRate(); }
        else if (!radio_set_is_noop(cr, lora_cr, 0)) { radio_config_refused("coding rate"); }
        kiss_indicate_codingrate();
      }
    } else if (command == CMD_IMPLICIT) {
      set_implicit_length(sbyte);
      kiss_indicate_implicit_length();
    } else if (command == CMD_LEAVE) {
      if (sbyte == 0xFF) {
        display_unblank();
        cable_state   = CABLE_STATE_DISCONNECTED;
        current_rssi  = -292;
        last_rssi     = -292;
        last_rssi_raw = 0x00;
        last_snr_raw  = 0x80;
      }
    } else if (command == CMD_RADIO_STATE) {
      if (bt_state != BT_STATE_CONNECTED) {
        cable_state = CABLE_STATE_CONNECTED;
        display_unblank();
      }
      if (sbyte == 0xFF) {
        kiss_indicate_radiostate();
      } else if (sbyte == 0x00) {
        printf("[radio] CMD_RADIO_STATE(0) from host at %lums\n", (unsigned long)millis());
        stopRadio();
        kiss_indicate_radiostate();
      } else if (sbyte == 0x01) {
        startRadio();
        kiss_indicate_radiostate();
      }
    } else if (command == CMD_ST_ALOCK) {
      if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 2) {
          uint16_t at = (uint16_t)cmdbuf[0] << 8 | (uint16_t)cmdbuf[1];

          if (at == 0) {
            st_airtime_limit = 0.0;
          } else {
            st_airtime_limit = (float)at/(100.0*100.0);
            if (st_airtime_limit >= 1.0) { st_airtime_limit = 0.0; }
          }
          kiss_indicate_st_alock();
        }
    } else if (command == CMD_LT_ALOCK) {
      if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 2) {
          uint16_t at = (uint16_t)cmdbuf[0] << 8 | (uint16_t)cmdbuf[1];

          if (at == 0) {
            lt_airtime_limit = 0.0;
          } else {
            lt_airtime_limit = (float)at/(100.0*100.0);
            if (lt_airtime_limit >= 1.0) { lt_airtime_limit = 0.0; }
          }
          kiss_indicate_lt_alock();
        }
    } else if (command == CMD_STAT_RX) {
      kiss_indicate_stat_rx();
    } else if (command == CMD_STAT_TX) {
      kiss_indicate_stat_tx();
    } else if (command == CMD_STAT_RSSI) {
      kiss_indicate_stat_rssi();
    } else if (command == CMD_RADIO_LOCK) {
      update_radio_lock();
      kiss_indicate_radio_lock();
    } else if (command == CMD_BLINK) {
      led_indicate_info(sbyte);
    } else if (command == CMD_RANDOM) {
      kiss_indicate_random(getRandom());
    } else if (command == CMD_DETECT) {
      if (sbyte == DETECT_REQ) {
        if (bt_state != BT_STATE_CONNECTED) cable_state = CABLE_STATE_CONNECTED;
        kiss_indicate_detect();
      }
    } else if (command == CMD_PROMISC) {
      if (sbyte == 0x01) {
        promisc_enable();
      } else if (sbyte == 0x00) {
        promisc_disable();
      }
      kiss_indicate_promisc();
    } else if (command == CMD_READY) {
      if (!queue_full()) {
        kiss_indicate_ready();
      } else {
        kiss_indicate_not_ready();
      }
    } else if (command == CMD_UNLOCK_ROM) {
      if (sbyte == ROM_UNLOCK_BYTE) {
        unlock_rom();
      }
    } else if (command == CMD_RESET) {
      if (sbyte == CMD_RESET_BYTE) {
        hard_reset();
      }
    } else if (command == CMD_ROM_READ) {
      kiss_dump_eeprom();
    } else if (command == CMD_CFG_READ) {
      kiss_dump_config();
    } else if (command == CMD_ROM_WRITE) {
      if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 2) {
          eeprom_write(cmdbuf[0], cmdbuf[1]);
        }
    } else if (command == CMD_FW_VERSION) {
      kiss_indicate_version();
    } else if (command == CMD_PLATFORM) {
      kiss_indicate_platform();
    } else if (command == CMD_MCU) {
      kiss_indicate_mcu();
    } else if (command == CMD_BOARD) {
      kiss_indicate_board();
    } else if (command == CMD_CONF_SAVE) {
      eeprom_conf_save();
    } else if (command == CMD_CONF_DELETE) {
      eeprom_conf_delete();
    } else if (command == CMD_FB_EXT) {
      #if HAS_DISPLAY == true
        if (sbyte == 0xFF) {
          kiss_indicate_fbstate();
        } else if (sbyte == 0x00) {
          ext_fb_disable();
          kiss_indicate_fbstate();
        } else if (sbyte == 0x01) {
          ext_fb_enable();
          kiss_indicate_fbstate();
        }
      #endif
    } else if (command == CMD_FB_WRITE) {
      if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }
        #if HAS_DISPLAY
          if (frame_len == 9) {
            uint8_t line = cmdbuf[0];
            if (line > 63) line = 63;
            int fb_o = line*8; 
            memcpy(fb+fb_o, cmdbuf+1, 8);
          }
        #endif
    } else if (command == CMD_FB_READ) {
      if (sbyte != 0x00) { kiss_indicate_fb(); }
    } else if (command == CMD_DISP_READ) {
      if (sbyte != 0x00) { kiss_indicate_disp(); }
    } else if (command == CMD_DEV_HASH) {
      #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
        if (sbyte != 0x00) {
          kiss_indicate_device_hash();
        }
      #endif
    } else if (command == CMD_DEV_SIG) {
      #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
        if (sbyte == FESC) {
              ESCAPE = true;
          } else {
              if (ESCAPE) {
                  if (sbyte == TFEND) sbyte = FEND;
                  if (sbyte == TFESC) sbyte = FESC;
                  ESCAPE = false;
              }
              if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
          }

          if (frame_len == DEV_SIG_LEN) {
            memcpy(dev_sig, cmdbuf, DEV_SIG_LEN);
            device_save_signature();
          }
      #endif
    } else if (command == CMD_FW_UPD) {
      if (sbyte == 0x01) {
        firmware_update_mode = true;
      } else {
        firmware_update_mode = false;
      }
    } else if (command == CMD_HASHES) {
      #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
        if (sbyte == 0x01) {
          kiss_indicate_target_fw_hash();
        } else if (sbyte == 0x02) {
          kiss_indicate_fw_hash();
        } else if (sbyte == 0x03) {
          kiss_indicate_bootloader_hash();
        } else if (sbyte == 0x04) {
          kiss_indicate_partition_table_hash();
        }
      #endif
    } else if (command == CMD_FW_HASH) {
      #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
        if (sbyte == FESC) {
              ESCAPE = true;
          } else {
              if (ESCAPE) {
                  if (sbyte == TFEND) sbyte = FEND;
                  if (sbyte == TFESC) sbyte = FESC;
                  ESCAPE = false;
              }
              if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
          }

          if (frame_len == DEV_HASH_LEN) {
            memcpy(dev_firmware_hash_target, cmdbuf, DEV_HASH_LEN);
            device_save_firmware_hash();
          }
      #endif
    } else if (command == CMD_WIFI_CHN) {
      #if HAS_WIFI
        if (sbyte > 0 && sbyte < 14) { eeprom_update(eeprom_addr(ADDR_CONF_WCHN), sbyte); }
      #endif
    } else if (command == CMD_WIFI_MODE) {
      #if HAS_WIFI
        if (sbyte == WR_WIFI_OFF || sbyte == WR_WIFI_STA || sbyte == WR_WIFI_AP) {
          wr_conf_save(sbyte);
          wifi_mode = sbyte;
          wifi_remote_init();
        }
      #endif
    } else if (command == CMD_WIFI_SSID) {
      #if HAS_WIFI
        if (sbyte == FESC) { ESCAPE = true; }
        else {
          if (ESCAPE) {
            if (sbyte == TFEND) sbyte = FEND;
            if (sbyte == TFESC) sbyte = FESC;
            ESCAPE = false;
          }
          if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (sbyte == 0x00) {
          for (uint8_t i = 0; i<33; i++) {
            if (i<frame_len && i<32) { eeprom_update(config_addr(ADDR_CONF_SSID+i), cmdbuf[i]); }
            else                     { eeprom_update(config_addr(ADDR_CONF_SSID+i), 0x00); }
          }
        }
      #endif
    } else if (command == CMD_WIFI_PSK) {
      #if HAS_WIFI
        if (sbyte == FESC) { ESCAPE = true; }
        else {
          if (ESCAPE) {
            if (sbyte == TFEND) sbyte = FEND;
            if (sbyte == TFESC) sbyte = FESC;
            ESCAPE = false;
          }
          if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (sbyte == 0x00) {
          for (uint8_t i = 0; i<33; i++) {
            if (i<frame_len && i<32) { eeprom_update(config_addr(ADDR_CONF_PSK+i), cmdbuf[i]); }
            else                     { eeprom_update(config_addr(ADDR_CONF_PSK+i), 0x00); }
          }
        }
      #endif
    } else if (command == CMD_WIFI_IP) {
      #if HAS_WIFI
        if (sbyte == FESC) { ESCAPE = true; }
        else {
          if (ESCAPE) {
            if (sbyte == TFEND) sbyte = FEND;
            if (sbyte == TFESC) sbyte = FESC;
            ESCAPE = false;
          }
          if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 4) { for (uint8_t i = 0; i<4; i++) { eeprom_update(config_addr(ADDR_CONF_IP+i), cmdbuf[i]); } }
      #endif
    } else if (command == CMD_WIFI_NM) {
      #if HAS_WIFI
        if (sbyte == FESC) { ESCAPE = true; }
        else {
          if (ESCAPE) {
            if (sbyte == TFEND) sbyte = FEND;
            if (sbyte == TFESC) sbyte = FESC;
            ESCAPE = false;
          }
          if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 4) { for (uint8_t i = 0; i<4; i++) { eeprom_update(config_addr(ADDR_CONF_NM+i), cmdbuf[i]); } }
      #endif
    } else if (command == CMD_BT_CTRL) {
      #if HAS_BLUETOOTH || HAS_BLE
        if (sbyte == 0x00) {
          bt_stop();
          bt_conf_save(false);
        } else if (sbyte == 0x01) {
          bt_start();
          bt_conf_save(true);
        } else if (sbyte == 0x02) {
          if (bt_state == BT_STATE_OFF) {
            bt_start();
            bt_conf_save(true);
          }
          if (bt_state != BT_STATE_CONNECTED) {
            bt_enable_pairing();
          }
        }
      #endif
    } else if (command == CMD_BT_UNPAIR) {
      #if HAS_BLE
        if (sbyte == 0x01) { bt_debond_all(); }
      #endif
    } else if (command == CMD_DISP_INT) {
      #if HAS_DISPLAY
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            display_intensity = sbyte;
            di_conf_save(display_intensity);
            display_unblank();
        }
      #endif
    } else if (command == CMD_DISP_ADDR) {
      #if HAS_DISPLAY
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            display_addr = sbyte;
            da_conf_save(display_addr);
        }

      #endif
    } else if (command == CMD_DISP_BLNK) {
      #if HAS_DISPLAY
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            db_conf_save(sbyte);
            display_unblank();
        }
      #endif
    } else if (command == CMD_DISP_ROT) {
      #if HAS_DISPLAY
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            drot_conf_save(sbyte);
            display_unblank();
        }
      #endif
    } else if (command == CMD_DIS_IA) {
      if (sbyte == FESC) {
          ESCAPE = true;
      } else {
          if (ESCAPE) {
              if (sbyte == TFEND) sbyte = FEND;
              if (sbyte == TFESC) sbyte = FESC;
              ESCAPE = false;
          }
          dia_conf_save(sbyte);
      }
    } else if (command == CMD_DISP_RCND) {
      #if HAS_DISPLAY
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (sbyte > 0x00) recondition_display = true;
        }
      #endif
    } else if (command == CMD_NP_INT) {
      #if HAS_NP
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            sbyte;
            led_set_intensity(sbyte);
            np_int_conf_save(sbyte);
        }

      #endif
    }
  }
}

#if MCU_VARIANT == MCU_ESP32
  portMUX_TYPE update_lock = portMUX_INITIALIZER_UNLOCKED;
#endif

bool medium_free() {
  update_modem_status();
  if (avoid_interference && interference_detected) { return false; }
  return !dcd;
}

bool noise_floor_sampled = false;
int  noise_floor_sample  = 0;
int  noise_floor_buffer[NOISE_FLOOR_SAMPLES] = {0};
void update_noise_floor() {
  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    if (!dcd) {
      #if HAS_LORA_LNA == false
      if (!noise_floor_sampled || current_rssi < noise_floor + CSMA_INFR_THRESHOLD_DB) {
      #else
      if ((!noise_floor_sampled || current_rssi < noise_floor + CSMA_INFR_THRESHOLD_DB) || (noise_floor_sampled && (noise_floor < LNA_GD_THRSHLD && current_rssi <= LNA_GD_LIMIT))) {
      #endif
        #if HAS_LORA_LNA
          // Discard invalid samples due to gain variance
          // during LoRa LNA re-calibration
          if (current_rssi < noise_floor-LORA_LNA_GVT) { return; }
        #endif
        bool sum_noise_floor = false;
        noise_floor_buffer[noise_floor_sample] = current_rssi;
        noise_floor_sample = noise_floor_sample+1;
        if (noise_floor_sample >= NOISE_FLOOR_SAMPLES) {
          noise_floor_sample %= NOISE_FLOOR_SAMPLES;
          noise_floor_sampled = true;
          sum_noise_floor = true;
        }

        if (noise_floor_sampled && sum_noise_floor) {
          noise_floor = 0;
          for (int ni = 0; ni < NOISE_FLOOR_SAMPLES; ni++) { noise_floor += noise_floor_buffer[ni]; }
          noise_floor /= NOISE_FLOOR_SAMPLES;
        }
      }
    }
  #endif
}

#define LED_ID_TRIG 16
uint8_t led_id_filter = 0;
uint32_t interference_start = 0;
bool interference_persists = false;
void update_modem_status() {
  #if MCU_VARIANT == MCU_ESP32
    portENTER_CRITICAL(&update_lock);
  #elif MCU_VARIANT == MCU_NRF52
    portENTER_CRITICAL();
  #endif

  bool carrier_detected = LoRa->dcd();
  current_rssi = LoRa->currentRssi();
  last_status_update = millis();

  #if MCU_VARIANT == MCU_ESP32
    portEXIT_CRITICAL(&update_lock);
  #elif MCU_VARIANT == MCU_NRF52
    portEXIT_CRITICAL();
  #endif

  #if HAS_LORA_LNA
    if (noise_floor > LNA_GD_THRSHLD)  { interference_detected = !carrier_detected && (current_rssi > (noise_floor+CSMA_INFR_THRESHOLD_DB)); }
    else                               { interference_detected = !carrier_detected && (current_rssi > LNA_GD_LIMIT); }
  #else
    interference_detected = !carrier_detected && (current_rssi > (noise_floor+CSMA_INFR_THRESHOLD_DB));
  #endif

  if (interference_detected) { if (led_id_filter < LED_ID_TRIG) { led_id_filter += 1; } }
  else                       { if (led_id_filter > 0) {led_id_filter -= 1; } }

  // Handle potential false interference detection due to
  // LNA recalibration, antenna swap, moving into new RF
  // environment or similar.
  if (interference_detected && current_rssi < CSMA_RFENV_RECAL_LIMIT_DB) {
    if (!interference_persists) { interference_persists = true; interference_start = millis(); }
    else {
      if (millis()-interference_start >= CSMA_RFENV_RECAL_MS) { noise_floor_sampled = false; interference_persists = false; }
    }
  } else { interference_persists = false; }

  if (carrier_detected) { dcd = true; } else { dcd = false; }

  dcd_led = dcd;
  if (dcd_led) { led_rx_on(); }
  else {
    if (interference_detected && noise_floor_sampled) {
      if (led_id_filter >= LED_ID_TRIG) { led_id_on(); }
    } else {
      if (airtime_lock) { led_indicate_airtime_lock(); }
      else              { led_rx_off(); led_id_off(); }
    }
  }

  update_noise_floor();
}

void check_modem_status() {
  if (millis()-last_status_update >= status_interval_ms) {
    update_modem_status();

    #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
      util_samples[dcd_sample] = dcd;
      dcd_sample = (dcd_sample+1)%DCD_SAMPLES;
      if (dcd_sample % UTIL_UPDATE_INTERVAL == 0) {
        int util_count = 0;
        for (int ui = 0; ui < DCD_SAMPLES; ui++) {
          if (util_samples[ui]) util_count++;
        }
        local_channel_util = (float)util_count / (float)DCD_SAMPLES;
        total_channel_util = local_channel_util + airtime;
        if (total_channel_util > 1.0) total_channel_util = 1.0;

        int16_t cb = current_airtime_bin();
        uint16_t nb = cb+1; if (nb == AIRTIME_BINS) { nb = 0; }
        if (total_channel_util > longterm_bins[cb]) longterm_bins[cb] = total_channel_util;
        longterm_bins[nb] = 0.0;

        update_airtime();
      }
    #endif
  }
}

void validate_status() {
  #if MCU_VARIANT == MCU_1284P
      uint8_t boot_flags = OPTIBOOT_MCUSR;
      uint8_t F_POR = PORF;
      uint8_t F_BOR = BORF;
      uint8_t F_WDR = WDRF;
  #elif MCU_VARIANT == MCU_2560
      uint8_t boot_flags = OPTIBOOT_MCUSR;
      if (boot_flags == 0x00) boot_flags = 0x03;
      uint8_t F_POR = PORF;
      uint8_t F_BOR = BORF;
      uint8_t F_WDR = WDRF;
  #elif MCU_VARIANT == MCU_ESP32
      // TODO: Get ESP32 boot flags
      uint8_t boot_flags = 0x02;
      uint8_t F_POR = 0x00;
      uint8_t F_BOR = 0x00;
      uint8_t F_WDR = 0x01;
  #elif MCU_VARIANT == MCU_NRF52
      // TODO: Get NRF52 boot flags
      uint8_t boot_flags = 0x02;
      uint8_t F_POR = 0x00;
      uint8_t F_BOR = 0x00;
      uint8_t F_WDR = 0x01;
  #elif MCU_VARIANT == MCU_NATIVE
      // Native userspace daemon — no MCU reset cause registers. Report
      // a synthetic "power-on, bootloader path" status.
      uint8_t boot_flags = 0x02;
      uint8_t F_POR = 0x00;
      uint8_t F_BOR = 0x00;
      uint8_t F_WDR = 0x01;
  #endif

  if (hw_ready || device_init_done) {
    hw_ready = false;
    printf("[init] Error, invalid hardware check state\r\n");
    #if HAS_DISPLAY
      if (disp_ready) {
        device_init_done = true;
        update_display();
      }
    #endif
    led_indicate_boot_error();
  }

  if (boot_flags & (1<<F_POR)) {
    boot_vector = START_FROM_POWERON;
  } else if (boot_flags & (1<<F_BOR)) {
    boot_vector = START_FROM_BROWNOUT;
  } else if (boot_flags & (1<<F_WDR)) {
    boot_vector = START_FROM_BOOTLOADER;
  } else {
      Serial.write("Error, indeterminate boot vector\r\n");
      #if HAS_DISPLAY
        if (disp_ready) {
          device_init_done = true;
          update_display();
        }
      #endif
      led_indicate_boot_error();
  }

  if (boot_vector == START_FROM_BOOTLOADER || boot_vector == START_FROM_POWERON) {
    if (eeprom_lock_set()) {
      if (eeprom_product_valid() && eeprom_model_valid() && eeprom_hwrev_valid()) {
#ifdef DISABLE_FIRMWARE_CHECKSUM
        // Native builds self-provision the EEPROM in PinMap.cpp's
        // seed_eeprom_if_unprovisioned() but skip MD5 computation —
        // they short-circuit the checksum check here.
        if (true) {
#else
        if (eeprom_checksum_valid()) {
#endif
          eeprom_ok = true;
          if (modem_installed) {
            #if PLATFORM == PLATFORM_ESP32 || PLATFORM == PLATFORM_NRF52 || PLATFORM == PLATFORM_NATIVE
              if (device_init()) {
                hw_ready = true;
              } else {
                hw_ready = false;
                printf("[init] Error, device init failed\r\n");
              }
            #else
              hw_ready = true;
            #endif
          } else {
            hw_ready = false;
            printf("[init] No radio module found\r\n");
            #if HAS_DISPLAY
              if (disp_ready) {
                device_init_done = true;
                update_display();
              }
            #endif
          }
          
          if (hw_ready && eeprom_have_conf()) {
            eeprom_conf_load();
            op_mode = prov_op_mode;
            // A TNC-mode board is autonomous and brings its own radio up. In
            // host mode the attached host owns the radio and starts it with
            // CMD_RADIO_STATE, as upstream RNode expects.
            if (op_mode == MODE_TNC) { startRadio(); }
          }
        } else {
          hw_ready = false;
          printf("[init] Invalid EEPROM checksum\r\n");
          #if HAS_DISPLAY
            if (disp_ready) {
              device_init_done = true;
              update_display();
            }
          #endif
        }
      } else {
        hw_ready = false;
        printf("[init] Invalid EEPROM configuration\r\n");
        #if HAS_DISPLAY
          if (disp_ready) {
            device_init_done = true;
            update_display();
          }
        #endif
      }
    } else {
      hw_ready = false;
      // Spell out the consequence and the fix. A blank EEPROM is the normal
      // state of NEW hardware, and the upload that produced it reports success
      // -- so without this the symptom reads as a failed flash or a dead radio
      // rather than a missing one-time provisioning step.
      printf("[init] Device unprovisioned, no device configuration found in EEPROM\r\n");
      printf("[init] hw_ready=0: the radio will NOT start until this board is\r\n");
      printf("[init] provisioned once with:  pio run -e <env> -t provision\r\n");
      #if HAS_DISPLAY
        if (disp_ready) {
          device_init_done = true;
          update_display();
        }
      #endif
    }
  } else {
    hw_ready = false;
    printf("[init] Error, incorrect boot vector\r\n");
    #if HAS_DISPLAY
      if (disp_ready) {
        device_init_done = true;
        update_display();
      }
    #endif
    led_indicate_boot_error();
  }
}

#if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
  void update_csma_parameters() {
    int airtime_pct = (int)(airtime*100);
    int new_cw_band = cw_band;

    if (airtime_pct <= CSMA_BAND_1_MAX_AIRTIME) { new_cw_band = 1; }
    else {
      int at = airtime_pct + CSMA_BAND_1_MAX_AIRTIME;
      new_cw_band = map(at, CSMA_BAND_1_MAX_AIRTIME, CSMA_BAND_N_MIN_AIRTIME, 2, CSMA_CW_BANDS);
    }

    if (new_cw_band > CSMA_CW_BANDS) { new_cw_band = CSMA_CW_BANDS; }
    if (new_cw_band != cw_band) { 
      cw_band = (uint8_t)(new_cw_band);
      cw_min  = (cw_band-1) * CSMA_CW_PER_BAND_WINDOWS;
      cw_max  = (cw_band) * CSMA_CW_PER_BAND_WINDOWS - 1;
      kiss_indicate_csma_stats();
    }
  }
#endif

void tx_queue_handler() {
  if (!airtime_lock && queue_height > 0) {
    if (csma_cw == -1) {
      csma_cw = random(cw_min, cw_max);
      cw_wait_target = csma_cw * csma_slot_ms;
    }

    if (difs_wait_start == -1) {                                                  // DIFS wait not yet started
      if (medium_free()) { difs_wait_start = millis(); return; }                  // Set DIFS wait start time
      else               { return; } }                                            // Medium not yet free, continue waiting
    
    else {                                                                        // We are waiting for DIFS or CW to pass
      if (!medium_free()) { difs_wait_start = -1; cw_wait_start = -1; return; }   // Medium became occupied while in DIFS wait, restart waiting when free again
      else {                                                                      // Medium is free, so continue waiting
        if (millis() < difs_wait_start+difs_ms) { return; }                       // DIFS has not yet passed, continue waiting
        else {                                                                    // DIFS has passed, and we are now in CW wait
          if (cw_wait_start == -1) { cw_wait_start = millis(); return; }          // If we haven't started counting CW wait time, do it from now
          else {                                                                  // If we are already counting CW wait time, add it to the counter
            cw_wait_passed += millis()-cw_wait_start; cw_wait_start   = millis();
            if (cw_wait_passed < cw_wait_target) { return; }                      // Contention window wait time has not yet passed, continue waiting
            else {                                                                // Wait time has passed, flush the queue
              bool should_flush = !lora_limit_rate && !lora_guard_rate;
              if (should_flush) { flush_queue(); } else { pop_queue(); }
              cw_wait_passed = 0; csma_cw = -1; difs_wait_start = -1; }
          }
        }
      }
    }
  }
}

void work_while_waiting() { loop(); }

// Re-announce the NomadNet site periodically.
//
// The startup announce below is sent exactly once, which is fine for a client
// that happens to be listening at that moment. It is not enough for a headless
// node: RNS path entries can go stale or get replaced by a worse route, and
// because this device runs with transport disabled it does not answer path
// requests either. Once a peer's path to us degrades there is nothing that ever
// repairs it, and the node looks dead until it is power-cycled -- even though it
// is up and on WiFi. A periodic announce is what heals that.
//
// Announces cost airtime on LoRa, so the interval is deliberately generous;
// tune NOMADNET_ANNOUNCE_INTERVAL_MS in Config.h.
#if defined(HAS_RNS) && defined(URTN_STATS_PAGES)
static void nomadnet_announce_watch() {
  static uint32_t last_announce = 0;
  static uint32_t announce_jitter = 0;
  static bool armed = false;
  static bool first_done = false;
  if (!nomadnet_enabled || !nomadnet_destination) return;
  // Jitter is rolled when arming as well as after each announce: the very
  // first announce is the one most likely to collide fleet-wide, because a
  // block of nodes restored to mains power all reach this point together.
  if (!armed) {
    armed = true; last_announce = millis();
    announce_jitter = (uint32_t)random(NOMADNET_ANNOUNCE_JITTER_MS);
    return;
  }
  // The startup announce in setup() is NOT sufficient. It fires at ~t+4s, about
  // a second before DHCP completes and the UDP socket is rebound to a real
  // address (see the rebind in Remote.h), so on a fast boot it is emitted into a
  // socket bound to 0.0.0.0 and is simply lost. That race was previously hidden
  // by a slow startup. Send the first real announce shortly after boot, once the
  // network is definitely up, then settle into the normal interval.
  uint32_t due = first_done ? NOMADNET_ANNOUNCE_INTERVAL_MS : NOMADNET_FIRST_ANNOUNCE_MS;
  due += announce_jitter;
  if (millis() - last_announce < due) return;
  last_announce = millis();
  announce_jitter = (uint32_t)random(NOMADNET_ANNOUNCE_JITTER_MS);
  first_done = true;
  printf("[announce] re-announcing NomadNet site \"%s\"\n", nomadnet_name);
  nomadnet_destination.announce(nomadnet_name);
}
#endif

// Recover a wedged modem.
//
// See RADIO_RX_WATCHDOG_MS in Config.h for the failure this exists for: the
// receive path can die while every indicator this firmware exposes still says
// the radio is healthy. The only observable that actually distinguishes a
// working modem from a wedged one is whether packets are still arriving, so
// that is what we watch.
//
// Deliberately NOT conditioned on having transmitted or on having a known
// peer. A wedged receiver and an empty channel are indistinguishable from
// here, and re-initialising costs milliseconds, so we treat prolonged silence
// as suspect either way rather than trying to prove which it is.
#if defined(HAS_RNS) && defined(LORA_TRANSPORT)
// Shared receive-activity tracking. radio_config_apply_live() consults it to
// decide whether the link it is about to reconfigure was actually working --
// only then is a rollback worth arming.
static uint32_t rx_last_change_ms = 0;
static size_t   rx_last_count     = 0;
static bool     rx_tracking       = false;

static void radio_rx_watchdog() {
  #if RADIO_RX_WATCHDOG_MS > 0
    static uint32_t reinit_count = 0;

    // Disarm while the radio is down so a deliberate stop (console mode, a
    // host CMD_RADIO_STATE(0)) is not counted as silence and does not trigger
    // a re-init the moment the radio comes back.
    if (!radio_online || !lora_interface) { rx_tracking = false; return; }

    size_t rx_now = lora_interface.rx();
    if (!rx_tracking) {
      rx_tracking = true; rx_last_count = rx_now; rx_last_change_ms = millis();
      return;
    }
    if (rx_now != rx_last_count) {
      rx_last_count = rx_now; rx_last_change_ms = millis();
      return;
    }
    if (millis() - rx_last_change_ms < RADIO_RX_WATCHDOG_MS) return;

    ++reinit_count;
    printf("[radio] rx watchdog: nothing demodulated for %lums, reinitialising "
           "modem (reinit #%lu, rx=%lu tx_calls=%lu nf=%d)\n",
           (unsigned long)(millis() - rx_last_change_ms), (unsigned long)reinit_count,
           (unsigned long)rx_now, (unsigned long)tx_calls, (int)noise_floor);

    stopRadio();
    if (startRadio()) {
      printf("[radio] rx watchdog: modem reinitialised OK\n");
    } else {
      // startRadio() has already reported the specific reason. Leave the timer
      // reset regardless so a permanently unstartable radio retries on the
      // normal interval instead of spinning every loop iteration.
      printf("[radio] rx watchdog: modem reinit FAILED\n");
    }
    rx_last_change_ms = millis();
    rx_last_count     = lora_interface ? lora_interface.rx() : 0;
  #endif
}

// Apply the live radio shadow config to the modem, and keep the two in step.
//
// The provisioning commit path writes lora_freq/bw/sf/cr/txp straight into the
// shadow variables and saves EEPROM, but does not touch the modem -- those
// fields are declared FF_REBOOT_REQUIRED. Nothing anywhere reported that the
// running radio and the reported config had diverged, and the divergence
// persisted for as long as the board stayed up.
//
// That is not cosmetic. On 2026-08-22 it took the mesh down. An SF8 -> SF7
// change was accepted and reported by both boards and applied by neither, so
// both kept running SF8. The first board to be power-cycled came back on SF7
// (read from EEPROM by startRadio) while its peer stayed on SF8, and the link
// died: RF arrived at -44 dBm and nothing demodulated in either direction,
// while every health indicator -- radio_online, hw_ready, noise floor, the
// reported SF itself -- looked perfect. It also explains why that SF7 change
// produced no measurable speed-up at the time: it never reached the radio.
//
// The snapshot below is the invariant: if the shadow config differs from what
// was last programmed, the modem is not running the configuration this node is
// telling everyone it runs. Reprogram it and say so.
static uint32_t rc_seen_freq = 0, rc_seen_bw = 0;
static int      rc_seen_sf   = 0, rc_seen_cr = 0, rc_seen_txp = 0;
static bool     rc_armed     = false;

static void radio_config_snapshot() {
  rc_seen_freq = lora_freq; rc_seen_bw  = lora_bw;  rc_seen_sf = lora_sf;
  rc_seen_cr   = lora_cr;   rc_seen_txp = lora_txp; rc_armed   = true;
}

// Reprogram every modem register from the shadow config. Each setter is a
// no-op while the radio is down, and startRadio() programs all of them from
// these same variables, so a stopped radio needs nothing here.
// Commit-confirm ("commit confirmed") state for PHY changes. See
// RADIO_CONFIG_CONFIRM_MS in Config.h.
static bool     rc_preset_sync_pending = false;
static bool     rb_armed     = false;
static bool     rb_reverting = false;
static uint32_t rb_deadline  = 0;
static size_t   rb_rx_at_apply = 0;
static uint32_t rb_freq = 0, rb_bw = 0;
static int      rb_sf = 0, rb_cr = 0, rb_txp = 0;

// A stable fingerprint of the parameters that must match for two nodes to hear
// each other at all. Two nodes with the same value can talk; two with different
// values cannot, no matter how strong the signal. Exposed in the [lora] log line
// and on /page/device.mu so a mismatch can be spotted from any node that is
// reachable by some other route -- which is exactly the check that would have
// found the 2026-08-22 outage in seconds instead of a day.
uint32_t lora_phy_hash() {
  // Frequency, bandwidth and spreading factor only.
  //
  // Coding rate is deliberately EXCLUDED. In explicit-header mode the CR is
  // carried in the header and the receiver adapts to it per packet, so two
  // nodes on different coding rates interoperate perfectly well. Including it
  // (as this did until 2026-08-23) raises a false "these nodes cannot hear each
  // other" alarm for a difference that costs nothing -- the opposite of what
  // this fingerprint is for.
  const uint32_t vals[3] = { lora_freq, lora_bw, (uint32_t)lora_sf };
  uint32_t h = 2166136261u;                 // FNV-1a
  for (size_t i = 0; i < 3; i++) {
    for (int b = 0; b < 4; b++) {
      h ^= (uint8_t)((vals[i] >> (8*b)) & 0xFF);
      h *= 16777619u;
    }
  }
  return h;
}

// Which named preset the live configuration corresponds to, or
// RADIO_PRESET_CUSTOM if bandwidth/SF/CR were set individually to something off
// the ladder. Computed from the live values rather than stored, so the reported
// preset can never drift out of step with the radio the way a cached copy would.
uint8_t radio_preset_current() {
  for (uint8_t i = 0; i < RADIO_PRESET_COUNT; i++) {
    if (RADIO_PRESETS[i].bw == lora_bw &&
        RADIO_PRESETS[i].sf == (uint8_t)lora_sf &&
        RADIO_PRESETS[i].cr == (uint8_t)lora_cr) return i;
  }
  return RADIO_PRESET_CUSTOM;
}

const char* radio_preset_name() {
  uint8_t i = radio_preset_current();
  return (i == RADIO_PRESET_CUSTOM) ? "Custom" : RADIO_PRESETS[i].name;
}

// Adopt a preset. Rejects RADIO_PRESET_CUSTOM and any out-of-range index:
// "custom" describes a configuration, it does not select one.
//
// Only the shadow values are written here. The consistency watch in loop()
// notices they no longer match what the modem was programmed with, applies them
// and arms the commit-confirm rollback -- so a preset that strands the link
// unwinds itself exactly like any other PHY change.
bool radio_preset_apply(uint8_t idx) {
  if (idx >= RADIO_PRESET_COUNT) return false;
  const RadioPreset& p = RADIO_PRESETS[idx];
  lora_bw = p.bw; lora_sf = (int)p.sf; lora_cr = (int)p.cr;
  printf("[radio] preset selected: %s (bw=%lu sf=%d cr=%d)\n",
         p.name, (unsigned long)lora_bw, (int)lora_sf, (int)lora_cr);
  // The individual Bandwidth/SF/CR fields persist independently of the preset,
  // and are re-applied from storage at boot. Without pushing the preset's
  // values back into them, a stale individual field silently overrides the
  // preset on the next restart -- observed 2026-08-23, where a board booted
  // with cr=6 from an earlier test while its preset field still read
  // "ShortFast". Deferred to the loop rather than done here: this runs inside
  // a provisioning commit, and re-entering commit from a field setter is not
  // safe.
  rc_preset_sync_pending = true;
  // The namespace's on_commit hook persists EEPROM before field setters run, so
  // a preset chosen in that same commit would otherwise not be saved.
  if (hw_ready && radio_online) { eeprom_conf_save(); }
  return true;
}

void radio_config_apply_live() {
  if (!radio_online) { rc_armed = false; return; }
  #if RADIO_CONFIG_CONFIRM_MS > 0
    // Arm a rollback only when we are changing away from a configuration that
    // was demonstrably carrying traffic. Applying config on a link that was
    // already silent tells us nothing, and reverting to an equally silent
    // previous setting would just add churn.
    if (rc_armed && !rb_armed && !rb_reverting && lora_interface && rx_tracking &&
        (millis() - rx_last_change_ms) < RADIO_CONFIG_CONFIRM_MS) {
      rb_freq = rc_seen_freq; rb_bw  = rc_seen_bw;  rb_sf = rc_seen_sf;
      rb_cr   = rc_seen_cr;   rb_txp = rc_seen_txp;
      rb_rx_at_apply = lora_interface.rx();
      rb_deadline    = millis() + RADIO_CONFIG_CONFIRM_MS;
      rb_armed       = true;
      printf("[radio] commit-confirm armed: revert to f=%lu bw=%lu sf=%d cr=%d txp=%d "
             "in %lums unless traffic resumes\n",
             (unsigned long)rb_freq, (unsigned long)rb_bw, rb_sf, rb_cr, rb_txp,
             (unsigned long)RADIO_CONFIG_CONFIRM_MS);
    }
  #endif
  // setFrequency() ends in getFrequency(), which writes the modem's quantised
  // readback back into lora_freq. Re-applying an already-quantised value
  // re-quantises it, so without this the configured frequency walks downward a
  // couple of hundred Hz on every apply -- unbounded, and it would eventually
  // matter. The configured value stays authoritative; the modem is within one
  // frequency step of it either way.
  const uint32_t requested_freq = lora_freq;
  setFrequency();
  lora_freq = requested_freq;
  setBandwidth();
  setSpreadingFactor();
  setCodingRate();
  setTXPower();
  updateBitrate();      // setBandwidth() does not do this itself
  lora_receive();       // back to RX after reprogramming
  radio_config_snapshot();
  printf("[radio] config applied live: f=%lu bw=%lu sf=%d cr=%d txp=%d bitrate=%lu\n",
         (unsigned long)lora_freq, (unsigned long)lora_bw, (int)lora_sf,
         (int)lora_cr, (int)lora_txp, (unsigned long)lora_bitrate);
}

static void lora_config_consistency_watch() {
  if (!radio_online) { rc_armed = false; return; }

  #ifdef HAS_PROVISIONING
    if (rc_preset_sync_pending) {
      rc_preset_sync_pending = false;
      extern void provisioning_sync_radio_from_runtime();
      provisioning_sync_radio_from_runtime();
      printf("[radio] preset written through to stored bw/sf/cr\n");
    }
  #endif
  if (!rc_armed)     { radio_config_snapshot(); return; }

  if (lora_freq != rc_seen_freq || lora_bw  != rc_seen_bw ||
      lora_sf   != rc_seen_sf   || lora_cr  != rc_seen_cr ||
      lora_txp  != rc_seen_txp) {
    printf("[radio] shadow config changed without reaching the modem: "
           "f %lu->%lu bw %lu->%lu sf %d->%d cr %d->%d txp %d->%d -- applying\n",
           (unsigned long)rc_seen_freq, (unsigned long)lora_freq,
           (unsigned long)rc_seen_bw,   (unsigned long)lora_bw,
           rc_seen_sf, lora_sf, rc_seen_cr, lora_cr, rc_seen_txp, lora_txp);
    radio_config_apply_live();
  }

  // LoRaInterface snapshots lora_bitrate in its constructor and never revisits
  // it, so RNS quotes the boot-time rate forever. It is used for RNS timing
  // estimates and is what /page/device.mu reports -- an interface claiming
  // "spreading_factor: 7" beside "bitrate: 3125" (the SF8 rate) is what first
  // exposed the bug above.
  if (lora_interface && lora_bitrate != 0 &&
      lora_interface.bitrate() != lora_bitrate) {
    printf("[lora] interface bitrate stale: %lu -> %lu bps (sf=%d bw=%lu cr=%d)\n",
           (unsigned long)lora_interface.bitrate(), (unsigned long)lora_bitrate,
           (int)lora_sf, (unsigned long)lora_bw, (int)lora_cr);
    lora_interface.bitrate(lora_bitrate);
  }
}

// Confirm or roll back a PHY change. Any decoded packet is proof the new
// settings work; silence for the whole window means they do not, and the node
// puts itself back on the last configuration known to carry traffic.
static void radio_commit_confirm_watch() {
#if RADIO_CONFIG_CONFIRM_MS > 0
  if (!rb_armed || !radio_online) return;

  if (lora_interface && lora_interface.rx() != rb_rx_at_apply) {
    rb_armed = false;
    printf("[radio] config confirmed: traffic resumed on f=%lu bw=%lu sf=%d (phy=%08lx)\n",
           (unsigned long)lora_freq, (unsigned long)lora_bw, (int)lora_sf,
           (unsigned long)lora_phy_hash());
    return;
  }

  if ((int32_t)(millis() - rb_deadline) < 0) return;

  printf("[radio] config NOT confirmed after %lums of silence -- reverting to "
         "f=%lu bw=%lu sf=%d cr=%d txp=%d\n",
         (unsigned long)RADIO_CONFIG_CONFIRM_MS, (unsigned long)rb_freq,
         (unsigned long)rb_bw, rb_sf, rb_cr, rb_txp);

  rb_armed     = false;
  rb_reverting = true;                      // keep apply_live from re-arming
  lora_freq = rb_freq; lora_bw = rb_bw; lora_sf = rb_sf;
  lora_cr   = rb_cr;   lora_txp = rb_txp;
  radio_config_apply_live();
  // Persist, so a reboot does not resurrect the configuration that stranded us.
  // Both stores must be corrected: EEPROM for the classic config path, and the
  // provisioning store because its values are re-applied at boot and would
  // otherwise strand the node again on the next restart.
  if (hw_ready && radio_online) { eeprom_conf_save(); }
  #ifdef HAS_PROVISIONING
    extern void provisioning_sync_radio_from_runtime();
    provisioning_sync_radio_from_runtime();
  #endif
  rb_reverting = false;
#endif
}
#endif

// Periodic heap report for headless operation. RNS_LOW_MEMORY_REBOOT resets the
// board when free heap reaches <=2% of total; without a trend line there is no
// way to tell a memory leak from a hang after the fact. Deliberately a plain
// printf so it survives a low RNS_LOG_LEVEL.
#if defined(ESP32) && defined(HAS_RNS)
static void heap_watch() {
  static uint32_t last_heap_report = 0;
  if (millis() - last_heap_report < HEAP_REPORT_INTERVAL_MS) return;
  last_heap_report = millis();
  rtc_last_uptime_s = (uint32_t)(millis() / 1000);
  // Duty-cycle headroom and BLE state. Both are otherwise invisible: a node
  // silenced by airtime_lock looks exactly like one with nothing to say, and
  // BLE has no status output at all.
  printf("[duty] longterm=%.4f limit=%.4f locked=%d | [ble] state=%d\n",
         (double)longterm_airtime, (double)lt_airtime_limit,
         (int)airtime_lock, (int)bt_state);
  size_t total = RNS::Utilities::Memory::heap_size();
  size_t avail = RNS::Utilities::Memory::heap_available();
  // Guarded on UDP_TRANSPORT, not HAS_WIFI: the counters are defined in
  // Remote.h inside #if defined(UDP_TRANSPORT), so a Wi-Fi board built without
  // UDP transport (heltec_wifi_lora_32_V2/V3) fails to LINK, not to compile --
  // which is why building only the RAD-01 targets missed it.
  #if HAS_WIFI == true && defined(UDP_TRANSPORT)
    extern volatile uint32_t udp_rx_count; extern volatile uint32_t udp_tx_count;
    printf("[udp] rx=%lu tx=%lu\n", (unsigned long)udp_rx_count, (unsigned long)udp_tx_count);
  #endif
  // Is the modem ever actually keyed? packets_sent only counts queueing, so a
  // CSMA stall looks identical to a working transmitter from the RNS side.
  {
    extern volatile uint32_t tx_calls;
    #if MODEM == SX1262
      extern volatile uint32_t sx126x_preamble_count;
      extern volatile uint32_t sx126x_header_count;
      printf("[lora] tx_calls=%lu queue=%u dcd=%d rssi=%d nf=%d online=%d pre=%lu hdr=%lu "
             "locked=%d hwr=%d err=%d con=%d alock=%d "
             "f=%lu bw=%lu sf=%d txp=%d phy=%08lx\n",
             (unsigned long)tx_calls, (unsigned)queue_height, (int)dcd,
             (int)current_rssi, (int)noise_floor, (int)radio_online,
             (unsigned long)sx126x_preamble_count, (unsigned long)sx126x_header_count,
             (int)radio_locked, (int)hw_ready, (int)radio_error, (int)console_active,
             (int)airtime_lock,
             (unsigned long)lora_freq, (unsigned long)lora_bw, (int)lora_sf,
             (int)lora_txp, (unsigned long)lora_phy_hash());
    #elif MODEM == SX1276 || MODEM == SX1278
      extern volatile uint32_t sx127x_sigdet_count;
      extern volatile uint32_t sx127x_synced_count;
      printf("[lora] tx_calls=%lu queue=%u dcd=%d rssi=%d nf=%d online=%d sig=%lu syn=%lu "
             "locked=%d hwr=%d err=%d con=%d alock=%d f=%lu bw=%lu sf=%d txp=%d phy=%08lx\n",
             (unsigned long)tx_calls, (unsigned)queue_height, (int)dcd,
             (int)current_rssi, (int)noise_floor, (int)radio_online,
             (unsigned long)sx127x_sigdet_count, (unsigned long)sx127x_synced_count,
             (int)radio_locked, (int)hw_ready, (int)radio_error, (int)console_active,
             (int)airtime_lock, (unsigned long)lora_freq, (unsigned long)lora_bw,
             (int)lora_sf, (int)lora_txp, (unsigned long)lora_phy_hash());
    #else
      printf("[lora] tx_calls=%lu queue=%u dcd=%d rssi=%d nf=%d online=%d\n",
             (unsigned long)tx_calls, (unsigned)queue_height, (int)dcd,
             (int)current_rssi, (int)noise_floor, (int)radio_online);
    #endif
  }
  printf("[heap] free %u / %u bytes (%u%%), min-free %u, uptime %lus\n",
         (unsigned)avail, (unsigned)total,
         (unsigned)(total ? (avail * 100 / total) : 0),
         (unsigned)ESP.getMinFreeHeap(), (unsigned long)(millis() / 1000));

#if MCU_VARIANT == MCU_ESP32 && defined(HAS_RNS)
  // Why this line exists, next to a heap total that already looked sufficient:
  // Rev 1 was observed sliding from its recorded 25% free baseline to 3% over
  // roughly three and a half hours and then software-restarting, taking its
  // TCP listener and every Link down with it. A single free-bytes figure
  // cannot say why. These four can:
  //
  //   internal vs psram  -- whether the spill threshold is doing its job, or
  //                         whether growth is in allocations too small or too
  //                         DMA-bound to leave internal RAM;
  //   largest            -- exhaustion or fragmentation. A healthy free total
  //                         with a small largest block is fragmentation, and
  //                         no amount of capping tables will fix it;
  //   paths              -- the table is capped at 2000 records on a board
  //                         with 240 KB of internal RAM, four times what this
  //                         same file gives boards that have external flash.
  //                         If this number climbs with the curve, that is the
  //                         consumer and the cap is the fix.
  printf("[mem] internal=%u largest=%u psram=%u paths=%u/%u\n",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
         (unsigned)RNS::Transport::path_table().size(),
         (unsigned)RNS::Transport::path_table_maxsize());
#endif
}
#endif

void loop() {
#if defined(ESP32) && defined(HAS_RNS)
  heap_watch();
#endif
#if defined(HAS_RNS) && defined(URTN_STATS_PAGES)
  nomadnet_announce_watch();
#endif
#if defined(HAS_RNS) && defined(RRC_HUB)
  rrc_hub_loop();
#endif
#if defined(BLE_PEER_TRANSPORT)
  // Started lazily rather than at init: the GATT server does not exist until
  // Bluetooth has come up, and the transport identity is not loaded until
  // Reticulum has. Waiting for both here avoids ordering assumptions that
  // would fail silently.
  if (ble_peer_impl != nullptr && !ble_peer_impl->started() &&
      bt_state != BT_STATE_OFF && bt_state != BT_STATE_NA &&
      SerialBT.ble_server != nullptr && RNS::Transport::identity()) {
    ble_peer_impl->begin(SerialBT.ble_server, RNS::Transport::identity().hash());
  }
  if (ble_peer_impl != nullptr) ble_peer_impl->loop();
#endif
#if defined(HAS_RNS) && defined(LORA_TRANSPORT)
  radio_rx_watchdog();
  lora_config_consistency_watch();
  radio_commit_confirm_watch();
#if defined(LXMF_PROPAGATION_NODE)
  lxmf_propagation_announce_watch();
  lxmf_peer_sync_watch();
#endif
#endif

  #if MCU_VARIANT == MCU_NATIVE
    // Deferred-reboot hook: a KISS-driven property change or CMD_RESET in
    // a prior iteration called hard_reset() → native_request_reboot(), which
    // just set a flag. By the time we re-enter loop(), any KISS ACK from
    // that handler has already been written to the socket. Now perform the
    // cleanup + re-exec. native_reboot::perform() is [[noreturn]].
    extern bool native_reboot_pending();
    extern void native_reboot_perform();
    if (native_reboot_pending()) native_reboot_perform();
  #endif

#ifdef HAS_RNS
  // CBA
  if (reticulum) {
    try {
      reticulum.loop();
    }
    catch (const std::bad_alloc&) {
      ERROR("RNS loop failed: bad_alloc - out of memory");
    }
    catch (std::exception& e) {
      ERRORF("RNS loop failed: %s", e.what());
    }
  }
#endif

  if (radio_online) {
    #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NATIVE
      LoRa->handleDio0IfPending();
      modem_packet_t *modem_packet = NULL;
      if(modem_packet_queue && xQueueReceive(modem_packet_queue, &modem_packet, 0) == pdTRUE && modem_packet) {
        host_write_len = modem_packet->len;
        last_rssi      = modem_packet->rssi;
        last_snr_raw   = modem_packet->snr_raw;
        memcpy(&pbuf, modem_packet->data, modem_packet->len);
        free(modem_packet);
        modem_packet = NULL;

        kiss_indicate_stat_rssi();
        kiss_indicate_stat_snr();
        kiss_write_packet();
      }

      airtime_lock = false;
      if (st_airtime_limit != 0.0 && airtime >= st_airtime_limit) airtime_lock = true;
      if (lt_airtime_limit != 0.0 && longterm_airtime >= lt_airtime_limit) airtime_lock = true;

    #elif MCU_VARIANT == MCU_NRF52
      LoRa->handleDio0IfPending();
      modem_packet_t *modem_packet = NULL;
      if(modem_packet_queue && xQueueReceive(modem_packet_queue, &modem_packet, 0) == pdTRUE && modem_packet) {
        memcpy(&pbuf, modem_packet->data, modem_packet->len);
        host_write_len = modem_packet->len;
        free(modem_packet);
        modem_packet = NULL;

        portENTER_CRITICAL();
        last_rssi = LoRa->packetRssi();
        last_snr_raw = LoRa->packetSnrRaw();
        portEXIT_CRITICAL();
        kiss_indicate_stat_rssi();
        kiss_indicate_stat_snr();
        kiss_write_packet();
      }

      airtime_lock = false;
      if (st_airtime_limit != 0.0 && airtime >= st_airtime_limit) airtime_lock = true;
      if (lt_airtime_limit != 0.0 && longterm_airtime >= lt_airtime_limit) airtime_lock = true;

    #endif

      sample_loop_stack();
  tx_queue_handler();
    check_modem_status();
    #if MCU_VARIANT == MCU_NATIVE
      // Drop a TCP host client that's gone silent past the idle window.
      // poll_accept() in buffer_serial() handles the connect side; this
      // is the disconnect-side sweep.
      native_kiss_tcp::check_active();
    #endif

  } else {
    if (hw_ready) {
      if (console_active) {
        #if HAS_CONSOLE
          console_loop();
        #endif
      } else {
        led_indicate_standby();
      }
    } else {

      led_indicate_not_ready();
      stopRadio();
    }
  }

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
      buffer_serial();
      if (!fifo_isempty(&serialFIFO)) serial_poll();
  #else
    if (!fifo_isempty_locked(&serialFIFO)) serial_poll();
  #endif

  #if defined(ENABLE_WEBSOCKETS) && __has_include(<WiFi.h>)
    ws_console::service();
  #endif

  #if HAS_DISPLAY
    if (disp_ready && !display_updating) update_display();
  #endif

  #if HAS_PMU
    if (pmu_ready) update_pmu();
  #endif

  #if HAS_BLUETOOTH || HAS_BLE == true
    if (!console_active && bt_ready) update_bt();
  #endif

  #if HAS_WIFI
    if (wifi_initialized) update_wifi();
  #endif

  #if HAS_INPUT
    input_read();
  #endif

  // Feed WDT
#if MCU_VARIANT == MCU_ESP32
  esp_task_wdt_reset();
#elif MCU_VARIANT == MCU_NRF52
  NRF_WDT->RR[0] = WDT_RR_RR_Reload;
#endif

  if (memory_low) {
    #if PLATFORM == PLATFORM_ESP32
      if (esp_get_free_heap_size() < 8192) {
        kiss_indicate_error(ERROR_MEMORY_LOW); memory_low = false;
      } else {
        memory_low = false;
      }
    #else
      kiss_indicate_error(ERROR_MEMORY_LOW); memory_low = false;
    #endif
  }
}

void sleep_now() {
  #if HAS_SLEEP == true
    stopRadio(); // TODO: Check this on all platforms
    #if PLATFORM == PLATFORM_ESP32
      #if BOARD_MODEL == BOARD_T3S3 || BOARD_MODEL == BOARD_XIAO_S3
        #if HAS_DISPLAY
          display_intensity = 0;
          update_display(true);
        #endif
      #endif
      #if BOARD_MODEL == BOARD_HELTEC32_V4
          digitalWrite(LORA_PA_CPS, LOW);
          digitalWrite(LORA_PA_CSD, LOW);
          digitalWrite(LORA_PA_PWR_EN, LOW);
          digitalWrite(Vext, HIGH);
      #endif
      #if BOARD_MODEL == BOARD_HELTEC_TRACKER_V2
          digitalWrite(LORA_PA_CTX, LOW);
          digitalWrite(LORA_PA_CSD, LOW);
          digitalWrite(LORA_PA_PWR_EN, LOW);
          digitalWrite(Vext, VEXT_OFF);
      #endif
      #if PIN_DISP_SLEEP >= 0
        pinMode(PIN_DISP_SLEEP, OUTPUT);
        digitalWrite(PIN_DISP_SLEEP, DISP_SLEEP_LEVEL);
      #endif
      #if HAS_BLUETOOTH
        if (bt_state == BT_STATE_CONNECTED) {
          bt_stop();
          delay(100);
        }
      #endif
      esp_sleep_enable_ext0_wakeup(PIN_WAKEUP, WAKEUP_LEVEL);
      esp_deep_sleep_start();
    #elif PLATFORM == PLATFORM_NRF52
      #if BOARD_MODEL == BOARD_HELTEC_T114
        npset(0,0,0);
        digitalWrite(PIN_VEXT_EN, LOW);
        digitalWrite(PIN_T114_TFT_BLGT, HIGH);
        digitalWrite(PIN_T114_TFT_EN, HIGH);
      #elif BOARD_MODEL == BOARD_TECHO
        for (uint8_t i = display_intensity; i > 0; i--) { analogWrite(pin_backlight, i-1); delay(1); }
        epd_black(true); delay(300); epd_black(true); delay(300); epd_black(false);
        delay(2000);
        analogWrite(PIN_VEXT_EN, 0);
        delay(100);
      #endif
      sd_power_gpregret_set(0, 0x6d);
      nrf_gpio_cfg_sense_input(pin_btn_usr1, NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);
      NRF_POWER->SYSTEMOFF = 1;
    #endif
  #endif
}

void button_event(uint8_t event, unsigned long duration) {
  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    if (display_blanked) {
      display_unblank();
    } else {
      if (duration > 10000) {
        #if HAS_CONSOLE
          #if HAS_BLUETOOTH || HAS_BLE
            bt_stop();
          #endif
          console_active = true;
          console_start();
        #endif
      } else if (duration > 5000) {
        #if HAS_BLUETOOTH || HAS_BLE
          if (bt_state != BT_STATE_CONNECTED) { bt_enable_pairing(); }
        #endif
      } else if (duration > 700) {
        #if HAS_SLEEP
          sleep_now();
        #endif
      } else {
        #if HAS_BLUETOOTH || HAS_BLE
        if (bt_state != BT_STATE_CONNECTED) {
          if (bt_state == BT_STATE_OFF) {
            bt_start();
            bt_conf_save(true);
          } else {
            bt_stop();
            bt_conf_save(false);
          }
        }
        #endif
      }
    }
  #endif
}

volatile bool serial_polling = false;
void serial_poll() {
  serial_polling = true;

  #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52 && MCU_VARIANT != MCU_NATIVE
  while (!fifo_isempty_locked(&serialFIFO)) {
  #else
  while (!fifo_isempty(&serialFIFO)) {
  #endif
    char sbyte = fifo_pop(&serialFIFO);
    serial_callback(sbyte);
  }

  serial_polling = false;
}

#if MCU_VARIANT != MCU_ESP32
  #define MAX_CYCLES 20
#else
  #define MAX_CYCLES 10
#endif
void buffer_serial() {
  if (!serial_buffering) {
    serial_buffering = true;

    uint8_t c = 0;

    #if MCU_VARIANT == MCU_NATIVE
    // Refill the TCP staging buffer once per buffer_serial() pass —
    // accept any pending connection (or reject if we're already busy),
    // then drain whatever the kernel queued for the active client.
    native_kiss_tcp::poll_accept();
    while (c < MAX_CYCLES && native_kiss_tcp::available()) {
      c++;
      if (!fifo_isfull(&serialFIFO)) { fifo_push(&serialFIFO, native_kiss_tcp::read()); }
    }
    #else
    #if HAS_BLUETOOTH || HAS_BLE == true
    while (
      c < MAX_CYCLES &&
      #if HAS_WIFI
      ( (!bt_host_is_connected() && Serial.available()) || (bt_host_is_connected() && SerialBT.available()) || (wr_state >= WR_STATE_ON && wifi_remote_available()) )
      #else
      ( (bt_state != BT_STATE_CONNECTED && Serial.available()) || (bt_state == BT_STATE_CONNECTED && SerialBT.available()) )
      #endif
      )
    #else
    while (c < MAX_CYCLES && Serial.available())
    #endif
    {
      c++;

      #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52
        if (!fifo_isfull_locked(&serialFIFO)) { fifo_push_locked(&serialFIFO, Serial.read()); }
      #elif HAS_BLUETOOTH || HAS_BLE == true || HAS_WIFI
        // The Bluetooth arm is compiled only when a stack exists. The condition
        // above admits a Wi-Fi-only image, where bt_state can never leave
        // BT_STATE_NA and SerialBT is not declared at all.
        #if HAS_BLUETOOTH == true || HAS_BLE == true
        if      (bt_host_is_connected())         { if (!fifo_isfull(&serialFIFO)) { fifo_push(&serialFIFO, SerialBT.read()); } }
        #else
        if      (false)                          { }
        #endif
        #if HAS_WIFI
        else if (wifi_host_is_connected())       { if (!fifo_isfull(&serialFIFO)) { fifo_push(&serialFIFO, wifi_remote_read()); } }
        #endif
        else                                     { if (!fifo_isfull(&serialFIFO)) { fifo_push(&serialFIFO, Serial.read()); } }
      #else
        if (!fifo_isfull(&serialFIFO)) { fifo_push(&serialFIFO, Serial.read()); }
      #endif
    }
    #endif

    serial_buffering = false;
  }
}

void serial_interrupt_init() {
  #if MCU_VARIANT == MCU_1284P
      TCCR3A = 0;
      TCCR3B = _BV(CS10) |
               _BV(WGM33)|
               _BV(WGM32);

      // Buffer incoming frames every 1ms
      ICR3 = 16000;
      TIMSK3 = _BV(ICIE3);

  #elif MCU_VARIANT == MCU_2560
      // TODO: This should probably be updated for
      // atmega2560 support. Might be source of
      // reported issues from snh.
      TCCR3A = 0;
      TCCR3B = _BV(CS10) |
               _BV(WGM33)|
               _BV(WGM32);

      // Buffer incoming frames every 1ms
      ICR3 = 16000;
      TIMSK3 = _BV(ICIE3);

  #elif MCU_VARIANT == MCU_ESP32
      // No interrupt-based polling on ESP32
  #endif

}

#if MCU_VARIANT == MCU_1284P || MCU_VARIANT == MCU_2560
  ISR(TIMER3_CAPT_vect) { buffer_serial(); }
#endif
