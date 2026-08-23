# IMPR RAD-01 on microReticulum_Firmware — Install & Mesh Handoff

**Author:** Claude Code agent (RAD-01 bring-up session, 2026-08-20). Companion to
`~/projects/RNode_Firmware/RAD01_REV1_HANDOFF.md` and `ANP_RNS_BRIDGE_GAPS.md`.

**Goal:** put an IMPR RAD-01 on `microReticulum_Firmware` so the board becomes a
**self-contained Reticulum transport node** — routing on-device, with no host
attached — and join it to the Deck's existing mesh.

**Status:** repository cloned and surveyed. No build attempted, no device flashed.
Everything below is desk-verified against the cloned source; nothing is
hardware-confirmed yet.

---

## 1. What this firmware is, and why it is worth a look

A fork of RNode Firmware with the [microReticulum](https://github.com/attermann/microReticulum)
C++ stack embedded. Same version base (1.86 — `MAJ_VERS 0x01`, `MIN_VERS 0x56`),
but the board can run Reticulum Transport itself.

| | upstream RNode | microReticulum_Firmware |
|---|---|---|
| Last push | 2026-04-24 | **2026-08-04** |
| Role of the board | modem only | modem **or** standalone transport node |
| Relays with no host | no | **yes** (Transport Mode) |
| Wi-Fi host path | `Remote.h` KISS/TCP (broken — see gaps doc) | `UDPInterface.h`, a native RNS interface |
| PSRAM | undocumented boot dependency | first-class (`RNS_PSRAM_ALLOCATOR` for path table) |
| Extras | — | web console, provisioning subsystem, NomadNet stats pages |

The repository also carries `Provisioning.cpp/h`, `WebSocketConsole`, a `webconsole/`
tree, `boards/` + `variants/` for nRF52 parts, and a `native/` build. It is
visibly more developed than upstream.

**Cloned at:** `~/projects/microReticulum_Firmware`, HEAD `592c826`
(*"feat: optimize BLE advertising intervals…"*).

---

## 2. The important difference for our mesh

`Remote.h` in upstream exposes a **raw KISS socket** that a host drives — that is
the path that failed all day. This fork instead ships `UDPInterface.h`, which is a
**real Reticulum interface** (`class UDPInterface : public RNS::InterfaceImpl`)
doing UDP broadcast:

```c
#define UDP_LOCAL_HOST  "0.0.0.0"
#define UDP_REMOTE_HOST "255.255.255.255"
#define UDP_PORT        4242
```

This is a completely different implementation from the one carrying gaps G1–G4.
It is the single strongest reason to try this fork: the board joins the mesh as a
**peer**, not as a modem someone has to hold open a socket to.

Rev 1's Wi-Fi association is already proven working (STA, DHCP, stable ping), so
the radio and stack underneath are known good — only the transport above them was
broken.

---

## 3. Work required before it will build for RAD-01

### 3.1 Board codes — `0x45` is taken here

Upstream let us use `BOARD_RAD01_REV1 = 0x45`. **In this fork `0x45` is
`BOARD_HELTEC_TRACKER_V2`.** Codes in use: `0x31`–`0x42`, `0x44`, `0x45`, `0x50`,
`0x51`, `0x52`, `0x60`.

Use free codes:

```c
#define BOARD_RAD01_REV1  0x46
#define BOARD_RAD01_REV2  0x47
```

### 3.2 Board definitions

`Boards.h` has the same structure as upstream, so the Rev 1 definition ports
across unchanged apart from the code. Both, verified from PCB pad-to-net:

**Rev 1 — ESP32-S3 + RFM95W (SX1276)**
```c
#elif BOARD_MODEL == BOARD_RAD01_REV1
  #define IS_ESP32S3 true
  #define HAS_CONSOLE false
  #define HAS_SD false
  #define HAS_EEPROM true
  #define HAS_WIFI true
  #define HAS_BLUETOOTH false
  #define HAS_BLE true
  const int pin_cs = 13;  const int pin_reset = 14;
  const int pin_sclk = 10; const int pin_mosi = 11; const int pin_miso = 12;
  const int pin_dio = 5;
  const int pin_led_rx = 4; const int pin_led_tx = 4;   // active LOW
```

**Rev 2 — ESP32-S3 + Ra-01SH (SX1262)**
```c
#elif BOARD_MODEL == BOARD_RAD01_REV2
  #define IS_ESP32S3 true
  #define HAS_CONSOLE false
  #define HAS_SD false
  #define HAS_EEPROM true
  #define HAS_WIFI true
  #define HAS_BLUETOOTH false
  #define HAS_BLE true
  #define HAS_BUSY true
  #define HAS_TCXO false            // Ra-01SH uses a plain crystal, NOT a TCXO
  #define DIO2_AS_RF_SWITCH false   // U2 DIO2 is physically unconnected
  #define HAS_RF_SWITCH_RX_TX true
  const int pin_rxen = 7;   const int pin_txen = 21;
  const int pin_cs = 13;    const int pin_reset = 14;
  const int pin_sclk = 10;  const int pin_mosi = 11; const int pin_miso = 12;
  const int pin_busy = 5;   const int pin_dio = 6;    // DIO1 = IRQ
  const int pin_led_rx = 4; const int pin_led_tx = 4; // active LOW
```

Also required for both, as upstream:
- `sx127x.cpp` — add `BOARD_RAD01_REV1` to the custom `SPI.begin(...)` condition.
- `Utilities.h` — active-low LED branch; `eeprom_model_valid()` accepting `MODEL_FE`.
- `RNode_Firmware.ino` — exclude the board from the `while (!Serial)` boot gate.

### 3.3 Rev 2 only — the TXEN gap is still present in this fork

Confirmed: `sx126x.cpp` here has **no `_txen` support**, exactly as upstream.
`pin_txen` is consumed only by `sx128x.cpp`. Rev 2 wires TXEN (IO21) and RXEN
(IO7) and leaves DIO2 unconnected, so with stock code the TX path is never
enabled.

**Rev 2 cannot transmit correctly until `sx126x.cpp` drives TXEN.** Rev 1 is
unaffected (SX1276, no RF switch).

> ⚠ **Module variant warning.** `REV2_HANDOFF.md` specifies **Ra-01SH-P**;
> `production/bom.csv` orders plain **Ra-01SH** (`C2764087`). Their pinouts differ
> on exactly these pins — plain: pin 5 = TXEN, pin 11 = RXEN; `-P`: pin 5 =
> **VCCPA** (PA power input, 5 V) and pin 11 = **RF_EN**. On a `-P` module the
> layout would drive GPIO21 into a PA supply rail rated 750 mA–1 A. **Confirm the
> fitted part before powering Rev 2.**

### 3.4 PlatformIO environments

Base `[env]` already sets `-DHAS_RNS`, `-DHAS_PROVISIONING`, `-DLORA_TRANSPORT`,
`-DRNS_USE_FS`, `-DRNS_LOW_MEMORY_REBOOT`. Model the RAD envs on
`[env:lilygo-t3-s3]`, which is ESP32-S3 + SX1262 + 8 MB PSRAM — nearly our profile:

```ini
[env:impr-rad01-rev1]
extends = env:embedded
platform = espressif32
board = esp32-s3-devkitc-1
board_build.partitions = no_ota.csv
board_build.filesystem = littlefs
build_flags =
	${env:embedded.build_flags}
	-DBOARD_MODEL=BOARD_RAD01_REV1
	-DMODEM=SX1276
	-DBOARD_HAS_PSRAM=1
	-DRNS_CONTAINER_ALLOCATOR=RNS_PSRAM_ALLOCATOR
	-DUSTORE_USE_POSIXFS=1
	-DUDP_TRANSPORT
lib_deps =
	${env:embedded-esp32.lib_deps}
	https://github.com/attermann/microStore.git
	https://github.com/attermann/microReticulum.git
```

`-DUDP_TRANSPORT` is what brings up the Wi-Fi mesh interface. Add
`-DRNS_PATH_TABLE_SEGMENT_SIZE` / `_COUNT` tuning later if the path table grows.

**Our 8 MB PSRAM is an asset here**, not the liability it was upstream — it backs
the RNS path table via `RNS_CONTAINER_ALLOCATOR`.

---

## 4. Install

Prerequisite: PlatformIO (`pip install platformio` into the RNS venv, or the
VSCode extension). The repo is configured for PlatformIO, not arduino-cli.

**Option A — prebuilt, for a *supported* board only** (not RAD-01):
```
rnodeconf --clear-cache
rnodeconf --autoinstall --fw-url https://github.com/attermann/microReticulum_Firmware/releases/
```

**Option B — build our variant (this is our path):**
```
cd ~/projects/microReticulum_Firmware
pio run -e impr-rad01-rev1
pio run -e impr-rad01-rev1 -t upload --upload-port /dev/ttyACM1
```

Then **re-authorize the firmware hash** — without it the device reports invalid
firmware and refuses to fully initialise (gap G7):
```
HASH=$(~/.local/share/rnode-rns-venv/bin/python ~/projects/RNode_Firmware/partition_hashes .pio/build/impr-rad01-rev1/firmware.bin)
~/.local/share/rnode-rns-venv/bin/rnodeconf /dev/ttyACM1 -H $HASH
```

Existing provisioning (`f0:fe:45`, hwrev 1) survives an app-only flash and does
not need redoing.

---

## 5. Joining the Deck mesh

### 5.1 Default — behaves as an ordinary RNode

Out of the box it acts like any RNode, so the Deck's current
`RNodeInterface[RAD-01 Rev1 USB]` keeps working with no config change. Good first
smoke test: prove the port didn't regress anything.

### 5.2 Transport Mode — the actual objective

Switch the device to TNC mode to activate the on-board RNS:
```
rnodeconf --tnc --freq 867200000 --bw 125000 --sf 8 --cr 5 --txp 7 /dev/ttyACM1
```

> ⚠ **Per upstream's README, serial logging from the embedded RNS clobbers KISS in
> this mode — do not attach another RNS to the device while in Transport Mode.**
> Practically: **remove the `RAD-01 Rev1 USB` interface from `~/.reticulum/config`
> before switching**, or `rnsd` and the device will fight. Same one-owner
> constraint as gap G5.

### 5.3 Meeting the Deck over Wi-Fi

With `-DUDP_TRANSPORT` the board broadcasts on **UDP 4242**. Add a matching
interface to `~/.reticulum/config`:

```ini
  [[RAD-01 UDP Mesh]]
    type = UDPInterface
    enabled = yes
    listen_ip = 0.0.0.0
    listen_port = 4242
    forward_ip = 255.255.255.255
    forward_port = 4242
```

Note the existing `Columba LAN TCP Server` also uses 4242 but over **TCP** — no
conflict, though renaming one avoids confusion later.

Wi-Fi credentials are already stored in the device EEPROM
(`FiberHGW_ZTXK5F_2.4GHz`, DHCP) and survive an app-only flash.

### 5.4 Verification

1. `rnstatus` on the Deck shows the new `UDPInterface` **Up**.
2. The board appears as a **peer**, not merely an interface — an announce arrives
   from the device's own identity.
3. `rnpath <deck-lxmf-address>` from a mesh peer resolves **through** the board.
4. Decisive test: **unplug USB, leave the board on wall power.** If traffic still
   routes, it is genuinely standalone — the capability upstream RNode cannot
   provide.

---

## 6. Risks and rollback

- **This replaces RNode firmware.** Recovery images for every state reached today
  are in `~/RNode_Backups/RAD01_Rev1_80B54EF4C7A4_2026-08-20/`. Known-good:
  `RNode_Firmware_1.86_RAD01_WiFi_BLE_PSRAM.bin`, partition hash
  `787a0b6d8e88e221f288e8a8c91de154075b32172b4884732ec93d7e101589d7`. Restore:
  ```
  esptool.py --chip esp32s3 --port /dev/ttyACM1 --before usb-reset --after hard-reset \
    write-flash 0x10000 <image.bin>
  ```
- **No console on Rev 1.** Panic output goes to UART0 (GPIO43/44), not USB. This
  firmware is far heavier than stock RNode — an embedded stack, filesystem and
  provisioning engine — so a boot failure is materially more likely and much
  harder to diagnose blind. **Wiring IMPR-POW-C-02 to UART0 before this attempt is
  strongly advised** (gap G8).
- **Memory.** `RNS_LOW_MEMORY_REBOOT` reboots above 98% usage. Watch for reboot
  loops; PSRAM should give headroom but this is unmeasured on our board.
- **Unverified.** Nothing here has been built or flashed. Footprint, stability,
  Transport completeness and throughput on ESP32-S3 are all unmeasured.

---

## 7. Suggested order

1. Wire POW-C-02 → RAD-01 Rev 1 UART0 for a console. Everything else is cheaper
   afterwards.
2. Add board codes `0x46`/`0x47`, both board definitions, and the PlatformIO envs.
3. Build `impr-rad01-rev1`. Do not flash on the first green build — inspect size
   and the linked feature set first.
4. Flash Rev 1, re-authorize the hash, confirm it still works as a plain RNode
   (§5.1).
5. Only then enable Transport Mode and the UDP interface (§5.2–5.3).
6. Rev 2 afterwards, and **only** once the module variant is confirmed and the
   `sx126x.cpp` TXEN patch is written.

---

## 8. Rev 2 addendum — flashing a board with a dead USB port

**Added 2026-08-20.** Everything in this section is read out of the as-built Rev 2
design at `~/projects/kicad_labs/lab6_mcu_lora/rev2/IMPR-RAD-01/`
(`lab6_mcu_lora.kicad_pcb` pad→net map, `mcu.kicad_sch` symbol pin names,
`production/bom.csv`). It supersedes the desk estimates in §3 where they differ.

### 8.1 The module-variant warning in §3.3 is resolved — plain Ra-01SH

`production/bom.csv` orders **`Ra-01SH`, LCSC `C2764087`** — the plain part, not
`-P`. The schematic agrees with that pinout and not with the `-P` one:

| U2 pin | Net | Plain Ra-01SH | Ra-01SH-P |
|---|---|---|---|
| 5  | `LORA_TXEN` → IO21 | TXEN | VCCPA (5 V PA rail) |
| 11 | `LORA_RXEN` → IO7  | RXEN | RF_EN |
| 7  | **unconnected**    | DIO2 | DIO2 |

The layout drives IO21 into a logic input, not a PA supply rail. **The "do not
power Rev 2" hold from §3.3 can be lifted.** The comment on `[env:impr-rad01-rev2]`
in `platformio.ini` has been updated to record this.

### 8.2 Board definition verified against the netlist

Every pin in the `BOARD_RAD01_REV2` block of `Boards.h` was checked against the
`U1` (ESP32-S3-WROOM-1) pad→net map. All correct: CS 13, RST 14, SCLK 10, MOSI 11,
MISO 12, BUSY 5, DIO1 6, RXEN 7, TXEN 21. `pin_led_* = 4` is right and is active
low — D2's cathode is on IO4, anode through R10 (330R) to 3V3. `DIO2_AS_RF_SWITCH
false` matches U2 pin 7 being unconnected.

The §3.3 TXEN gap is **closed** in the working tree: `sx126x.cpp` now has
`txAntEnable()`, `endPacket()` asserts TXEN, and `receive()`/`begin()` deassert it
via `rxAntEnable()`.

Still open: `HAS_TCXO false` is *not* verifiable from the schematic, because the
crystal-vs-TCXO choice is internal to the module and DIO3 (U2 pin 8) is left
unconnected on our board either way. Confirm from the Ai-Thinker datasheet. If the
module does carry a DIO3-powered TCXO, the symptom is an XOSC start failure — the
radio never leaves standby and nothing is received.

### 8.3 J3 is the flashing header

`J3` and `J4` are 1×14 **2.54 mm through-hole** headers on the top layer, along the
top edge of the board (J3 starts ~4.9 mm in from the left edge, 2.5 mm down; J4 is
the matching row 40.6 mm below it). Neither is in the production BoM, so the holes
are bare — solder a header or wires straight in.

J3 carries a complete flashing/console interface:

| J3 pin | Net | Goes to |
|---|---|---|
| 1 | `VREG_5V` | 5 V rail (LDO input) |
| 2 | `GND` | |
| 3 | `MB_EN` | LDO enable, diode-OR'd with the slide switch |
| 4 | `VBAT_SNS` | |
| **5** | **`UART_RX`** | **U1 pin 37 = TXD0 = GPIO43 — board TX, wire to bridge RX** |
| **6** | **`UART_TX`** | **U1 pin 36 = RXD0 = GPIO44 — board RX, wire to bridge TX** |
| 7 | `MB_DTR` | Q2 gate → IO0 (see §8.5) |
| 8 | `MB_RTS` | Q1 gate → EN (see §8.5) |
| 9–14 | SUSP_A/B, LCD BL/RST/DC/CS | |

Net names are **host-referenced**: `UART_TX` is the host's TX line, so bridge TX →
J3.6 and bridge RX → J3.5. No crossover to reason about.

J4 exposes the JTAG quartet (TCK/TDO/TMS/TDI on IO39–42) if the UART path also
fails, and `IO45` on pin 13 — the VDD_SPI strap that `docs/frozen-pinmap.md` flags
as having no pull. If the board will not boot at all, tie J4.13 to GND first.

### 8.4 Powering it without USB

D4 (BAT60A) is anode-on-`VBUS_USB`, cathode-on-`VREG_5V`, so feeding 5 V into J3.1
powers the board and **cannot** backfeed the damaged USB port. Two conditions:

1. The bridge and the board must share ground (J3.2).
2. **SW5 (the slide switch) must be in the on position.** It switches `MAN_EN`
   between GND and `VREG_5V`; `MAN_EN` and `MB_EN` diode-OR through D1 into
   `BOARD_EN`, which is the enable pin of U3 (AP7361C). R1 pulls `BOARD_EN` down,
   so with the switch off there is no 3V3 rail at all and the board looks dead.

Measure 3V3 before concluding anything about USB damage.

### 8.5 The auto-reset circuit is polarity-inverted vs esptool

Rev 2 does not use the cross-coupled two-transistor circuit esptool expects. It
uses two gate-driven 2N7002 N-MOSFETs:

```
MB_RTS --10k(R13)--> Q1.gate ;  Q1.source = GND ; Q1.drain = ESP_CHIP_PU (EN)
MB_DTR --10k(R14)--> Q2.gate ;  Q2.source = GND ; Q2.drain = MCU_BOOT   (IO0)
```

Gate HIGH pulls the line LOW. A USB-serial bridge drives DTR#/RTS# LOW when the
signal is *asserted*, so asserting RTS **releases** EN here, where esptool's
`ClassicReset`/`UnixTightReset` assume asserting RTS **pulls EN low**. Both lines
are inverted. Two consequences:

- **`pio run -t upload` with default reset flags will not enter the bootloader**,
  and `--after hard_reset` ends with RTS deasserted, which on this board leaves the
  chip *held in reset*.
- **An idle bridge holds the board in reset.** With nothing holding the port open,
  DTR#/RTS# sit HIGH, both FETs conduct, and EN is pulled low. There are no gate
  pull-downs, so `MB_DTR`/`MB_RTS` also float when the carrier is unplugged.

**Update after bring-up (§9): wiring DTR/RTS worked well** with the inverted
sequence in `tools/rad01_rev2_uart_esptool.cfg`, giving fully automated flashing.
The cost is real though: the board is held in reset whenever the port is closed,
and reboots every time any program opens it. Original advice retained below for a
first attempt.

**Conservative option: do not wire DTR/RTS at all.** Four wires — 5 V, GND, TX, RX — and
use the buttons, which are all populated: **S1 = RESET (EN), S3 = BOOT (IO0)**.

### 8.6 Procedure

```
# 1. Wire bridge TX -> J3.6, bridge RX -> J3.5, GND -> J3.2, 5V -> J3.1.
#    Slide SW5 on. Confirm 3V3.

# 2. Build. Note the -uart env: it is Rev 2 with USB CDC disabled (see §8.7).
pio run -e impr-rad01-rev2-uart

# 3. Enter the bootloader by hand: hold S3 (BOOT), tap S1 (RESET), release S3.

# 4. Upload. The env already passes --before=no_reset --after=no_reset.
pio run -e impr-rad01-rev2-uart -t upload --upload-port /dev/ttyUSB0

# 5. Tap S1 to run the new firmware.
```

To automate the reset instead of using the buttons, wire DTR→J3.7 and RTS→J3.8,
accept that the board sits in reset while the port is closed, and use the inverted
sequences in `tools/rad01_rev2_uart_esptool.cfg`:

```
ESPTOOL_CFGFILE=tools/rad01_rev2_uart_esptool.cfg \
  esptool.py --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 \
    --before default_reset --after hard_reset \
    write_flash 0x10000 .pio/build/impr-rad01-rev2-uart/firmware.bin
```

(That file is deliberately *not* named `esptool.cfg` — `custom_reset_sequence`
short-circuits esptool's whole reset-strategy selection, including the
USB-JTAG-Serial detection Rev 1 depends on for native-USB flashing. Keep it opt-in
through `ESPTOOL_CFGFILE`.)

### 8.7 Without this env change, a flashed board is mute

`[env:impr-rad01-rev2]` builds with `-DARDUINO_USB_CDC_ON_BOOT=1`, which binds the
Arduino `Serial` object — the object carrying KISS and the console — to the USB
peripheral. On a board with a broken USB port that firmware flashes fine over UART
and then says nothing, on any interface.

`[env:impr-rad01-rev2-uart]` unflags it and sets `-DARDUINO_USB_CDC_ON_BOOT=0`, so
`Serial` is UART0 on GPIO43/44 and the same four wires used for flashing carry
KISS afterwards at 115200. Verified resolved: the build sees only
`ARDUINO_USB_CDC_ON_BOOT=0`. The `while (!Serial)` boot gate is already excluded
for both RAD-01 board codes, and `extra_script.py`'s KISS firmware-hash step talks
to `$UPLOAD_PORT` at 115200, so it works unchanged over the bridge — note that
opening the port does *not* reset this board, so its 4-second "wait for boot" drain
is harmless but does nothing.

`monitor_dtr = 1` / `monitor_rts = 1` are set on the env on purpose: asserted means
the bridge pins are LOW, which is the state that leaves EN and IO0 released.

### 8.8 Worth fixing in Rev 3

- **Gate pull-downs (10k, gate to GND) on Q1 and Q2.** Without them, a floating or
  idle `MB_DTR`/`MB_RTS` can hold the ESP in reset or in download mode. This is the
  most likely cause of a Rev 2 board that appears dead for no reason.
- Match the auto-reset polarity to the standard cross-coupled circuit so stock
  esptool and PlatformIO work with no configuration.
- Add the pull on IO45 that `docs/frozen-pinmap.md` already flags as deferred.
- Bring UART0 + EN + IO0 out to a dedicated 6-pin programming header, so a broken
  USB port is a five-minute recovery rather than an investigation.


---

## 9. Bring-up log — Rev 2 flashed over the UART bridge (2026-08-20)

**Status: succeeded.** The board is flashed, provisioned, and its radio initialises.
Hardware: CP2102N bridge on `/dev/ttyUSB0`, all six lines wired to J3 (5V, GND, TX,
RX, DTR, RTS). Board MAC `80:b5:4e:f4:c7:c4`, device UID `80B54EF4C7C5`.

### 9.1 What the board reported

```
Chip is ESP32-S3 (QFN56) revision v0.2
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
Detected flash size: 8MB, quad (4 data lines), eFuse flash voltage 3.3V
```

PSRAM and flash match the `qio_opi` / N8R8 build assumptions. Pre-existing flash
contents were an `arduino-lib-builder` factory stub (ESP-IDF v4.4.7, Mar 2024) with
an empty filesystem region — backed up regardless to
`~/RNode_Backups/RAD01_Rev2_80B54EF4C7C4_2026-08-20/preflash_full_0x0-0x400000.bin`.

The inverted reset sequence worked on the first attempt, so `--before default_reset`
and `--after hard_reset` are both usable with `ESPTOOL_CFGFILE` set. Under
PlatformIO, override the env's `no_reset` flags with a **newline-separated**
`PLATFORMIO_UPLOAD_FLAGS` — a space-separated string is passed as a single argument
and esptool rejects it.

### 9.2 Two firmware issues the UART path exposed

**1. `Serial.setRxBufferSize()` was being called after `Serial.begin()`.** Arduino's
`HardwareSerial` refuses to resize a running port, so the KISS RX buffer silently
stayed at the default 256 bytes instead of `CONFIG_UART_BUFFER_SIZE` (6144 on
ESP32) — a 24x shortfall on the link that carries KISS. Invisible on every other
board in this repo because USB CDC is a different class with no such restriction.
Fixed in `RNode_Firmware.ino`: the call now precedes `Serial.begin()`, guarded to
`MCU_VARIANT == MCU_ESP32`. The `[E][HardwareSerial.cpp:526]` warning is gone.

**2. TRACE-level RNS logging makes KISS unusable on a UART-only board.** `[env]`
sets `RNS_LOG_LEVEL_TRACE`; `[env:embedded-esp32]` unflags it but the ESP32 board
envs — ours, `lilygo-t3-s3`, `heltec-wireless-tracker-v2` — all extend
`env:embedded` instead, so TRACE survives. The log stream interleaves with KISS
framing and breaks frame parsing outright: `rnodeconf` reported *"Serial port
opened, but RNode did not respond. Is a valid firmware installed?"* until the level
was dropped. `[env:impr-rad01-rev2-uart]` now compiles at `RNS_LOG_LEVEL_ERROR`,
which cut steady-state chatter from 5693 to 1119 bytes per 15 s **and** reduced the
app image from 87.1% to 84.6% of the 2 MB partition. This is a pre-existing
property of the repo, not something the UART path introduced — it is simply fatal
here, where the debug console and the KISS transport are the same wire.

### 9.3 Fresh Rev 2 boards need provisioning — §4's assumption is Rev-1-only

`extra_script.py` prints *"Preserving existing RAD-01 EEPROM provisioning"* for both
RAD-01 variants. That is correct for Rev 1, which was already provisioned upstream,
but a new Rev 2 board has an empty EEPROM and boots with:

```
[init] Device unprovisioned, no device configuration found in EEPROM
[init] hw_ready: 0
```

`hw_ready: 0` means the radio never initialises. Provision it once (hex codes, no
`0x`; `hwrev` only has to be non-zero and non-`0xFF`):

```
rnodeconf --product f0 --model fe --hwrev 2 --rom /dev/ttyUSB0
```

After that the board reports:

```
Product            : Hombrew RNode (Band capabilities unknown) (f0:fe:47)
Device signature   : Validated - Local signature
Hardware revision  : 2
Max TX power       : 17 dBm
Device mode        : Normal (host-controlled)
```

and boots with **`hw_ready: 1`** — the Ra-01SH answers over SPI and RNS reaches
`RNS is READY!`. Note `f0:fe:47`: the third byte is the `BOARD_RAD01_REV2` code
emitted by the firmware, not an argument to `rnodeconf`. Rev 1 on this fork will
report `f0:fe:46`, not the `f0:fe:45` recorded in §4 — that was the upstream code.

### 9.4 What is proven, and what is not

Proven: flashing and console over UART0, PSRAM, filesystem, provisioning, EEPROM
signature, SX1262 SPI bring-up (`hw_ready: 1`), RNS init with both LoRaInterface
and UDPInterface registered.

**Not proven: anything RF.** `hw_ready: 1` only means the modem answered over SPI.
No packet has been transmitted or received, so the TXEN patch, the antenna path,
and the `HAS_TCXO false` assumption from §8.2 are all still unverified. The board is
in `Normal (host-controlled)` mode with no radio configuration; Transport Mode
(§5.2) and the UDP mesh interface (§5.3) are the next steps and have not been
attempted.

Also unmeasured: RNS boot time grew from ~6 s to ~40 s once the radio initialised.
Not obviously alarming, but worth understanding before relying on the board.


---

## 10. Node configuration — WiFi, name, and the UDP bind bug (2026-08-21)

The board is on the Deck's mesh as **`IMPR-RAD-01-REV2`**, associated to
`FiberHGW_ZTXK5F_2.4GHz` at **192.168.1.88**, announcing its NomadNet site at
`f45ee928f68445b04ab698743795fb77` (confirmed 1 hop away over
`UDPInterface[RAD-01 UDP Mesh]` in `rnpath -t`).

### 10.1 WiFi — set over KISS, not provisioning

The Provisioning network namespace exposes SSID but **no password field**, so
credentials must go through the raw KISS commands, which write the 256-byte config
block: `CMD_WIFI_SSID 0x6B`, `CMD_WIFI_PSK 0x6C` (both take the string followed by a
`0x00` terminator, which is what triggers the EEPROM write), then `CMD_WIFI_MODE
0x6A` with `WR_WIFI_STA = 0x01`, which saves and calls `wifi_remote_init()`
immediately. Script: `scratchpad/wifi_cfg.py`. The board reports `[WiFi] status: 3`
(`WL_CONNECTED`) and the settings survive reboot.

### 10.2 Node name — over the Provisioning protocol

`nomadnet_name` is `PROV_GENERAL_NOMADNET_NAME` = field **5** in namespace
`PROV_NS_GENERAL` = **100**. There is no host-side client in the repo, so one was
written: `scratchpad/prov.py` (hand-rolled MsgPack, no external deps).

Wire format, learned by probing — **the envelope is an ARRAY, not a map**:
`[op, seq, payload]`. A map gets `Error 1: "envelope must be array"`. Ops are in the
library's `Provisioning/Ops.h`: GetSchema 1, GetInfo 2, GetCapabilities 3, GetState
4, SetState 5, Commit 6, Reboot 9, Ack 100, Error 101. `GetState`'s namespace filter
must be an **array** (`{1: [100]}`); a bare int returns nothing.

Setting a field is two ops — `SetState` only creates a draft:

```python
p.request(5, {3: {100: {5: "IMPR-RAD-01-REV2"}}})   # SetState -> {1:1, 2:True}
p.request(6, {1: [100], 5: True})                    # Commit   -> Applied, NeedsReboot
```

Verified to survive a cold boot. Useful namespaces: 100 General, 101 Radio, 102
Network, 103 Metrics, 105 LoRaInterface, 106 UDPInterface, 107 Addresses (RNS
destination hashes), 62 Metrics.

### 10.3 Bug: the UDP socket binds before DHCP and is never rebound

**Symptom.** Board associates and is pingable, but never appears on the mesh.
`UDPInterface` (ns 106) reports an **empty** IP address and every transport metric
in ns 62 reads zero.

**Cause.** `wifi_remote_start()` in `Remote.h` sets `wifi_initialized = true` and
calls `udp.begin(udp_port)` directly after `WiFi.begin()` plus a 500 ms delay —
seconds before DHCP returns an address. The socket binds with no local IP, and
nothing ever reopens it: `wifi_remote_start()` is reached only via
`wifi_remote_init()`, which re-runs only when the station **drops**. A station that
connects normally therefore never gets a working UDP socket. `send_outgoing()` gates
on `wifi_initialized`, not on `WiFi.status() == WL_CONNECTED` (that check is present
but commented out), so it believes it is transmitting.

**Fix applied** in `wifi_update_status()` (`Remote.h`): when the station reports
`WL_CONNECTED` with a non-zero address that differs from the one currently bound,
`udp.stop()` / `udp.begin(udp_port)` and record it in `udp_bound_ip`. One rebind per
address change. After this the announce at ~t+40 s goes out over a working socket
and the Deck resolves the board.

This is not RAD-01-specific — it affects any board using `-DUDP_TRANSPORT` in STA
mode, which is the whole point of §2.

### 10.4 Still open

- **The board only runs while something holds the serial port open.** With RTS wired
  to J3.8, an idle bridge holds EN low, so the node dies the moment the port closes
  — verified: with the port closed it does not answer a ping sweep. For genuinely
  standalone operation (the §5.4 objective), **disconnect RTS from the bridge and
  tie J3.8 to GND**, which holds Q1 off permanently. Move it back to the bridge to
  flash, or use the S3/S1 buttons. This is the §8.8 gate-pull-down item, and it is
  now the single thing between this board and the "unplug it and it still routes"
  test.
- **No web console.** `HAS_CONSOLE false` in the Rev 2 board definition gates it out;
  the board has no listening TCP port. Set it true and rebuild if the console is
  wanted.
- **The NomadNet site is announced exactly once**, at startup (`RNode_Firmware.ino`
  line ~1151). There is no periodic re-announce, so anything that misses that single
  packet will not learn the node until it reboots.
- Transport Mode itself is still **disabled** (`op_mode 17`, no radio config). The
  board is on the mesh as a NomadNet node over WiFi/UDP, but §5.2 has not been done
  and nothing has been transmitted over LoRa.


---

## 11. Rev 1 "goes unresponsive when unplugged" — root cause (2026-08-21)

**Answer: the board runs out of internal RAM and restarts itself.** Not a crash,
not a brownout, not the LoRa/WiFi hardware.

### 11.1 Evidence

**It never panicked.** `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` in the Arduino
ESP32-S3 build, and Rev 1 uses the same `no_ota.csv` layout with a coredump
partition at `0x3F0000`. That partition read back **entirely `0xFF`** — erased. A
panic, abort, or task-watchdog would have written an ELF core dump there.

**It restarts itself.** The persistent boot log added in this session (§11.3)
shows, across real field boots:

```
boot reason=SW (ESP.restart)     <- four of these
boot reason=UNKNOWN              <- host USB-JTAG resets, i.e. mine
```

`ESP_RST_SW` means something called `esp_restart()`. Every such call site in this
stack is a memory-exhaustion path:

| Site | Trigger |
|---|---|
| `Reticulum.cpp:280` | `RNS_LOW_MEMORY_REBOOT` — free heap <= 2% |
| `Reticulum.cpp:258` | `bad_alloc` in `Reticulum::loop` |
| `Interface.cpp:56`  | `bad_alloc` in `Interface::send_outgoing` |
| `Interface.cpp:79`  | `bad_alloc` in `Interface::handle_incoming` |

(`Utilities.h:434` is the firmware's own `hard_reset()`, reached only via a
provisioning Reboot op or `CMD_RESET` — not in play here.)

This fits every observation: no core dump (a clean restart writes none), no ICMP
response while it cycles, onset "after some time" as the heap fills, and — the key
one — **fine while tethered**, because the Deck's `rnsd` carries the transport
work then. Standalone, the board holds the path, link and announce tables itself.

**Correction to an earlier claim in this session.** The `known_destinations_maxsize(50)`
cap at `RNode_Firmware.ino:930` was blamed first. That was wrong: Rev 1's store
holds ~25 objects, well under the cap, and its filesystem is 98% free. The
`remember: failed to store identity` / `Failed to add destination ... to path table!`
errors seen on Rev 2 are **downstream symptoms of the same memory pressure**
(allocation failures), not the ceiling being hit.

### 11.2 The lever: PSRAM is sitting idle

Measured on both boards via the provisioning metrics namespaces:

| | Rev 1 | Rev 2 (fresh boot) |
|---|---|---|
| Internal heap free | 75,020 / 271,084 (**27%**) | 92,516 / 272,724 (**33%**) |
| PSRAM free | — | 8,356,547 / 8,385,415 (**99%**) |
| Filesystem free | — | 1,941,504 / 1,966,080 (98%) |

**8 MB of PSRAM is 99% unused while the internal heap is the binding constraint**,
even though `-DRNS_CONTAINER_ALLOCATOR=RNS_PSRAM_ALLOCATOR` is set on both RAD
envs. Whatever is consuming internal heap is not being routed to PSRAM. That is
the thing to fix — it is also the difference between a node that survives a day
and one that reboots every few hours. It has not been attempted yet.

Startup time is a corroborating signal: Rev 2's time-to-`RNS is READY` grew
6 s -> 40 s -> **11.5 minutes** across boots. `setup()` blocks for that whole
period, so `loop()` never runs and nothing is serviced — an unresponsiveness
window in its own right, and consistent with allocation getting progressively
harder.

### 11.3 Instrumentation added (both boards)

- **`esp_reset_reason()` at boot**, printed with its numeric code:
  `[boot] reset reason: SW (ESP.restart) (3)`. `UNKNOWN (0)` is what a
  host-initiated USB-JTAG reset reports on the S3 — expected, not an error.
- **A persistent boot log** at `./bootlog.txt`, capped at 4 KB. This is the part
  that matters for a field failure: replugging a dead board to a host
  power-cycles it, so a live `esp_reset_reason()` would only ever say `POWERON`.
  The file keeps the history, and **is echoed to the console on every boot**, so
  reading it needs no file-transfer path — just watch the boot output.
- **A 60-second `[heap]` trend line** (free / total / %, min-free, uptime), as a
  plain `printf` so it survives a low `RNS_LOG_LEVEL`. This is what will confirm
  the leak and measure any fix.

### 11.4 Also fixed on Rev 1 in this session

- Allow list populated with the Deck's NomadNet identity
  `43a8907c03e6ecff75188b3170e8d949`, so the NomadNet subpage links work (§10.2
  covers the mechanism — it was `[]` on Rev 1 too, exactly as on Rev 2).
- Inherited the shared `Remote.h` UDP-rebind fix (§10.3) and the periodic
  NomadNet re-announce, both of which apply to Rev 1 as well.
- Pre-flash image saved to
  `~/RNode_Backups/RAD01_Rev1_80B54EF4C7A4_2026-08-21_preinstrument/`.

### 11.5 What this means for Rev 2

Rev 2 will hit the same wall — it is the same stack with the same allocator
configuration, and its heap sits at 33% free on a *fresh* boot. The LDO uprate
(AP2112K 600 mA -> AP7361C 1 A) does not help here; this was never a power
problem. Fix the PSRAM allocation before relying on either board unattended.


---

## 12. The memory fix — spilling malloc to PSRAM (2026-08-21)

Follow-on from §11. **Applied and verified on Rev 2; not yet on Rev 1.**

### 12.1 Why RNS_PSRAM_ALLOCATOR alone was not enough

Measured via the allocator metrics namespaces (57 = Default Allocator, 58 =
Container Allocator): the container allocator reported **29** live allocations and
PSRAM held ~29 KB, while internal heap carried ~180 KB. So
`-DRNS_CONTAINER_ALLOCATOR=RNS_PSRAM_ALLOCATOR` *is* working — it just only covers
a handful of STL containers. The bulk of the stack's allocations (Bytes buffers,
packets, links, map nodes, strings) go through ordinary `malloc` and stayed on
internal heap.

The framework ships `CONFIG_SPIRAM_USE_MALLOC=y` with
**`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`**, so only allocations >= 4 KB ever
reach PSRAM. Almost nothing RNS allocates is that big.

### 12.2 The fix

`heap_caps_malloc_extmem_enable(PSRAM_MALLOC_THRESHOLD)` early in `setup()`,
before any RNS allocation, lowering that threshold at runtime. Set via
`-DPSRAM_MALLOC_THRESHOLD` on the env, so it is trivially tunable and revertible.

### 12.3 Measured, on Rev 2, same firmware, threshold the only variable

| threshold | heap free | min-free | time to `RNS is READY` | `hw_ready` |
|---|---|---|---|---|
| 4096 (framework default) | 91,504 (33%) | 81,908 | 6 s .. **11.5 min** | 1 |
| **1024 (chosen)** | 112,916 (41%) | **111,856** | **4.0 s** | **1** |
| 256 | 123,008 (45%) | 122,476 | 4.4 s | **0 — radio fails** |

**256 breaks the radio.** `hw_ready: 0`, reproducibly, where 1024 gives
`hw_ready: 1` on otherwise identical firmware. This is the classic PSRAM hazard:
SPI/DMA buffers cannot live in external RAM, and at a 256-byte threshold the
driver's buffers land there. **Any change to this value must be re-verified by
checking `hw_ready` at boot** — the failure is silent otherwise; the board comes
up, joins WiFi, and serves NomadNet perfectly while being deaf and mute on LoRa.

Secondary result: time-to-ready collapsed from minutes to ~4 s, confirming the
pathological startup in §11.2 was memory pressure, not filesystem scanning. (The
11.5 min figure was one observation on a board with heavily accumulated state; a
clean baseline boot was ~6 s. The 4.0 s figure is now consistent across boots.)

Heap held flat at 41% across successive `[heap]` samples — encouraging, but the
real test is hours of uptime, which the trend line now records.

### 12.4 Not applied to Rev 1

Rev 1 is deployed on wall power collecting boot-log data, and its env was
deliberately left unchanged so that field experiment has only one variable. When
it next comes to the bench, adding `-DPSRAM_MALLOC_THRESHOLD=1024` to
`[env:impr-rad01-rev1]` should carry the same benefit — but Rev 1 is SX1276 on a
different SPI path, so **verify `hw_ready: 1` after flashing it**, per §12.3.


---

## 13. NomadNet unreachable — what is actually wrong (2026-08-21)

**Both boards are healthy.** Rev 1 (192.168.1.54) and Rev 2 (192.168.1.88) are up,
answering ICMP, announcing on schedule, with correct names and no new
`SW (ESP.restart)` entries. The failure is in the RNS **path layer between the
Deck and the boards**, not on the boards.

### 13.1 Ruled out, with evidence

| Suspicion | Verdict |
|---|---|
| Board hung / crashed | No. Both ping; boot log clean. |
| Board can't receive UDP | **No.** Added rx/tx counters: 15 probe packets sent from the Deck (unicast + subnet + limited broadcast), board counted **rx=13**. |
| Router suppressing broadcast to WiFi | No — unicast to `192.168.1.88:4242` also arrived. |
| The PSRAM change broke the network | **No.** Reverting `PSRAM_MALLOC_THRESHOLD` to the framework default (4096) left `rx=0` unchanged. An earlier note blaming it was wrong. |
| `udp.begin()` failing after the rebind | No. Instrumented the return value: `[WiFi] udp bound on 192.168.1.88:4242`, succeeds first try. |
| Allow-list / permissions | Not the blocker. Links fail before any request is issued. |

A link **has** established successfully once (`status=2 ACTIVE`, board showed
`rx=2 tx=3`), proving the whole path works when the route is good. It is not
reproducible, because the route does not stay good.

### 13.2 The actual problem: paths appear, then vanish

Observed repeatedly: an announce installs a path (`1 hop`, direct), a fetch is
attempted, and moments later `has_path` is `False` again. RNS invalidates a path
when delivery fails — and **this node runs with transport disabled, so it never
answers path requests**. The only mechanism that reinstalls the route is the next
announce. One failed link therefore costs a full announce interval of
unreachability, and the next attempt inside that window fails too, invalidating
again. It self-sustains.

Two compounding factors:

1. **The Deck hears its own broadcasts.** `~/.reticulum/config` has
   `enable_transport = Yes` with the UDP interface on
   `listen_ip = 0.0.0.0` / `forward_ip = 255.255.255.255`, **same port**. A socket
   bound to `0.0.0.0:4242` receives its own limited broadcasts, and RNS's
   `UDPInterface` has **no self-filtering** (checked the source — it never compares
   the sender against itself). So the Deck learns routes to the boards *via its own
   transport instance* at +1 hop. Both NomadNet destinations were observed at
   `2 hops via <81909ba6...>` — the Deck's own instance — while the same boards'
   other destinations were 1 hop direct. Whichever copy arrives first wins, which is
   why it is inconsistent. This cannot be fixed by config alone: the boards must
   broadcast to that same port, so the Deck cannot stop listening on it.

2. **The boot announce lost a race.** Chronologically: WiFi begins at t+2.6s, the
   startup announce fires at ~t+4.2s, and the UDP socket is only rebound to a real
   address at t+5.2s. The announce went out before the interface could carry it.
   This was previously masked by slow startup and was exposed by the §12 speedup.
   Fixed by `NOMADNET_FIRST_ANNOUNCE_MS` (60 s) rather than relying on the boot
   announce.

### 13.3 Changes made

- `NOMADNET_FIRST_ANNOUNCE_MS` = 60 s — first announce after the network is up.
- `NOMADNET_ANNOUNCE_INTERVAL_MS` 30 min -> **5 min** — shortens the unreachable
  window after a path invalidation. Cheap on WiFi/UDP; reconsider for LoRa-only.
- UDP **rx/tx counters** reported alongside `[heap]`, and `udp.begin()`'s return
  value checked and retried rather than assumed. Transmit works without a bound
  socket (`beginPacket()` needs none), so a board can announce perfectly while
  being completely deaf — these counters distinguish the two.

### 13.4 Recommended next step (not done — needs a decision)

**Enable Transport Mode on the boards** (§5.2, `rnodeconf --tnc ...`). A node that
answers path requests recovers a route in seconds instead of waiting for an
announce, which removes the failure mode in §13.2 entirely rather than narrowing
its window. This is the objective the handoff was written around anyway, and it
needs radio parameters chosen, so it is left as a decision rather than assumed.


---

## 14. RESOLVED — both NomadNet sites reachable (2026-08-21)

Both boards serve their NomadNet sites, index **and** subpages, with the browsing
identity verified. Root cause was never the boards.

### 14.1 Root cause: the AP drops broadcast traffic to WiFi stations

Measured against Rev 2 using packet counters added to the firmware:

| destination | sent | received | loss |
|---|---|---|---|
| `255.255.255.255` (limited broadcast) | 10 | 3 | **70%** |
| `192.168.1.255` (subnet broadcast) | 10 | 5 | **50%** |
| unicast to the board | 10 | **10** | **0%** |

Both ends of the UDP transport were broadcasting. A single announce survives 70%
loss if you repeat it, which is why paths looked healthy and the nodes appeared
"up" — but an RNS **link handshake is several packets in sequence**, so it
essentially never completed. That is the entire "main loads but links do not work"
and "cannot reach the site" symptom.

### 14.2 The fix — unicast in both directions

**Deck side** (`~/.reticulum/config`, backup at `config.bak-2026-08-21`): the single
broadcast `RAD-01 UDP Mesh` interface was replaced with one unicast interface per
board. Two `UDPInterface`s cannot share a listen port — `socketserver.UDPServer`
does not set `SO_REUSEADDR` — but transmit uses a fresh ephemeral socket, so only
the *listen* ports must differ. Rev 1 keeps 4242; **Rev 2 was moved to 4244** via
provisioning (ns 102 field 2).

**Board side**: `UDP_REMOTE_HOST` in `UDPInterface.h` was hardcoded to
`255.255.255.255`. It is now overridable, and `[env:impr-rad01-rev2-uart]` sets
`-DUDP_REMOTE_HOST='"192.168.1.51"'`. Deck-side unicast alone made fetches work
but intermittently; making the *board's* transmit unicast too took it to **3/3**.

Both directions now pin IPs. **Set DHCP reservations for both boards and the Deck**,
or a lease change breaks this silently.

### 14.3 Verified working

- Rev 2 `/page/index.mu` — 3/3 consecutive fetches, `Verified identity: 43a8907c...`
- Rev 2 `/page/stack.mu` with `c=heap`, `c=flash`, `c=metrics` — all served
- Rev 1 `/page/index.mu` — served, identity verified, **without reflashing Rev 1**
  (the Deck-side unicast was sufficient for it)
- Rev 1 `/page/stack.mu` subpages — served

The allow-list fix (§10.2) is confirmed end-to-end: `stack.mu` returns content
rather than a denial. Note subpages need their category as request data keyed
**`var_c`** (NomadNet's `var_` prefix), e.g. `{'var_c': 'heap'}` — without it the
page correctly answers `CATEGORY NOT FOUND`.

### 14.4 Bonus: remote health readout

`/page/stack.mu` now gives live telemetry over the mesh with no serial cable.
Read from Rev 1 while it sat on the outlet:

| | Rev 1 (no PSRAM fix) | Rev 2 (threshold 1024) |
|---|---|---|
| heap free | 68,324 (**25%**) | 107,984 (39%) |
| heap min-free | **56,016** | 104,252 |
| heap fragmentation | **22%** | 8% |
| PSRAM used | ~36 KB / 8 MB | ~52 KB / 8 MB |

Rev 1 has materially less headroom and nearly three times the fragmentation —
consistent with it being the board that OOM-restarts (§11). Its 8 MB of PSRAM is
still effectively unused.

### 14.5 Rev 1 is one flash behind

Rev 1 still lacks: the PSRAM threshold (§12), the 5-minute announce interval, the
unicast `UDP_REMOTE_HOST`, and the UDP packet counters. It has the reset-reason
logging, persistent boot log, heap trend, UDP rebind and 30-minute announce.
Applying `-DPSRAM_MALLOC_THRESHOLD=1024` and
`-DUDP_REMOTE_HOST='"192.168.1.51"'` to `[env:impr-rad01-rev1]` should carry
across — but **verify `hw_ready: 1` after flashing**, since Rev 1 is SX1276 on a
different SPI path and 256 demonstrably broke the radio on Rev 2 (§12.3).


---

## 15. LoRa bring-up — ground truth from the Meshtastic variants (2026-08-21)

**Both boards are known-good on Meshtastic in their current physical
configuration** (100+ messages exchanged). That eliminates the antennas, the RF
paths, the MM8030 removal and range as causes. Anything still broken is in *our*
firmware.

The working variants live in the Meshtastic tree at
`~/projects/firmware/variants/esp32s3/impr-rad-01{,-rev2}/variant.h`. They are the
authority for these boards; §8.2's desk-derived guesses are superseded.

### 15.1 What the Rev 2 variant settles

| Question | Answer | Our original | Status |
|---|---|---|---|
| DIO2 as RF switch | **Yes** — module has an integrated SC70-6 load switch driven from DIO2 *inside the can*; DIO2 deliberately not routed | `false` | **fixed -> true** |
| TXEN (IO21) / RXEN (IO7) | **Unused.** Routed on the PCB for the *-P* variant's external PA/LNA only; plain Ra-01SH switches via DIO2. Left undefined -> `RADIOLIB_NC` | driven | **fixed -> -1** |
| TCXO | **None.** Plain crystal; `SX126X_DIO3_TCXO_VOLTAGE` must stay undefined | `false` | already correct |
| Max power | `SX126X_MAX_POWER 22` | MODEL_FE caps 17 | noted |
| Pin map | SCK 10, MOSI 11, MISO 12, CS 13, RST 14, DIO1 6, BUSY 5 | identical | confirmed |

Rev 1's variant is `USE_RF95` with DIO0 5 / DIO1 6 / DIO2 7 and no PA overrides —
identical to our board definition. **Rev 1 needs no changes.**

Enabling `HAS_TCXO` was tried and put Rev 2 into a silent `ESP.restart()` loop
(~25 s period, before the first heap report). Reverted. Do not re-try it.

### 15.2 The firmware layer is not the problem

Instrumented `transmit()` with a call counter and dumped the CSMA state:

```
[lora] tx_calls=3 queue=0 dcd=0 rssi=-118 nf=-117 online=1
[lora] tx_calls=4 queue=0 dcd=0 rssi=-117 nf=-117 online=1
```

`tx_calls` increments per announce, the queue drains, `dcd` is clear and the radio
is online. So the modem **is** being keyed, CSMA is **not** stalling the queue, and
`packets_sent` at the RNS interface is not lying about reaching the driver. Note
that interface-level `packets_sent` counts *queueing* only — it was misleading
earlier and should not be used as evidence of RF emission.

Also checked and found correct: the SX1262 PA config in `sx126x::setTxPower()`
(PADutyCycle 0x04, HPMax 0x07, DeviceSel 0x00, ramp 40 us, level clamped -9..22).

### 15.3 Still unresolved

With the board definition now matching the known-good variant exactly, neither
board decodes anything from the other, and Rev 2's channel telemetry never rises
more than ~10 dB above its noise floor with the boards 30 cm apart. A 7 dBm
transmitter at that range should read ~-30 dBm.

**Do not trust `LoRaInterface.packets_received` alone.** Rev 1 has read exactly
`6 packets / 1130 bytes` across every configuration tried, including ones where
Rev 2 was not transmitting at all, while `last_rssi`/`last_snr` stayed at their
`-292`/`-32` sentinels. A genuine decode would populate those. Treat the pair
(`last_rssi` real AND counter increasing) as the only reliable evidence.

### 15.4 Next step

RadioLib — the driver Meshtastic uses successfully on this exact hardware — is
available for direct comparison at:

```
~/projects/firmware/.pio/libdeps/impr-rad-01-rev2/RadioLib/src/modules/SX126x/
```

Diff its SX1262 init and transmit sequence against `sx126x.cpp`. The remaining
difference has to be in there, since the board-level configuration now matches.

A cheaper discriminator first: add a **preamble-detect counter** on Rev 2
(`IRQ_PREAMBLE_DET_MASK_6X` in `sx126x::dcd()`). Preamble detection fires below
the threshold for a successful decode, so if Rev 1 is radiating at all, Rev 2
should see preambles even when nothing decodes. That splits "Rev 1 is not
transmitting" from "Rev 2 cannot demodulate" in one measurement.


## 16. LoRa link WORKING — root cause was host_disconnected() (2026-08-21)

**Both boards now transmit and receive over LoRa with no host attached to
either.** Measured standalone, serial ports closed:

```
REV2   rx 8 pkts / 1578 B   last_rssi -51 dBm  SNR +12.5 dB  (noise floor -117)
REV1   rx 25 pkts / 4625 B  last_rssi -65 dBm  SNR +11.0 dB  (noise floor -116)
```

Both LED indicators flash in sync (D2 on IO4, lit by `dcd_led` on carrier
detect) — one board transmitting lights the other's LED.

### 16.1 Root cause

`host_disconnected()` in `Utilities.h` called `stopRadio()` unconditionally.
That is correct for a host-driven modem (MODE_HOST) but wrong in MODE_TNC, where
the board is a standalone transport node. Traced on Rev 1:

```
[radio] startRadio OK at 3473ms
[radio] Sent 184 byte packet + 2x 168 byte
[WiFi] status: 3
[radio] host_disconnected() -> stopping radio at 18885ms
[radio] host_disconnected() -> ... again every ~12s, forever
```

Once WiFi comes up, `wifi_remote_check_active()` sees the WiFi-KISS session idle
past `WR_READ_TIMEOUT_MS` (6.5 s), concludes the host is gone, and kills the
radio. **Each board transmitted for ~15 s after boot and was then mute.** The
same function also wiped `current_rssi`/`last_rssi` to `-292`, which is why every
telemetry page showed a radio that looked configured, online and deaf.

Fix: guard the teardown with `if (op_mode != MODE_TNC)`.

This also explains why the two boards appeared to behave differently for most of
the session: Rev 2's radio kept running only because a script was holding its
serial port open, so it always counted as "host connected".

### 16.2 What the diagnosis actually required

The decisive instruments, in order of usefulness:

1. **`tx_calls`** — a counter inside `transmit()`. Interface-level `packets_sent`
   counts *queueing* and was actively misleading all session.
2. **Preamble / header counters** (`pre`/`hdr` on SX126x, `sig`/`syn` on SX127x).
   Preamble detection fires below decode threshold, so it separates "nothing is
   being radiated" from "heard but not demodulated". The unplug-Rev-1 control
   test proved the early preambles were ambient neighbourhood LoRa, not Rev 1.
3. **Radio lifecycle tracing** in `startRadio()` / `stopRadio()` /
   `host_disconnected()` — this is what actually found it.

### 16.3 Corrections applied along the way

| Change | Verdict |
|---|---|
| `DIO2_AS_RF_SWITCH` false -> **true** | correct (confirmed by Meshtastic variant) |
| `pin_rxen`/`pin_txen` -> **-1** | correct (module switches via DIO2 internally) |
| `HAS_TCXO` -> true | **WRONG** — caused a restart loop; reverted, module has a crystal |
| `SetRegulatorMode(DC-DC)` added | real gap vs RadioLib; kept |
| `enableTCXO()` before `calibrate()` | datasheet-correct ordering; kept (inert here) |

Not the cause, despite being investigated: antennas/RF paths (Meshtastic proved
the hardware), CSMA/`dcd`, PA config, frequency word, sync word, IQ errata,
preamble length, IRQ setup.

### 16.4 Hardware: J3.8 grounded

`MB_RTS` (J3 pin 8) is now tied to GND (J3 pin 2), holding Q1's gate low so EN is
never pulled down by an idle bridge. Rev 2 verified surviving with
`/dev/ttyUSB0` closed. Cost: automated flashing — use S3(BOOT)+S1(RESET) with
`--before no_reset --after no_reset`, or lift the jumper. Rev 3 should carry 10k
gate pull-downs on Q1/Q2 instead (section 8.8).

### 16.5 Still open

- Something connects to Rev 1's WiFi-KISS TCP listener and goes idle every ~12 s,
  which is what kept triggering `host_disconnected()`. Harmless now, unidentified.
- Transport Mode end-to-end (WiFi off on one board, route over LoRa only) is not
  yet tested — that is now possible for the first time.


## 17. Session close — state, and the J3 wiring problem (2026-08-21 late)

### 17.1 Where each board stands

| | Rev 1 (`/dev/ttyACM1`) | Rev 2 (CP2102N on `/dev/ttyUSB0`) |
|---|---|---|
| Firmware | **current** — sections 16 fix + items 3/4/6/7 | section 16 fix only |
| State at close | up, on mesh, LoRa working | **dark** — needs a physical S1 tap |
| Serial | reliable (native USB-JTAG) | **unreliable** — see below |

Rev 2 is missing: no-reboot-on-TX-failure, op_mode over the air, and the two boot
diagnostics. None are needed until the Transport Mode / Columba test.

### 17.2 The J3 header is the blocker

Six distinct failures in one evening, all on the same dupont-to-2.54mm-header
connection, each appearing after physical handling:

| symptom | what it looked like |
|---|---|
| deaf RX | board transmits, ignores every command |
| ground jumper off | board dark; revived by asserting RTS |
| brownout loop | 20 boots/60s, all `rst:0x1 (POWERON)` |
| stuck reset | silent, no bootloader SYNC response |
| deaf RX again | boots and runs, answers nothing |
| dark | no output, no ping |

These are not six faults; it is one marginal connection presenting differently as
the contact degrades. **Fix mechanically before any further Rev 2 work:** solder
the J3 wires or fit a latching connector. Dupont contacts on a bare header cannot
survive the handling that flashing requires.

### 17.3 The flashing catch-22

Grounding J3.8 gave Rev 2 autonomy (survives a closed port) but removed RTS from
the reset path, so **esptool can no longer reset it** — entering the bootloader
now requires holding S3 and tapping S1 by hand. Pressing those buttons disturbs
the same wires the upload needs. Until J3 is mechanically sound, Rev 2 is
effectively unflashable.

Note also: once Rev 2 is in reset, software cannot recover it. Only S1 or a power
cycle will.

### 17.4 Outstanding list

Done, committed:  board support, UDP transport fixes, PSRAM headroom, periodic
announce, instrumentation, and the section 16 `host_disconnected` fix.

Done, built, **flashed to Rev 1 only** (uncommitted at close):
- TX failure recovers instead of rebooting (`REBOOT_ON_TX_FAILURE 0`)
- `op_mode` settable via provisioning, defaulting to MODE_TNC
- Boot log distinguishes power loss from reset, records previous run length
- Instrumentation trimmed; `[kiss-tcp]` peer logging kept

Blocked on Rev 2 serial:
- **Item 2** — TX power will not persist. Expected to work now: `eeprom_conf_save()`
  needs `radio_online`, which the section 16 fix restores. Unverified.
- **Item 5** — retest `PSRAM_MALLOC_THRESHOLD=512` for headroom; **verify
  `hw_ready: 1` after**, since 256 killed the radio under the old board config.

Not actionable here:
- The OOM restart paths live in `microReticulum` (`Interface.cpp:56/79`,
  `Reticulum.cpp:258/280`), and `lib_deps` still points at **attermann's** repo in
  four places. Changing "restart" to "degrade" needs a fork of the *library*; only
  the firmware was forked. Item 5 is monitored, not fixed — and note no OOM
  message was ever captured this session, so the silent `SW` restarts were most
  likely the TX-failure `hard_reset()` that is now fixed.

### 17.5 Next session, in order

1. Re-solder / re-connector J3 on Rev 2.
2. Flash Rev 2 to match Rev 1; verify `hw_ready: 1` and `op_mode: 18`.
3. Item 2, then item 5's threshold retest.
4. Then Transport Mode end-to-end + Columba (section 5.4 / the plan in the chat log):
   WiFi off on one board so LoRa is its only path, and **verify per-interface
   counters** — a proof is only valid if no alternative route exists.
