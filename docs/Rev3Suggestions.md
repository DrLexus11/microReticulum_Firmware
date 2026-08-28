# IMPR-RAD-01 Rev3 Baseboard Suggestions

Hardware recommendations for a future IMPR-RAD-01 Rev3, with emphasis on an
autonomous RNode/Reticulum node that remains recoverable during extended
infrastructure and power failures.

Status: **design proposal**, written 2026-08-25. These are not Rev2 assembly
changes. Related optional carrier and mezzanine designs are in
[`docs/ModuleSuggestions.md`](ModuleSuggestions.md).

## 1. Product boundary

Rev3 should contain the capabilities that improve the reliability of every
node. Features needed only by a propagation gateway, logger, or specialised
power installation should remain on a mezzanine.

| Capability | Rev3 base | Optional module |
| --- | --- | --- |
| RTC and backup domain | **yes** | no |
| Voltage supervision and power-fail warning | **yes** | carrier may add input-side protection |
| Recoverable firmware and local filesystem | **yes** | no |
| Small high-endurance metadata journal | footprint recommended | no |
| Large store-and-forward archive | no | **storage module** |
| Removable log media | no | **storage module** |
| Wide-input, battery, solar, or PoE power | interface support | **power/network module** |
| GNSS and PPS | interface support | **GNSS module** |

The resulting baseboard remains compact and useful by itself. A heavy
propagation node can add storage and power features without forcing their cost,
mechanical risks, and idle consumption onto every resident node.

## 2. Rev2 facts that affect Rev3

Use the released production design as the source of truth when starting Rev3,
not the older conceptual handoffs.

- The released Rev2 BOM fits an `ESP32-S3-WROOM-1-N8R8` (`C2913201`): 8 MiB
  flash and 8 MiB octal PSRAM. Octal PSRAM consumes GPIO35, GPIO36 and GPIO37.
  The Rev2 raw BOM already identifies `N16R2` as the intended production
  alternative that frees those pins.
- Rev2 has three 20-contact Hirose DF12 connectors: J5 for the top module and
  J1/J6 for the lower carrier.
- The released Rev2 production BOM does **not** contain the RTC proposed in
  `REV2_HANDOFF.md`.
- On the released J5 interface, `RSVD_DISP_INT` is not proven connected to an
  MCU GPIO and `RSVD_PWR_EN` is explicitly unconnected. The current display
  can be polled, but Rev3 should complete these signals.
- The current propagation-node contract is 128 messages with a 512 KiB
  defence-in-depth byte cap. The firmware has no persistent wall clock, so
  time-based expiry is still blocked; see `docs/PropagationNodeTODO.md`.

Relevant hardware sources in the KiCad workspace:

- `lab6_mcu_lora/rev2/IMPR-RAD-01/production/bom.csv`
- `lab6_mcu_lora/rev2/IMPR-RAD-01/IMPR_RAD_01_raw_bom.md`
- `lab6_mcu_lora/rev2/IMPR-RAD-01/production/netlist.ipc`
- `lab6_mcu_lora/modules/IMPR-DISP-01/production/bom.csv`
- `lab6_mcu_lora/modules/IMPR-DISP-01/IMPR-DISP-01_HANDOFF.md`

## 3. Recommended Rev3 priorities

### P0 — make power loss safe

Adding storage without controlling brownouts increases the amount of data that
can be corrupted. Rev3 should provide an independent voltage supervisor with
two useful events:

1. an early `POWER_FAIL` warning while the MCU and storage are still within
   their valid voltage ranges; and
2. a lower reset threshold that holds the ESP32 in reset once operation is no
   longer safe.

The firmware response to `POWER_FAIL` must be bounded: reject new persistent
writes, finish or abandon the current transaction, commit only the small queue
journal, and enter a safe state. Hold-up capacitance must then be calculated
from measured worst-case node current, the usable voltage interval, component
tolerances and the measured shutdown time. Do not select capacitance from a
nominal average current.

The AP7361C regulator used by Rev2 has no system-level `POWER_GOOD` contract.
An example of the required supervisor class is the LTC2935, which provides
separate early-warning and reset thresholds at very low quiescent current:

- [Analog Devices LTC2935 product page](https://www.analog.com/en/products/ltc2935.html)
- [LTC2935 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/2935fa.pdf)

Part selection must be revisited against the final input rail, regulator,
reset threshold and required warning interval. Consider an independent
watchdog as a separate decision; it should recover a wedged application rather
than duplicate an ESP32 software watchdog without a stated failure case.

### P1 — use N16R2 and design for recovery

Prefer `ESP32-S3-WROOM-1-N16R2` unless runtime profiling demonstrates that the
firmware requires more than 2 MiB PSRAM. Compared with the fitted N8R8, this
choice:

- increases flash from 8 MiB to 16 MiB;
- retains 2 MiB of quad PSRAM; and
- frees GPIO35–GPIO37 from the octal PSRAM bus.

The extra flash has direct product value: it can support a factory/rescue image,
two OTA application slots and a materially larger LittleFS store. Exact
partition sizes must be generated from release build sizes and future growth
allowance, not copied from an illustrative table. The current application is
approximately 1.8 MiB, so a candidate layout should investigate roughly 3 MiB
per application slot while preserving a rescue image and the remaining space
for data.

Requirements:

- retain a factory/rescue boot path that does not depend on the active image;
- use two OTA slots and enable rollback after failed first boot;
- retain an operator-accessible boot/recovery mechanism;
- use LittleFS or another power-loss-tolerant filesystem with wear levelling;
- keep firmware, queue metadata and message-body failures separable where
  practical; and
- test rollback after interrupted download, interrupted activation and boot
  failure.

References:

- [ESP32-S3-WROOM-1/1U datasheet](https://documentation.espressif.com/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)
- [ESP-IDF partition tables](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/partition-tables.html)
- [ESP-IDF OTA and rollback](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32s3/api-reference/system/ota.html)
- [ESP-IDF filesystem considerations](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/file-system-considerations.html)

### P1 — fit a battery-backed RTC

Fit an RV-3028-C7 or an equivalent qualified low-power RTC on the baseboard.
Absolute time is useful on every autonomous node for:

- message expiry and store maintenance;
- chronological incident and reboot logs;
- scheduled announces and maintenance;
- measuring complete-power-loss intervals; and
- recovering time when WiFi, NTP, GNSS and the attached phone are absent.

The RV-3028-C7 integrates its crystal and provides automatic backup switchover,
alarms, a Unix-time counter and event timestamping while drawing tens of
nanoamps in timekeeping mode. Route its interrupt/event output to the MCU; an
RTC that can only be polled loses much of its value.

A 32.768 kHz crystal attached only to the ESP32 is not a replacement: it can
improve sleep timing but cannot retain absolute time through total board power
loss.

Backup-source policy must be explicit:

- a primary coin cell gives the longest unpowered retention, but RTC trickle
  charging must be disabled;
- an MS621/ML-series rechargeable cell avoids a primary coin cell but requires
  a correct charging design;
- a supercapacitor is acceptable only after worst-case retention is calculated
  with RTC, capacitor, PCB and protection leakage over temperature; and
- `RTC_VBACKUP` may also be exposed to the lower power carrier, but the base
  should retain at least a defined short-holdover option.

Reference: [Micro Crystal RV-3028-C7 datasheet](https://www.microcrystal.com/fileadmin/Media/Products/RTC/Datasheet/RV-3028-C7.pdf).

### P1 — finish the mezzanine contract

Rev3 should turn the existing connectors into a documented product interface.
At minimum, provide:

- switched and current-budgeted module power;
- one interrupt/wake input per logical slot;
- module-present or module-identity support;
- deterministic I2C address straps or slot IDs;
- I2C and a dedicated expansion/storage bus;
- a power-fail indication that storage modules can observe;
- defined reset-time levels and pull resistors; and
- explicit statements that the internal DF12 connectors are not hot-plug
  interfaces unless a later design deliberately qualifies them as such.

Connect J5's intended display interrupt to an MCU GPIO and use the intended
power-enable signal to control a load switch rather than simply exposing an MCU
pin as a supply. The TCA9534 interrupt on the display module can then eliminate
button polling in a future module revision.

For the lower connector pair, expose dedicated SDMMC or SPI storage signals.
Do not make a production storage design depend on repurposing JTAG or display
signals without an explicit mux, boot-state analysis and test procedure. Keep
JTAG available on test pads if connector pin pressure requires it.

### P1 — carry node identity in an I2C EEPROM

**Decided.** The part is being added; this section records the role it plays so
it is not later mistaken for a capacity upgrade (see §4).

**Why.** Both identities are files on LittleFS -- `transport_identity`
(`Transport.cpp`) and `local_identity` (`RNode_Firmware.ino`) -- and what the
firmware currently calls EEPROM is Arduino's flash-emulated NVS
(`EEPROM_SIZE 1024`, `HAS_EEPROM true` in `Boards.h`). Neither survives a
full-chip erase. [`docs/FlashHeadroom.md`](FlashHeadroom.md) §"The trap" records
how close the Rev2 repartition came to destroying a node's identity for exactly
this reason. A separate I2C part is a different durability class: it survives
erase, reflash and repartition because it is not the flash being erased.

**Durable set.** Only what must outlive a full erase: the two identities, the
RNode config block and its signature byte, the firmware hash, WiFi/SoftAP
credentials, and the RRC hub identity. Nothing message-sized.

**Implementation.**

- An `EepromStore` with a versioned header (magic, version, length, CRC) so the
  layout can change without bricking provisioned units. Back it with the I2C
  part when present and fall back to the emulated EEPROM when absent, so Rev1
  and Rev2 keep building from the same source.
- Route identity loading through the store. `RNode_Firmware.ino` currently calls
  `Identity::from_file` on a LittleFS path directly.
- Migrate on boot: if the part carries no valid magic, read the existing
  LittleFS/emulated copy, write through, and keep the old path as a read
  fallback for one release. Never auto-erase the old copy.
- Provisioning gets a read-only status op -- identity hash, store version, CRC
  state, never the private key -- and an explicit factory-clear.
- Page-aligned writes, bounded retries on NAK, and CRC verification after write.
  Identity is written once, so endurance is a config-churn question, not an
  identity one. If churn turns out to matter, that is the FRAM case below.
- Address allocation shares the bus with the display ID EEPROM and the future
  RTC, so strap A0-A2 clear of both.

### P2 — reserve a small FRAM footprint

An I2C FRAM in the 32 KiB class is useful for high-write-rate, small records:

- queue index and commit journal;
- last clean shutdown and reset cause;
- last known valid time;
- boot/crash/event ring; and
- storage-health counters.

It is **not** bulk message storage. A representative FM24V02A provides only
32 KiB, but supports effectively immediate writes and much higher endurance
than EEPROM:

- [Infineon FM24V02A datasheet](https://www.infineon.com/assets/row/public/documents/10/49/infineon-fm24v02a-256-kbit-32k-8-serial-i2c-f-ram-datasheet-en.pdf?fileId=8ac78c8c7d0d8da4017d0ec9592741c0)

The Rev3 PCB may carry this as a DNP option until firmware demonstrates that
the internal-flash journal is insufficient. Its I2C address must be included
in the system-wide address allocation.

## 4. Deliberately excluded from the Rev3 base

### No external EEPROM for LXMF blobs

Serial EEPROM is appropriate for rarely changed module identity, calibration,
or configuration. It is too small and too slow for a propagation store and has
far lower write endurance than FRAM. A representative 24CM02 holds only
256 KiB and allows up to 10 ms for an internal page write:

- [Microchip AT24CM02 datasheet](https://ww1.microchip.com/downloads/en/DeviceDoc/AT24CM02-Data-Sheet-20006197A.pdf)

Do not describe an EEPROM addition as a store-and-forward capacity upgrade.
The part scheduled in §3 is for identity and configuration durability; this
exclusion is about message blobs and still holds.

### No microSD socket on every baseboard

Large removable storage is valuable for a gateway or blackbox, but the socket,
contacts, removable card, ingress opening, idle current and brownout behaviour
are liabilities for small resident nodes. Put the complete storage subsystem
on a replaceable lower mezzanine and qualify it as a product configuration.

## 5. Additional disaster-node value

The following are primarily interface or manufacturing requirements rather
than more baseboard peripherals:

- preserve native USB recovery and accessible `BOOT`/`EN` test points;
- make secure boot, signed update, flash encryption and identity provisioning a
  deliberate manufacturing profile, with a documented recovery policy;
- expose enough signals for a wide-input/solar/battery carrier and a fuel
  gauge;
- support an Ethernet/PoE carrier for fixed shelters and rooftop relays;
- expose a clean interrupt/PPS input for GNSS time recovery;
- provide ESD/surge protection at interfaces that leave the enclosure; and
- record board/module revision and fitted options in a machine-readable way.

## 6. Rev3 acceptance gates

Do not release the revision solely because its normal boot works. Acceptance
should cover these failure cases:

1. Remove power during a message-body write, queue-index update and log append;
   after reboot the filesystem mounts and every visible record is valid.
2. Remove power during OTA download and during first boot of the new image;
   rescue/rollback remains reachable.
3. Run from the minimum valid supply while LoRa transmits, WiFi transmits and
   the display/buzzer module is active; there are no false resets or silent
   storage corruption.
4. Remove main power long enough to exercise the RTC backup source, then verify
   monotonic and absolute-time recovery.
5. Exercise every supported mezzanine combination at its declared power budget
   and check I2C addresses, boot straps and interrupt routing.
6. Profile actual internal RAM and PSRAM use under propagation-store, TCP-client
   and resource-transfer stress before locking N16R2 into production.
7. Verify native USB, UART recovery, factory boot, OTA rollback and the Rev2
   `fixhash`-equivalent manufacturing flow from a blank unit.
8. Erase the entire flash, reflash from blank, and confirm the node comes back
   with the same Reticulum identity. This is the whole reason the EEPROM is
   fitted; a revision that fails it has not gained anything from the part.
