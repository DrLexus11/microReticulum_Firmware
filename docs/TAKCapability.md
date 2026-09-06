# TAK Capability over the RAD Mesh

Whether ATAK/WinTAK situational awareness can run on this mesh, what it would
cost in airtime, how mobility interacts with Reticulum's routing, and what the
GP-02 GNSS module buys us beyond TAK.

Status: **TAK remains analysis. Its wall-time prerequisite is now finished and
hardware-verified** -- distribution included, which was the part that did not
scale to a deployment. Updated 2026-09-05.

What that leaves is **not** the duty cycle. These boards run under approved
laboratory conditions and are deliberately not airtime-limited: both duty-cycle
limits compile to `0.0f`, every enforcement site is guarded on a non-zero
limit, and the priority classes in the transmit queue are inert by choice
rather than by defect. The regulatory constraint that will apply is on gain,
not on time on air.

So nothing prerequisite is outstanding. §7 below is the whole of what remains,
and it can start whenever the position budget in §2 is agreed.

**Update 2026-09-06: the GNSS module is no longer part of this.** §5 argued the
GP-02 justified itself on the clock alone; time propagation shipped and took
that argument with it, and an Android device carried by a responder already has
a receiver, a battery and a mesh identity in Columba. On-board GNSS is now a
feature of its own, scheduled in [`OnboardGNSS.md`](OnboardGNSS.md), and §7
below is renumbered accordingly. §5 and §6 are kept as written because the
wiring facts in §6 remain correct; read them as background for that document
rather than as work queued here.

Getting trustworthy UTC onto every node is designed in
[`TimePropagation.md`](TimePropagation.md).

Adopting wall time, and the duty-cycle machinery that was drafted alongside it,
are in [`WallTimeAndDutyCycle.md`](WallTimeAndDutyCycle.md). The clock half is
built. The duty-cycle half stays unenforced by policy, not by omission --
see the status note above.

---

## 1. What TAK actually requires

TAK clients speak **CoT (Cursor on Target)**: XML events carrying position,
markers and GeoChat. Transport is normally UDP multicast on a LAN, or TCP/TLS to
a TAK Server. A single position report (PLI) is roughly **500-800 bytes of XML**.

Nothing in TAK expects a mesh. It expects an IP network with a feed of CoT.

## 2. Why raw CoT cannot cross this radio

Measured against our working point -- SF7 / BW 250 kHz, 10.9 kbps, 36 s of
transmit per hour per node at a 1% duty cycle:

| Encoding | On air | Airtime | Channel-wide capacity |
| --- | ---: | ---: | ---: |
| Raw CoT XML PLI (~700 B) | ~735 B | **538 ms** | **67 reports/hour** |
| Compact packed position (~25 B) | ~60 B | **44 ms** | **820 reports/hour** |

Sixty-seven position reports per hour, channel-wide, is not a team --
it is one person reporting once a minute and nobody else transmitting anything,
ever. Raw CoT is not slow over LoRa; it does not fit.

With a compact encoding it becomes real:

| Fleet | Rate | Reports/hour | Verdict |
| --- | --- | ---: | --- |
| 10 nodes | 1 per minute | 600 | **fits** |
| 25 nodes | 1 per minute | 1500 | over budget |
| 10 nodes | 1 per 10 s | 3600 | far over budget |

So the design constraint is fixed before any code is written: **compact on the
wire, expanded to CoT at a gateway**, and position rates measured in minutes,
not seconds. This is the same conclusion Meshtastic reached -- their ATAK support
sends a protobuf `TAKPacket`, not XML.

Note the duty cycle is currently unenforced (`RADIO_DUTY_CYCLE_LONGTERM = 0.0f`).
Position beaconing is precisely the workload that would blow it, so the numbers
above are the ones that matter for anything that ships in the EU, not the ones a
bench test will show.

## 3. Architecture that fits what we already have

```
RAD (GNSS) --compact position--> Reticulum --> blackbox gateway --CoT XML--> ATAK
                                                (Linux, has IP)      TCP/multicast
```

- The RAD reads NMEA from the GNSS module, packs a position, and sends it to a
  **fixed gateway destination**.
- The blackbox receives, expands to CoT XML, and serves ATAK clients over TCP or
  UDP multicast on the local network -- including over a RAD's SoftAP.
- **ATAK never speaks Reticulum.** It sees an ordinary CoT feed, which is what
  makes this tractable: no plugin, no fork, no client work.

### Send to a fixed destination, not by broadcast

The instinct is to broadcast position like Meshtastic does. Resist it here.
Reticulum is not a flooding network: reaching a destination needs a path, and
paths are learned from announces, which are deliberately expensive and
rate-limited. Announcing every minute per node to carry position would flood the
mesh with routing traffic to move a payload of 25 bytes.

Unicasting to one fixed gateway is the better fit. The gateway is stationary and
announces on a normal schedule, so every node -- including a moving one -- can
keep a current path *to it*. The mobile node's own reachability changing matters
much less when it is the sender.

## 4. How mobility interacts with what we have

Honestly: unevenly, and it is worth knowing which parts degrade.

| Subsystem | Under mobility |
| --- | --- |
| **LXMF** | Best. Store-and-forward tolerates a node being unreachable; that is what it is for. |
| **Position beacons** | Fine, if sent as above -- small, stateless, to a fixed destination. |
| **NomadNet pages** | Acceptable. Short request/response, retried easily. |
| **RRC** | Worst. Long-lived Links are stateful and break when the path changes, and link establishment is already the most fragile part of that stack in our testing. |

A mobile responder should expect messaging to keep working and a live chat
session to drop and need re-joining. That is a product statement worth making
before someone discovers it in an exercise.

## 5. The GNSS module buys something bigger than TAK

> **Superseded 2026-09-06.** Every consumer listed below now has a real
> timebase from signed time propagation, with no GNSS module in the fleet. The
> argument was sound when written and is no longer load-bearing; see
> [`OnboardGNSS.md`](OnboardGNSS.md).

The node has **no real-time clock**, and it has cost us repeatedly:

- the LXMF propagation announce previously advertised **uptime** as its
  timebase; the wall-time branch now advertises UTC when known and zero when
  unknown;
- message **expiry cannot be implemented** at all, because ageing needs a clock
  -- Python expires at 30 days and we only evict on capacity;
- RRC hub timestamps previously stayed **0**; the wall-time branch uses Unix
  milliseconds after synchronization and preserves zero while unknown.

A GNSS fix carries UTC. Wiring the module solves a problem we have deferred three
times, independent of whether TAK is ever built. **That alone probably justifies
the work**, and it is the argument to lead with rather than TAK.

Caveat: time then depends on a fix. Treat it as "set the clock when a fix
arrives, keep running on the monotonic clock afterwards", not "no fix, no time".

## 6. Wiring the GP-02 on Rev 2

> **Deferred 2026-09-06**, to [`OnboardGNSS.md`](OnboardGNSS.md). The facts
> below are still accurate. One correction: the J3 pinout this section calls
> "the one fact needed before wiring" is answerable from the KiCad projects in
> `~/projects/kicad_labs/lab6_mcu_lora/rev2/IMPR-RAD-01/`, not from inspection
> of a board.

The module is a UART GNSS emitting NMEA, conventionally 9600 8N1. `Boards.h`
already carries a `GPS_BAUD_RATE 9600` for other variants, and the
`lilygo_t_echo` variant shows the pin-definition pattern to follow.

Two constraints specific to Rev 2:

1. **UART0 is not available.** It is the KISS transport and the only way that
   board is flashed and provisioned. Use a second UART on free GPIOs.
2. **Only one wire is strictly needed.** For NMEA we read; we do not talk to the
   module unless reconfiguring it. GNSS TX -> ESP RX, plus ground and 3V3.

Pins in use on Rev 2: 4, 5, 6, 10, 11, 12, 13, 14, and 43/44 for UART0. Avoid the
strapping pins and the octal-PSRAM pins. Which GPIOs the J3 header actually
exposes decides the rest -- that is the one fact needed before wiring.

## 7. Effort, in order

Renumbered 2026-09-06. The two GNSS steps that led this list are now
[`OnboardGNSS.md`](OnboardGNSS.md); what is left is TAK proper, and none of it
waits on hardware.

1. **A position source interface.** *(built)* One small seam: a source supplies a fix,
   the firmware does not care where it came from. The phone supplies it now
   through Columba; the GP-02 supplies it later without changing anything
   downstream. Building this first is what keeps the module off the critical
   path instead of merely postponing it.
2. **Compact position encoding** *(built)* and a unicast send path to a fixed gateway
   destination. Moderate; the destination and codec patterns already exist from
   RRC and LXMF. §3 explains why this is unicast to a stationary gateway rather
   than a broadcast.
3. **Position from Columba.** *(built)* The phone already holds a mesh identity and signs
   with it for the time-authority work, and Android has GNSS. This is the
   source that makes the pipeline demonstrable end to end.
4. **Blackbox CoT gateway** *(built)* in Python: receive, expand to CoT XML, serve ATAK
   over TCP/multicast. Moderate, and entirely off-device.
5. **Rate policy** *(built)* before any of it is used in anger. Accounting, not
   enforcement: the point is knowing what a node spends, so a position cadence
   can be chosen on evidence. Airtime is not capped on these boards, and the
   regulatory constraint that will apply is on gain.

All five are done. `tools/cot_gateway.py` receives on
`rnstransport.position.report`, expands each report into a CoT event, and serves
it on TCP 8087 and multicast 239.2.3.1:6969 -- the two places ATAK already
looks. `tools/position_budget.py` computes the §2 table for whichever working
point a deployment actually uses, and a node reports what its own position
traffic has spent on the clock page -- both accounting, neither enforcing.

One discrepancy worth knowing: the planner puts a compact report at about 53 ms
on air where the table in §2 says 44. It uses the firmware's own
`packet_airtime_ms()` arithmetic, so it agrees with what a node will report,
which is the agreement that matters. The table's figure has not been traced.

Steps 1 and 2 are source-agnostic and were safe to build before the budget
question was settled: `Position.h` is the seam, `PositionReport.h` the codec
and send path, and `tools/position_codec.py` the same wire format in Python for
the gateway to decode with. Twenty bytes at full extent against roughly seven
hundred for the XML, and a report is a single encrypted packet to a stationary
gateway rather than a Link -- Link establishment measured about eight kilobytes
of transient heap on the OZD fixture, which would have made the routing more
expensive than the payload all over again. Steps 3 to 5 commit to the position budget in §2, and that should be
agreed as a product constraint rather than discovered in an exercise.
