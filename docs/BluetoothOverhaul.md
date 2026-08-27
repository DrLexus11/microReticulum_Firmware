# Bluetooth Low Energy Overhaul

Why the current BLE implementation cannot be paired the way a phone user
expects, what Meshtastic does differently, and what replacing it involves.

Status: **proposal, nothing implemented.** Written 2026-08-27.

---

## 1. The problem, stated as a user sees it

A responder in an incident cannot be asked to install nRF Connect and drive GATT
characteristics by hand. They open the phone's own Bluetooth settings, see the
node, tap it, enter a PIN. That is the entire interaction budget.

Today the node cannot do that. It is discoverable, but it does not appear as a
pairable device that the operating system will bond with, and a link that does
come up has not survived past roughly 45 seconds in testing. The workaround has
been nRF Connect, which works precisely because it bypasses the OS pairing flow
and speaks GATT directly. That is a developer tool, not a deployment.

This was previously read as a possible Columba problem. It is not: the same
limitation applies to any client that expects standard OS pairing.

## 2. Root cause

Two implementations, one difference that explains the symptom.

| | This firmware (`BLESerial.cpp`) | Meshtastic (`src/nimble/NimbleBluetooth.cpp`) |
| --- | --- | --- |
| Stack | **Bluedroid** (`BLEDevice::*`) | **NimBLE** (`NimBLEDevice::*`) |
| Security | `setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM)` | `setSecurityAuth(BOND \| MITM \| SC)` |
| IO capability | **not declared** | `setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY)` |
| MTU | default | `setMTU(517)` |

**Bonding is never requested.** Without `BLE_SM_PAIR_AUTHREQ_BOND` the peer has
no long-term key to store, so the phone cannot keep the pairing. The device
never becomes a normal remembered entry in system settings, and every
reconnection is a fresh unauthenticated attempt.

**No IO capability is declared.** `BLE_HS_IO_DISPLAY_ONLY` is what tells the
peer "this device shows a number, ask the human to type it". Without it the OS
has no defined pairing interaction to present, so it offers none. The existing
passkey work — random when a display is present, `BLE_FIXED_PASSKEY` otherwise —
is sound and is not the problem; nothing was advertising that the passkey should
be entered.

**Secure Connections is not requested**, so pairing falls back to legacy
mechanisms where it happens at all.

The 45-second drop is a separate defect with a separate cause already observed
in this project: `serial_write()` reaches `SerialBT.write()` and blocks the main
loop on `notify()` when the link is half-open. Meshtastic decouples the radio
loop from BLE delivery. Fixing pairing without fixing that yields a device that
pairs and then stops responding.

## 3. Why replacing the stack is the right move, not just adding flags

The three security lines could in principle be set on Bluedroid. Replace the
stack anyway, for reasons that are measured rather than aesthetic:

- **Flash.** `HAS_BLE` is already `true` on both RAD boards, so Bluedroid is
  inside the current image, which sits at 87.3% (Rev 1) and 88.1% (Rev 2) of the
  application partition. NimBLE is substantially smaller. This overhaul is
  expected to *return* headroom at a point where headroom is scarce, which is
  the opposite of the usual cost of a new feature.
- **A working reference.** Meshtastic runs this configuration on the same
  ESP32-S3 silicon, against the same phone operating systems, at scale. The
  checkout at `~/projects/firmware` is a behavioural oracle in the same way
  `rrcd` and NomadNet were for RRC.
- **Maintenance.** Bluedroid is in long-term maintenance in ESP-IDF; NimBLE is
  where current work happens.

## 4. The larger goal: BLE as a Reticulum interface

The end state is not a BLE serial console. It is a `BLEInterface` sitting beside
`TCPServerInterface` and `UDPInterface`, so a phone attaches over Bluetooth
exactly as it attaches over TCP and gets LXMF, RRC and NomadNet unchanged, with
no new application protocol.

This matches the project's stated philosophy of using every radio the board has,
and it is the most disaster-appropriate attachment path available: it needs no
access point, no DHCP, no router and no local infrastructure of any kind. When
the building's network is gone, BLE is the only link a phone still has to a node
in the same room.

Honest constraints: a ~517-byte MTU and modest throughput. That is comfortable
for LXMF text, RRC chat and NomadNet pages, and unsuitable for bulk transfer.
The propagation node's measured stall above roughly 8 KB is a reminder to size
expectations before building on it.

## 4a. Prerequisite: flash headroom

This work is scheduled after the flash-headroom item in
[`docs/FeatureRoadmap.md`](FeatureRoadmap.md), and depends on it twice over.

`impr-rad01-rev2` is at 90.2% of its 2 MB application partition, so a stack
replacement has nowhere to land while it is in progress -- both stacks are
briefly present during a port.

More importantly, the claim in section 3 that NimBLE returns headroom is
**unmeasured**. `RAD01_NO_BLE` does not currently compile, because `Remote.h`
uses `bt_devname` for the SoftAP SSID and DHCP hostname while that symbol lives
behind the Bluetooth guard. Until that is fixed nobody has built a
Bluetooth-free image to compare against, and the size argument for this whole
overhaul rests on a link-map reading that proved unreliable. Fix the target,
take the measurement, then start.

## 5. Proposed sequencing

Deliberately two stages, so the first is provable on its own.

### Stage 1 — pairing and link stability

1. Replace Bluedroid with NimBLE behind the existing `HAS_BLE` gate.
2. Request `BOND | MITM | SC` and declare `BLE_HS_IO_DISPLAY_ONLY`.
3. Keep the existing passkey policy: random with a display, `BLE_FIXED_PASSKEY`
   without.
4. Decouple BLE delivery from the main loop so a half-open link cannot block
   the radio.
5. Record the flash and RAM delta against the current image.

Acceptance: pair from the phone's own Bluetooth settings with no third-party
app; the device appears as a remembered device; reconnect after the phone
leaves and returns; hold a link and pass traffic for at least 30 minutes.

### Stage 2 — Reticulum interface

6. Wrap the GATT link as an `RNS::InterfaceImpl`, following the
   `TCPServerInterface` pattern.
7. Frame and bound it, honouring the negotiated MTU.
8. Prove LXMF delivery, an RRC session and a NomadNet page fetch over BLE alone,
   with WiFi disabled.

Acceptance: a phone with no network attachment uses the node for all three
services.

## 6. Risks

- **Coexistence.** BLE and WiFi share one radio on the ESP32-S3. Throughput and
  latency interact, and RRC holds long-lived Links. Measure with both active
  rather than assuming.
- **A partial port is worse than none.** Pairing that works but drops after
  a minute reads as a broken product. Stage 1 is not done until the link is
  stable, which is why the 30-minute hold is in its acceptance and not deferred.
- **Bonding storage.** Bonds must persist across reboot or every restart forces
  re-pairing. Confirm where NimBLE keeps them and that it survives a firmware
  update, as the RRC hub identity does.
- **Flash is a prediction, not a measurement.** The expectation that NimBLE
  frees space should be confirmed early, because the plan changes if it does not.
