# Mesh Resilience and Self-Healing (QuakeMesh)

Design notes for autonomous mesh survival: what already works, what BLE and
WiFi can and cannot do, and what to build so an orphaned node finds its way
back.

Target deployment: rooftop "blackbox" relays on apartment buildings, IMPR-RAD
nodes inside apartments, residents attaching with phones. Nodes must survive
losing their relay and re-absorb automatically when a field team restores one.

Status: **design proposal.** Written 2026-08-23. Verified facts are marked as
such; everything else is a proposal.

---

## 1. Layer the roles, do not mesh on every radio

The single most important architectural decision:

| Layer                | Medium                    | Role                                        |
| -------------------- | ------------------------- | ------------------------------------------- |
| **Backbone**         | LoRa 868 MHz              | node ↔ node. *This is the mesh.*            |
| **Client attachment**| BLE, WiFi SoftAP          | human ↔ node. Short range, higher rate.     |
| **Uplink** (optional)| WiFi STA → TCP            | mesh ↔ internet, when infrastructure lives. |

Do **not** try to mesh over BLE or WiFi. Both are 2.4 GHz, both are badly
attenuated by reinforced concrete, and both cost far more power than LoRa RX.
Their value is attaching people to a node that is already meshed, which is a
different and easier problem.

## 2. What already works today (verified)

- **LoRa comes up with no WiFi, no host, no configuration.** `startRadio()` is
  gated on `hw_ready && eeprom_have_conf()` and `op_mode == MODE_TNC`
  (`RNode_Firmware.ino`), never on WiFi. Verified 2026-08-22: Rev 1 ran a full
  day LoRa-only at IP `0.0.0.0`, announcing and relaying.
  **Deployment rule: the fleet must ship in `MODE_TNC`.** In `MODE_HOST` the
  radio waits for an attached host to start it and an unattended node stays mute.
- **Nodes relay for each other.** `Transport Enabled = true` on both RADs
  (namespace 1, field 1). A node is a transport node; no coordinator exists or
  is needed.
- **Orphans rejoin automatically.** Periodic announces
  (`NOMADNET_ANNOUNCE_INTERVAL_MS`, 5 min) mean a returning relay hears existing
  nodes and paths re-form with no intervention. Verified: the mesh self-healed
  after the Deck slept and woke, with no manual action.
- **PHY divergence is now self-correcting.** Presets, `phy_hash`, and
  commit-confirm auto-revert (2026-08-22) mean a node that is pushed onto
  parameters its peers do not share rolls itself back rather than stranding.
- **WiFi SoftAP exists** (`WR_WIFI_AP`, `wifi_remote_start_ap()`).
- **BLE exists** as a peripheral serial transport (`BLESerial.h`).

So the healing baseline is already good. What follows is what is missing.

## 3. BLE: what it can and cannot do

**Today** BLE is a *peripheral-only* Nordic-UART-style service. It accepts one
connected central (a phone acting as an RNode host). It cannot scan for or
connect to another ESP32. **It is not a mesh and cannot become one for free.**

**Does it need pairing?** No. A plain GATT connection requires no bonding;
pairing is only needed if characteristics demand encrypted access. The class
implements `BLESecurityCallbacks` so passkey pairing is *available*, but the
transport does not require it. Devices do not "recognise each other and mesh
automatically" — that behaviour does not exist in BLE without one of:

1. **GATT node-to-node** — each node runs central *and* peripheral roles,
   scanning and connecting to neighbours. Each link becomes an RNS interface and
   RNS transport handles routing, which is elegant. Cost: significant firmware
   work, ~4–9 concurrent connections, and connection churn as nodes move.
2. **Extended advertising (connectionless)** — BLE 5, supported by ESP32-S3,
   ~255-byte payloads, no pairing, anyone can hear anyone. Good for presence
   beacons and discovery; poor for carrying RNS packets.
3. **ESP-BLE-MESH (SIG Mesh)** — requires *provisioning* into a network with
   NetKey/AppKey (so effectively "paired at all times", just not in the classic
   sense). Flood-based, designed for lighting control, very low effective
   throughput. A poor fit for RNS's ~500-byte packets.

**Recommendation: do not build a BLE backbone.** Through apartment walls BLE
manages perhaps one room; LoRa at 868 MHz manages the building. Keep BLE for
what it is already good at — a resident's phone attaching to the RAD in their
flat with no infrastructure whatsoever. That path works today.

## 4. WiFi: what it can and cannot do

**Does LoRa still work when WiFi cannot connect?** Yes, unconditionally — see
§2. No special configuration is required; this is not something that had to be
arranged for Rev 1 and will not have to be arranged per node.

**Can nodes become APs and mesh with each other?** Not directly. Two SoftAPs
cannot talk: WiFi requires one side to be a station. Meshing over WiFi needs
either AP+STA with a role election, or **ESP-WIFI-MESH**, ESP-IDF's
self-organising tree. ESP-WIFI-MESH is real and works, but it wants to own the
WiFi stack, draws an order of magnitude more power than LoRa RX (~100–200 mA vs
~10 mA), and propagates badly through buildings. For a battery/solar rooftop
box it is the wrong trade.

**What SoftAP is genuinely worth doing:** when the building's AP is gone, a RAD
raises its own AP so residents' phones can join and reach the mesh. That is high
value in exactly the disaster scenario this project targets — but it only
becomes useful once the firmware can accept RNS clients over IP, which today it
cannot (see `docs/ResourceAPI.md` §"TCP interfaces" discussion and the note
below).

**Blocking dependency:** `UDPInterface.h` transmits only to a single
compile-time `UDP_REMOTE_HOST`. A phone joining a node's SoftAP has nothing to
talk to. An RNS `TCPServerInterface` in the firmware is a prerequisite for
SoftAP client attachment to mean anything.

## 5. The orphan problem, and the stampede that must be avoided

Requirement: a node that loses its relay should "try everything" to get back.

**The danger is doing this naively.** After an earthquake — or simply after
mains power is restored to a block — every node reboots within seconds of every
other, discovers it is orphaned, and escalates simultaneously. Thousands of
nodes transmitting harder at the same moment will saturate the band and prevent
the very recovery they are attempting. Half-duplex LoRa has no collision
detection; the failure is self-reinforcing.

Three rules follow, and they matter more than any clever discovery scheme:

1. **Heal by listening, not by transmitting.** Receiving costs no airtime and
   jams nobody. An orphaned node should spend its time in RX, not shouting.
2. **Enforce duty cycle.** `st_airtime_limit` / `lt_airtime_limit` are currently
   `0.0` (disabled). For a real deployment under EU 868 rules the 1% duty cycle
   must be enforced — this is a safety property of the whole network, not a
   per-node nicety.
3. **Jitter everything.** Announce timing must carry a random offset so that
   simultaneously-rebooted nodes do not transmit in lockstep.

## 6. Proposed orphan state machine

All of this lives in the firmware/interface layer. **Nothing here requires
changes to Reticulum core** — the only RNS-level knobs used are the announce
interval and transport enable, both already exposed.

**Normal** — packets decoded recently. Announce on the standard interval with
jitter. Nothing special.

**Suspect** — nothing decoded for ~2 announce intervals. Stay on the configured
preset. Keep listening. Do not change anything yet; the peer may simply be
quiet.

**Orphaned** — nothing decoded for a longer window (tens of minutes). Begin a
**listen-only preset sweep**: dwell in RX on each known preset from
`RadioPresets.h` for a bounded period, looking for *any* demodulated traffic.
Transmit nothing on a preset until traffic has been heard there. This is
duty-cycle-free, jam-free, and directly solves "a field team arrived with a
relay on a different preset".

**Rejoining** — traffic heard on some preset. Adopt it, announce (jittered), and
let RNS path discovery do the rest. Commit-confirm already guards this: if the
adopted preset turns out not to carry traffic, the node reverts.

**Notes**

- The existing RX watchdog (`RADIO_RX_WATCHDOG_MS`) is the natural trigger for
  Suspect→Orphaned; it already tracks exactly this signal.
- The sweep must remember the *configured* preset and return to it, so a node
  that hears nothing anywhere ends up where it started rather than parked on an
  arbitrary setting.
- Sweep dwell must exceed the longest announce interval in the fleet, or the
  node will step past a healthy network that simply had not spoken yet.

## 6a. Implemented 2026-08-23 (NOT yet hardware-verified)

Both boards were away when this was written, so everything below is verified by
build and code inspection only. **Test on return before trusting any of it.**

- **Announce jitter** (`NOMADNET_ANNOUNCE_JITTER_MS`, 0–60 s, re-rolled every
  cycle and also at arm time so the *first* announce after a mass reboot is
  already spread). The PRNG is seeded from `esp_random()` in `setup()`, so
  identical firmware still yields different offsets per node.
- **Duty-cycle knob added, deliberately left DISABLED for lab work.**
  `RADIO_DUTY_CYCLE_LONGTERM` defaults to 0.0; set it to `0.01f` (EU 868
  sub-band g3) to ship. It was briefly defaulted on and then reverted on
  2026-08-23: enforcement throttles the bench, because 1% of an hour is 36 s of
  transmit time and serving one NomadNet page costs roughly a second, so a
  back-to-back test run exhausts the budget in minutes and every later failure
  looks like a radio fault rather than a deliberate `airtime_lock`.

  **This is a release requirement, not an optimisation.** The `#ifndef` guards
  let a production environment in `platformio.ini` set it without touching
  `Config.h`. Accounting runs regardless of whether a limit is set, so bench
  runs still report via `[duty]` what a real deployment would have spent —
  which is the number to check before enabling it for real.
- **`[duty]` / `[ble]` telemetry** in the periodic report, added deliberately
  alongside the change above: a node muted by `airtime_lock` is otherwise
  indistinguishable from one with nothing to say, and BLE had no status output
  at all.

**BLE needs no firmware work.** `bt_init()` already runs at boot and starts BLE
whenever the EEPROM byte at `ADDR_CONF_BT` equals `BT_ENABLE_BYTE`, toggled over
KISS by `CMD_BT_CTRL` (0x46). Enabling it is a runtime command, not a build.

**Known gap, deliberately not built blind:** BLE cannot be enabled *remotely*,
because `CMD_BT_CTRL` needs a local KISS link — a rooftop node reachable only
over LoRa cannot be told to turn BLE on. The fix is a provisioning field, which
should be added with hardware present: field registration has subtle failure
modes (a `FF_REBOOT_REQUIRED` field's setter never runs on commit — that cost a
debugging cycle on 2026-08-22), and adding another one unverified invites the
same bug.

**Open architectural question for the BLE test:** the firmware's BLE is a KISS
*host* transport, i.e. RNode semantics, where the attached phone owns the radio.
Our nodes run `MODE_TNC` with their own RNS stack. A host and the local stack
can coexist on one modem — that was demonstrated on 2026-08-22 by injecting KISS
frames into Rev 1 while it ran as a TNC — but whether a phone's RNS behaves
sensibly when the board is *also* an independent transport node on the same
channel is unverified. Note in particular that the board does not loop its own
transmissions back to the host, so a BLE-attached phone may not learn a path to
the board it is attached to. Resolve this by experiment before designing around
it.

## 7. Build order

1. **`TCPClientInterface`** (outbound) — lets a node anchor itself to an
   always-on off-site transport through NAT, with no port forwarding.
2. **`TCPServerInterface`** (inbound) — makes SoftAP and LAN client attachment
   meaningful; turns a mains-powered node into the local hub.
3. **Announce jitter + duty-cycle enforcement** — small, and required before any
   real deployment.
4. **Orphan state machine with listen-only preset sweep.**
5. *(Optional, later)* BLE GATT node-to-node as a short-range last resort, only
   if field experience shows it is needed.

## 8. Non-goals

- BLE or WiFi as a mesh backbone.
- ESP-WIFI-MESH.
- Changes to Reticulum core routing. The stack already heals; the work is
  keeping nodes on a common PHY and attached to something.
