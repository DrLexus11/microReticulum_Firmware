# BLE Overhaul — Branch Working Notes

Scratch plan for `feature/ble-nimble-overhaul`. The design and rationale live in
[`docs/BluetoothOverhaul.md`](BluetoothOverhaul.md); this file only tracks how
the branch is sequenced and what must be true before it starts.

---

## Blocker status

1. **Space -- cleared.** The repartition is merged. `impr-rad01-rev2-uart` now
   links at **44.5% of a 4 MB application partition** (1867893 bytes), against
   the 90.2% of 2 MB that made a stack swap impossible. Both stacks can be
   linked at once with room to spare.
2. **The size argument -- still unmeasured.** The claim that NimBLE returns
   headroom rests on a link-map reading that proved unreliable: it attributed
   44 MB to a 1.8 MB image. `RAD01_NO_BLE` does not compile, so no
   Bluetooth-free image has ever been built to compare against. Until that
   number exists, "NimBLE is smaller" is folklore, and the first commit below is
   what turns it into a number.

3. **Carried in from the bridge branch:** Rev 1 resets roughly every two hours
   for reasons not yet known, with ample heap free and with no observer
   attached. See [`CarriedIssues.md`](CarriedIssues.md). This matters here
   specifically: a dropped BLE link at the two-hour mark could be the new stack
   or could be that, and stage 1 acceptance below asks for a 30-minute hold
   which sits uncomfortably close. Run acceptance on Rev 2, or settle the resets
   first.

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

**The stated defect was wrong, and this was checked before porting anything.**
Bluedroid already sets `ESP_LE_AUTH_REQ_SC_MITM_BOND` -- bonding, MITM and
Secure Connections -- and `ESP_IO_CAP_OUT`, which is display-only. Those are
exactly the settings this plan proposed to add via `setSecurityAuth` and
`setSecurityIOCap`. Porting to NimBLE and setting them would have changed
nothing about pairing, and the port would then have been credited with a fix it
did not make.

The real reason a phone could not pair is a gate, not a stack.
`bt_security_request_callback()` returns false unless `bt_allow_pairing` is set,
and that is only true for 35 seconds after an explicit trigger -- a KISS frame
nothing sends by hand, or a five-second button hold. A phone tapping the device
in its Bluetooth settings is refused outright; a GATT explorer that never
requests pairing connects fine. That is the reported symptom precisely.

Fixed by giving Bluetooth a provisioning surface (namespace 114): enable, a
"Pair Now" action that opens the window, and read-back of state, device name and
the pairing passkey -- which on a display-less board was otherwise unknowable,
so pairing could not be completed even with the window open.

What remains for a NimBLE port is therefore **size and link stability, not
pairing**:

- NimBLE in place of Bluedroid, behind the existing `HAS_BLE` gate. All
  Bluetooth costs 580 KB of flash and 27 KB of RAM (measured, `FlashHeadroom.md`
  §3.2), which is the ceiling on what the port can return.
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

## The evidence that actually justifies the port

Recorded because the branch's original justification did not survive contact
with the code, and this one is worth more than either of the two guesses that
preceded it.

**The advertisement is now provably correct, and the phone still cannot see it.**
Verified from the Deck's own adapter against a running Rev 2:

    Device 80:B5:4E:F4:C7:C5 (public)
        Name:        RNode 87D8
        Appearance:  0x0080 (128)   Icon: computer
        UUID:        Nordic UART Service (6e400001-...)
        AdvertisingFlags: 06     -- LE General Discoverable | BR/EDR Not Supported

Continuous (seen repeatedly with the pairing window closed), at -35 dBm, and
connectable -- nRF Connect establishes a link. BlueZ resolves every field. A
phone's Bluetooth settings still does not list it.

**The same boards paired directly from Android's own Bluetooth settings when
running Meshtastic, which uses NimBLE.** Same hardware, same phone, different
stack. That rules out the phone, rules out Android's treatment of LE-only
peripherals, and rules out the advertising payload -- which has now been
measured rather than assumed.

So the remaining difference is Bluedroid itself, in details that are not visible
from the fields the Arduino wrapper exposes. Chasing them one at a time against
an unknown reference is precisely the "plausible fix for an unmeasured fault"
this project has lost most of its time to. Porting to the stack that is known to
work on this exact hardware is the cheaper and more honest path.

**This is a better reason to port than either previously recorded.** The plan
originally blamed missing `setSecurityAuth`/`setSecurityIOCap`; both were already
set. The next hypothesis was the 35-second pairing gate; that was a real defect,
now fixed, but the device raised a pairing prompt on the phone once the window
was open, so it was not the blocker either. The size argument (580 KB, measured)
is real but was never urgent. Discoverability from a phone's own settings is the
product requirement, and a working reference implementation on the same boards
is the strongest evidence available that NimBLE delivers it.

**Acceptance is unchanged and now has a baseline:** the device must appear in
Android's Bluetooth settings by name, with no third-party app, exactly as
Meshtastic does on the same hardware.

`h2zero/NimBLE-Arduino` 2.5.1 is available and documents a migration path from
the Bluedroid API, which `BLESerial.cpp` uses directly.

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
