# BLE Overhaul — Branch Working Notes

Scratch plan for `feature/ble-nimble-overhaul`. The design and rationale live in
[`docs/BluetoothOverhaul.md`](BluetoothOverhaul.md); this file only tracks how
the branch is sequenced and what must be true before it starts.

---

## Do not start stage 1 yet

This branch is deliberately opened early so the work has a home, but it is
**blocked on the flash-headroom work** on `feature/flash-headroom`. Two reasons,
both concrete:

1. **There is nowhere to put it.** `impr-rad01-rev2` is at 90.2% of a 2 MB
   application partition. A stack replacement briefly links both stacks, and
   that does not fit.
2. **The size argument is unmeasured.** The claim that NimBLE returns headroom
   rests on a link-map reading that proved unreliable -- it attributed 44 MB to
   a 1.8 MB image. `RAD01_NO_BLE` does not compile, so nobody has built a
   Bluetooth-free image to compare against. Until that number exists, "NimBLE is
   smaller" is folklore.

Start when the partition is enlarged and a Bluetooth-free image size is
recorded.

## First commit on this branch should be the measurement

Before any NimBLE code:

1. Fix `RAD01_NO_BLE`. `Remote.h` uses `bt_devname` for the SoftAP SSID and the
   DHCP hostname, while the symbol is declared inside the
   `HAS_BLUETOOTH || HAS_BLE` guard in `Utilities.h`. A device name is not a
   Bluetooth concern; move it out and give it a MAC-derived fallback so the
   SoftAP still has a name without a Bluetooth stack.
2. Build `impr-rad01-rev1` with and without `RAD01_NO_BLE` and record both
   sizes.
3. Write the delta into `docs/FlashHeadroom.md` §3.2, replacing the note that
   currently says the figure is unmeasured.

That fixes a documented-but-broken target, gives deployments that do not need
Bluetooth a smaller image immediately, and turns the case for this branch into a
number.

## Then stage 1, per BluetoothOverhaul.md §5

Pairing and link stability, in one reviewable piece:

- NimBLE in place of Bluedroid, behind the existing `HAS_BLE` gate.
- `setSecurityAuth(BOND | MITM | SC)` and `setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY)`.
  These two lines are the actual defect: without bonding the phone cannot store
  the pairing, and without a declared IO capability the OS has no pairing dialog
  to present. That is why nRF Connect worked and Settings did not.
- Keep the existing passkey policy: random with a display, `BLE_FIXED_PASSKEY`
  otherwise. That part was never wrong.
- Decouple BLE delivery from the main loop. A half-open link currently blocks
  `serial_write()` on `notify()`, which is a separate cause of the 45-second
  drops and will survive a stack swap untouched.

**Acceptance for stage 1** — pair from the phone's own Bluetooth settings with
no third-party app; appear as a remembered device; reconnect after the phone
leaves and returns; hold a link passing traffic for at least 30 minutes. A port
that pairs and then drops is worse than today, because it reads as a broken
product rather than a missing feature.

## Stage 2 is a separate PR

The Reticulum `BLEInterface` only after stage 1 is proven on hardware. Do not
combine them: stage 1 is a stack swap with a crisp pass/fail, stage 2 is a new
interface with its own framing and MTU concerns.

## Watch items specific to this branch

- **Bond persistence across reboot and firmware update.** If bonds do not
  survive, every restart forces re-pairing and the feature fails its own
  purpose. Confirm where NimBLE stores them and whether that survives a flash,
  the way the RRC hub identity does.
- **WiFi coexistence.** One radio, shared. RRC holds long-lived Links and LXMF
  moves Resources; measure with both active rather than assuming.
- **`printf` in callback context remains unsafe.** See
  `docs/RRCRequirements.md` §12b. The loop task has ~5 KB of margin; BLE
  callbacks will run on their own task, but the same discipline applies.
