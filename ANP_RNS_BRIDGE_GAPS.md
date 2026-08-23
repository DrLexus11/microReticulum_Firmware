# RNode Firmware Gaps Affecting the ANP ↔ RNS Bridge on IMPR-RAD

**Author:** Claude Code agent (RNode firmware / RAD-01 bring-up session, 2026-08-20).
**Not** the agent that manages the ANP Jira board — this document was written by a
different agent instance and is offered as input for ticket creation, not as a
record of work already tracked.

**Status:** findings from a full bring-up of IMPR-RAD-01 Rev 1 on RNode Firmware
1.86 (upstream `markqvist/RNode_Firmware` @ `d39339f`, verified as repository HEAD).

---

## Why this matters to ANP

`DEC-006` / `ANP-20` selects the official Python RNS bridge as the **M1 proof
path**, with production integration decided at M1 exit. `profiles/m1-vertical-slice.md`
explicitly excludes *"Native LoRa, Wi-Fi HaLow device integration, and ESP
firmware"* from M1.

That exclusion is what makes these findings worth filing now rather than later.
Every gap below sits precisely in the band M1 does not exercise, and all of them
bind at **M2/M3**, when the bridge stops being a supervised Python process on a
workstation and starts running against real IMPR-RAD hardware. The
hardware-baseline note that *"the firmware layer binds before `ANP-41` fixes the
M3 runtime architecture"* applies directly: several items here are firmware
architecture decisions that will be made by accident if they are not made
deliberately.

None of these are Reticulum protocol defects. RNS 1.4.2 itself behaved correctly
throughout — transport node routing, interface management, announces and LoRa
TX/RX all worked. **The gaps are in RNode firmware's optional host transports.**

---

## Evidence base

Verified on hardware this session, not inferred:

- SX1276 detected on custom SPI pins; provisioned `f0:fe:45` hwrev 1; signature validated.
- `RNodeInterface` over USB reached **Up**, 3.12 kbps, −114 dBm noise floor.
- Live transmission confirmed: 3 announces, 549 B, airtime accounted.
- BLE advertised and was discoverable as `RNode 1114` (`80:B5:4E:F4:C7:A5`).
- Wi-Fi STA associated, held DHCP `192.168.1.54`, TCP/7633 accepted connections.
- KISS-over-TCP **never completed a detect handshake** in any of three firmware builds.

---

## G1 — Wi-Fi host transport is an unmaintained two-day prototype

`Remote.h` was introduced 2025-11-17 across four commits, patched once on
2025-11-18 (`1e05409`), and **has not been touched since**. A search of upstream
issues for `7633`, TCP disconnects and ESP32-S3 Wi-Fi returns **zero results,
open or closed**.

Nine months, one contributor, no bug reports. The most probable reading is that
nobody is using this path in production, not that it is finished.

**ANP impact:** if any ANP bearer or HIL harness assumes IP reach to a RAD board
over its own Wi-Fi, that assumption currently has no working implementation
behind it. Either the defects below get fixed (and maintained, by us), or the
architecture must route IP to the carrier and reach the radio over USB.

---

## G2 — Host input is dispatched by connection state, not data availability

`RNode_Firmware.ino::buffer_serial()` selects its input source by *which
transport is connected*, then unconditionally reads from it:

```c
if      (bt_state == BT_STATE_CONNECTED) { fifo_push(SerialBT.read()); }
else if (wifi_host_is_connected())       { fifo_push(wifi_remote_read()); }
else                                     { fifo_push(Serial.read()); }
```

With a Wi-Fi client attached, every read is taken from Wi-Fi even when the bytes
are pending on USB. `Remote.h::wifi_remote_read()` compensates by **destroying
the session** when nothing is buffered:

```c
if (connection) { wifi_remote_close_all(); }
return 0xC0;
```

This is the measured ~2 s teardown. It is not a timeout — the firmware's own
activity timeout is 6500 ms — it is the firmware tearing down its own session.
The commit that introduced it (`1e05409`) is titled *"improved host reconnection
reliability"*, i.e. it is a workaround for this same defect rather than a fix.

**Correct fix:** dispatch on data availability (`SerialBT.available()`,
a non-accepting `wifi_remote_data_available()`, `Serial.available()`), after
which the destructive close becomes unnecessary and can be removed.

**Do not remove the close without fixing the dispatch first.** Verified this
session: doing so starves the USB path instead, because a stale session then
captures output permanently with no recovery route.

---

## G3 — Wi-Fi TX writes one byte per TCP segment

```c
void wifi_remote_write(uint8_t byte) { if (connection) { connection.write(byte); } }
```

`serial_write()` is called per byte by the KISS framer, so every byte becomes its
own `send()`. Observed directly: a detect reply arrived as a lone `0xC0` in one
segment. Beyond throughput, this splits KISS frames across segment boundaries.

**Fix:** buffer into a frame buffer, flush on closing `FEND` or when full.

---

## G4 — Single client, no discovery participation

`WiFiServer remote_listener(7633, 1)` accepts **one** client. The endpoint is a
raw KISS socket; it does not participate in AutoInterface or Reticulum interface
discovery. Clients must use a reserved address and port.

**ANP impact:** any topology assuming more than one consumer of a single radio —
for example a lab harness plus a live bearer — is not supportable as written.

---

## G5 — Host transports are mutually exclusive (architecture, not bug)

`Utilities.h::serial_write()` routes to exactly one sink in strict priority:

```
BLE connected  →  Wi-Fi host connected  →  USB serial
```

One host owns the radio at a time, and a connecting Wi-Fi client actively steals
I/O from USB.

**ANP impact — this is the one most likely to blindside a HIL plan.** You cannot
run ANP over Wi-Fi while keeping USB attached for instrumentation. Measurement
gates 6–8 in the hardware baseline should state which transport owns the radio
for each measurement, or the evidence will be ambiguous about what was connected.

---

## G6 — Wi-Fi + BLE will not boot without PSRAM, and fails silently

Building with both `HAS_WIFI` and `HAS_BLE` and `PSRAM=disabled` produces a
firmware that **completes `setup()` but never services host commands** — silent
on USB, no BLE advertising, no Wi-Fi. No error is emitted anywhere.

Building the same source with `PSRAM=opi` works. Confirmed by bisection: BLE-only
and Wi-Fi-only each boot fine without PSRAM; only the combination requires it.
Chip readout confirms `Embedded PSRAM 8MB (AP_3v3)`.

This directly reinforces the existing baseline risk *"N8R8 versus N16R2
population"*: **the fitted module variant is now a functional dependency of the
firmware feature set, not merely a memory-budget note.** An N16R2 board (2 MB
PSRAM, quad) will need this re-validated before it can run a Wi-Fi+BLE image.

---

## G7 — Every firmware rebuild requires manual re-authorization

RNode validates the running app partition hash against a value in EEPROM. Any
rebuild changes the hash, `device_init()` then returns false and `hw_ready`
stays 0 until `rnodeconf -H <hash>` is run against the device.

**ANP impact:** any CI or automated flashing flow for RAD boards must compute the
partition hash (`partition_hashes`, which is SHA-256 over the image minus its
appended 32-byte digest) and re-authorize as part of the flash step, or every
automated build will produce a device that enumerates but refuses to operate.

---

## G8 — No usable console on RAD-01 (blocks all of the above)

ESP-IDF panic output and logs go to **UART0 (GPIO43/44)**, not to the USB CDC
that carries the KISS protocol. Rev 1 exposes UART0 only on the expansion header.

This session, three separate firmware defects had to be chased by bisection and
hand-rolled instrumentation because no panic or log output was reachable. That is
the single largest multiplier on debugging cost for this hardware.

**Recommendation:** make a UART0 console a standing requirement for firmware work
on RAD boards — IMPR-POW-C-02 already carries a CP2102N and exposes `CP_TX`,
`CP_RX`, `RTS`, `DTR` on its J3. Wiring it is cheap and unblocks G1–G3.

---

## G9 — ESP32-S3 BLE is the thin path; nRF52 is the maintained one

Relevant to the proposed IMPR-RAD-02:

| | ESP32-S3 | nRF52 |
|---|---|---|
| `Bluetooth.h` implementation | ~354 lines, custom `BLESerial` | ~598 lines, Adafruit Bluefruit |
| Boards with full BLE device identity | none | RAK4631, T-Echo, Heltec T114 |
| Stack | Bluedroid + hand-rolled GATT serial | Bluefruit, `SECMODE_ENC_WITH_MITM` |

The firmware side of ESP32-S3 BLE is nonetheless *correct* — it advertises the
right Nordic UART UUIDs (`6E400001/2/3-B5A3-F393-E0A9-E50E24DCCA9E`), matching
what RNS's own BLE client filters on, and Sideband discovered it. This session's
BLE failure was on the **client** side, not the firmware.

---

## Proposed ANP tickets

Suggested for the ANP board. Numbering left to the board owner.

| # | Title | Type | Priority | Notes |
|---|---|---|---|---|
| A | Fix RNode host-transport input dispatch (G2) | Bug | High | Root cause of Wi-Fi unusability; upstreamable. Must land before G3. |
| B | Buffer RNode Wi-Fi TX into frames (G3) | Bug | High | Depends on A. |
| C | Wire UART0 console for RAD boards (G8) | Task | High | Blocks A/B verification. POW-C-02 already suitable. |
| D | Decide radio-ownership model per measurement (G5) | Decision | High | Feeds `ANP-41` / M3 runtime architecture. Binds by firmware architecture. |
| E | Bind firmware feature set to fitted module variant (G6) | Task | Medium | Extends existing N8R8/N16R2 baseline risk to a functional dependency. |
| F | Add partition-hash re-authorization to flash automation (G7) | Task | Medium | Required for any CI flashing of RAD boards. |
| G | Assess RNode Wi-Fi transport maintenance burden (G1) | Spike | Medium | Decide: adopt and maintain, or route IP via carrier and use USB. |
| H | RAD-02 MCU-family transport assessment (G9) | Spike | Medium | Input to the nRF52 decision. |

**Recommended sequencing:** C → A → B. Without the console, A and B are
verified by bisection, which is what made today expensive.

---

## What is *not* a gap

Worth recording explicitly, so the board does not over-correct:

- **RNS itself.** No protocol-level defect observed. The stack behaved correctly
  for the whole session.
- **The USB host path.** Worked first time on a previously unported custom board,
  and remained stable for hours under a transport node with a live TCP interface.
- **ESP32-S3 as an ANP target.** Nothing found this session argues against it.
  The failures were in two optional transports, not in the SoC or the core port.
