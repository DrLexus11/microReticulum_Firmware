# On-board GNSS (GP-02) — deferred feature

Reading NMEA from the GP-02 on a second UART, parsing a fix, and offering it to
the node as a position source and a wall-time source.

Status: **deferred, and deliberately not on the critical path for TAK.**
Scheduled 2026-09-06. It is a feature in its own right, not a TAK prerequisite.

---

## Why it was deferred

[`TAKCapability.md`](TAKCapability.md) §5 argued the module justified itself on
the clock alone: the node has no RTC, so a GNSS fix carrying UTC would fix
message expiry, the LXMF propagation timebase and RRC timestamps.

**That argument has been spent.** Time propagation shipped and is
hardware-verified: nodes take signed UTC from a provisioned authority, and the
bench runs three of them — Rev 1 at stratum 1 from NTP, the deck daemon, and
Columba on the phone. OZD-01, a board with no radio of its own and no clock of
any kind, adopted UTC at stratum 2 over ESP-NOW on 2026-09-06. Every consumer
§5 listed now has a real timebase without a single GNSS module in the fleet.

So a fix would be one more `WallTimeSource` among four, not the only one.

For **position**, the phone is already a better answer for the case we are
building first. Columba holds a mesh identity and can sign with it, and an
Android device has a GNSS receiver, a battery and a screen. A responder carries
one. Requiring a soldered header and a second UART to learn where that person is
standing is work we do not need to do to prove the pipeline.

## Where it still earns its place

The unattended node. A relay left on a hill, a vehicle node, a cached beacon —
anything with no phone attached and no operator to carry one. That node cannot
report its position at all without its own receiver, and it is exactly the kind
of node a deployment leaves behind.

It also removes a dependency: a node with its own fix does not need a phone
in range to know the time, which matters when the mesh is partitioned and the
authorities are on the other side of the break.

Neither case blocks TAK. Both are real.

## How it fits when we do build it

The position source is pluggable by design. The firmware owns the compact
encoding and the unicast send path to the gateway destination; a source fills in
a position. The phone is one source, the GP-02 is another, and the wire format
does not change between them. Building this later costs a source
implementation, not a redesign.

The same applies to time: a parsed fix adopts through the existing
`adopt_wall_time()` path with its own `WallTimeSource`, and inherits the stratum
handling, the step limits and the rollback floor already in place.

## Wiring, when the time comes

From [`TAKCapability.md`](TAKCapability.md) §6, still accurate:

1. **UART0 is not available** on Rev 2. It is the KISS transport and the only
   way that board is flashed and provisioned. Use a second UART on free GPIOs.
2. **Only one wire is strictly needed.** For NMEA we read and do not talk back,
   unless reconfiguring the module: GNSS TX → ESP RX, plus ground and 3V3.
3. Avoid the strapping pins and the octal-PSRAM pins.
4. `Boards.h` already carries the `GPS_BAUD_RATE 9600` / `PIN_GPS_TX` /
   `PIN_GPS_RX` pattern in the generic ESP32 block; follow it for the Rev 2
   variant.

**The pin question is answerable from the schematics, not from guesswork.** The
KiCad projects are in `~/projects/kicad_labs`:

- `lab6_mcu_lora/rev2/IMPR-RAD-01/` — Rev 2, with `carrier.kicad_sch` and
  `display.kicad_sch`, the PCB, and a STEP model. `rev1/` alongside it.
- `lab5_gps/` — the GNSS board itself.

These are S-expression text files and can be grepped for a net or a reference
designator directly. §6 of the TAK document called the J3 pinout "the one fact
needed before wiring"; it has been in the schematic the whole time.

Note the Rev 2 header pins are not soldered on the board currently on the bench.

## Effort

1. NMEA read on a second UART, with a parsed RMC/GGA fix. Small.
2. Adopt UTC from the fix as a `WallTimeSource`. Small, and now incremental
   rather than foundational.
3. Register the fix as a position source behind the interface the TAK branch
   defines. Small, once that interface exists.

Nothing here is hard. It was only ever urgent because the clock depended on it,
and the clock does not depend on it any more.
