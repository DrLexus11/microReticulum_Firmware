# IMPR-RAD Mezzanine and Module Suggestions

Proposed extension-board roadmap for IMPR-RAD-01 Rev3 and later. The objective
is to keep a reliable autonomous RNode on the baseboard while allowing storage,
power and backhaul roles to be assembled as replaceable product variants.

Status: **design proposal**, written 2026-08-25. Baseboard changes and common
requirements are in [`docs/Rev3Suggestions.md`](Rev3Suggestions.md).

## 1. Existing module baseline

Rev2 exposes one top J5 connector and a paired J1/J6 lower interface. The
current IMPR-DISP-01 production BOM contains:

- a 0.96-inch 128x64 SSD1306 OLED;
- a TCA9534 I/O expander;
- four buttons;
- a VEML7700 ambient-light sensor;
- a MOSFET-driven buzzer; and
- three LEDs.

This differs from the older display handoff, which describes a simpler OLED
and 24C02 identity EEPROM concept. The released production BOM is the source of
truth: it does not contain that EEPROM. The fabricated display should not be
changed now; its buttons and sensors can be polled.

The host-side J5 interrupt and power-enable intentions are incomplete on Rev2.
Rev3 should connect them, and a future IMPR-DISP revision can then use the
TCA9534 interrupt and true host-controlled power gating.

## 2. Common module contract

Every future module should conform to one connector specification rather than
inventing its electrical contract independently.

### Identity and discovery

Use one of these ordered approaches:

1. a small module descriptor EEPROM with address pins strapped by slot;
2. a slot-specific ID resistor measured by the host; or
3. a dedicated address translator/multiplexer if the product eventually needs
   multiple identical modules.

Do not place an unstrapped EEPROM at `0x50` on every module. Top and lower
modules would collide. The descriptor should include at least:

- manufacturer and product ID;
- hardware revision;
- descriptor format version;
- required rail and maximum current;
- bus type and I2C addresses;
- driver/feature bitmap; and
- optional calibration-data location and checksum.

The descriptor is configuration data, not general-purpose storage.

### Power

- Provide a switched module rail controlled by a host-side load switch.
- Define continuous, peak and inrush current for each slot.
- Define whether a module may back-power the baseboard; normally it must not.
- Give storage modules a power-fail warning and enough local energy to reach a
  safe state.
- Default enable/control signals to the safe state during reset and bootloader
  execution.
- Treat DF12 connections as power-off assembly interfaces unless hot-plug
  protection is explicitly designed and validated.

### Buses and interrupts

- Retain I2C for low-rate sensors, identity and control.
- Give the lower pair a dedicated storage/expansion SPI bus or SDMMC bus.
- Avoid sharing storage with the SX1262 radio bus when pins permit.
- Provide an interrupt/wake signal per logical slot.
- Define voltage, pull-ups, maximum bus capacitance and cable/trace assumptions.
- Preserve production debug through test pads if JTAG pins are reassigned.

I2C pull-ups belong in one documented location. Modules should not all fit
strong pull-ups by default; use DNP options when a module must also operate
standalone.

## 3. Suggested module roadmap

### P1 — IMPR-STOR-01 storage and logging module

Purpose: turn a selected RAD into a heavy propagation node, blackbox or field
logger without weakening every base unit.

Candidate variants:

| Storage | Best use | Main trade-off |
| --- | --- | --- |
| Industrial/high-endurance microSD | removable field logs and large archives | socket/card/ingress and removal risk |
| Soldered industrial eMMC | rugged sealed gateway | not field-removable |
| Industrial SPI NOR | simple bounded queue/archive | lower capacity and explicit wear management |

The ESP32-S3 SDMMC host supports SD and eMMC devices and 1-, 4- and 8-bit bus
modes. Prefer 4-bit SDMMC when the new lower connector has sufficient pins;
otherwise use a dedicated SDSPI bus rather than sharing the LoRa bus.

References:

- [ESP-IDF SDMMC host documentation](https://docs.espressif.com/projects/esp-idf/en/v5.1/esp32s3/api-reference/peripherals/sdmmc_host.html)
- [ESP-IDF SDSPI host documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/sdspi_host.html)
- [Espressif shared SPI/SD restrictions](https://docs.espressif.com/projects/esp-idf/en/v5.0.2/esp32s3/api-reference/peripherals/sdspi_share.html)

Minimum storage-module hardware:

- card detect for removable media;
- ESD protection at an externally accessible socket;
- local bulk capacitance and high-frequency decoupling;
- host-controlled load switch so failed media can be power-cycled;
- power-fail input from the base/power board;
- optional local FRAM for the queue journal;
- write-protect input where the chosen medium supports it; and
- exposed test points for power and bus signals.

Minimum firmware contract:

- mount failure must not prevent the base node and radio from booting;
- writes must use temporary/commit or append/journal semantics;
- bound log size and define rotation;
- stop accepting writes immediately on power-fail warning;
- flush and unmount before controlled module power-off or card removal;
- report media health, capacity and last error to the management UI;
- survive absent, blank, read-only, full, corrupt and unexpectedly removed
  media; and
- preserve LXMF confidentiality expectations when exporting logs. Message
  bodies may be encrypted, but metadata and operational logs can still be
  sensitive.

Do not use a small serial EEPROM as the blob store. If a local FRAM is fitted,
reserve it for the transactional index and recovery state.

### P1 — IMPR-PWR-01 resilient power carrier

Purpose: make rooftop, shelter and unattended nodes tolerant of the power
sources actually encountered during an outage.

Candidate features:

- protected wide-range DC input;
- reverse-polarity, surge, over-current and thermal protection;
- battery charging appropriate to the selected chemistry;
- load sharing/power-path control rather than uncontrolled source ORing;
- fuel gauge and battery-temperature input;
- solar-input option where justified;
- sufficient hold-up energy for base and storage shutdown;
- RTC backup supply output; and
- latched power-source and low-battery events for later diagnosis.

High-energy input protection, large inductors and batteries belong on the lower
carrier because they depend on the enclosure and deployment. The Rev3 base
still needs its own 3.3 V supervisor and reset contract; the carrier cannot
guarantee that every possible source collapses cleanly.

### P1 — IMPR-NET-01 Ethernet/PoE carrier

Purpose: provide a deterministic wired backhaul for fixed shelters, operations
rooms and rooftop blackboxes while leaving WiFi available for local clients.

Candidate features:

- Ethernet controller or PHY compatible with the available ESP32 interface;
- galvanics and protected external connector;
- optional standards-compliant PoE input;
- hardware link/activity indication available to firmware;
- watchdog/reset or switched power for the network controller; and
- clear isolation, creepage and enclosure requirements if PoE is fitted.

Ethernet/PoE can add more disaster value than local SD alone: it supplies a
stable backhaul and can simplify power delivery to a fixed relay. It should
remain optional because magnetics, connector size and PoE thermals are poor
fits for every resident unit.

### P2 — IMPR-GNSS-01 time and location module

Purpose: recover trusted UTC and location without internet infrastructure.

Provide:

- UART for receiver control/data;
- PPS routed to a timestamp-capable GPIO;
- backup supply or warm-start domain if required;
- antenna bias/protection appropriate to passive or active antennas; and
- host-controlled power for duty-cycled operation.

GNSS complements rather than replaces the base RTC: GNSS establishes accurate
time when sky view exists; the RTC carries it through indoor operation and
complete loss of main power.

### P2 — future radio module

A second radio or alternate-band modem may be valuable for gateways, but its
interface should be defined only after deciding whether the host is expected to
run two Reticulum interfaces concurrently. Requirements would include:

- dedicated chip select, busy, reset and interrupt signals;
- an RF-safe mechanical and antenna layout;
- explicit simultaneous-transmit power budget;
- independent power gating; and
- coexistence testing rather than assuming two nearby radios are isolated.

Do not reserve large numbers of baseboard pins for an undefined radio at the
expense of the known storage, interrupt and recovery requirements.

## 4. IMPR-DISP-01 next-revision suggestions

The display module currently at fabrication should be brought up as designed.
For its next revision, after Rev3 completes the host signals, consider:

- connect TCA9534 `INT` to the module interrupt pin;
- add or standardise a slot-addressed module descriptor;
- use the host-controlled switched rail and state the peak buzzer/OLED current;
- leave I2C pull-ups configurable so the populated system has one correct
  effective value;
- document boot behaviour when the module is absent or I2C is held low; and
- retain the existing display, buttons, ambient sensor and buzzer unless
  bring-up identifies a concrete usability or power problem.

The unused display SPI positions can remain expansion space, but should not be
assigned to a feature unless the base connector contract guarantees that bus.

## 5. Module priority by deployment role

| Deployment | Base only | Recommended additions |
| --- | --- | --- |
| Resident/apartment node | Rev3 + radio | IMPR-DISP-01 as needed |
| Portable field node | Rev3 + radio | display + power carrier |
| Rooftop relay | Rev3 + radio | power carrier; Ethernet/PoE where available |
| Propagation gateway | Rev3 + radio | storage + power; Ethernet/PoE strongly preferred |
| Incident blackbox | Rev3 + radio | soldered storage + resilient power + RTC backup |
| Mobile mapping/time node | Rev3 + radio | GNSS + display + battery carrier |

## 6. Module acceptance gates

Every module should pass the following before being declared compatible:

1. Base boots and operates with the module absent, present and electrically
   failed where the design can safely simulate that failure.
2. Module identity, address and revision are detected without bus collisions.
3. Peak and inrush current remain inside the declared slot budget.
4. Removing main power during the module's worst write or activity does not
   corrupt the base filesystem or prevent the next boot.
5. A wedged module can be isolated or power-cycled without resetting the radio
   node, where the interface claims that capability.
6. All supported module combinations are tested together, including interrupts,
   I2C loading, boot straps, RF transmit and maximum power demand.
7. Unsupported or newer module revisions fail visibly and safely rather than
   being partially initialised as the wrong hardware.

