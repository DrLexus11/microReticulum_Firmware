# TAK Capability over the RAD Mesh

Whether ATAK/WinTAK situational awareness can run on this mesh, what it would
cost in airtime, how mobility interacts with Reticulum's routing, and what the
GP-02 GNSS module buys us beyond TAK.

Status: **TAK remains analysis; its wall-time prerequisite is implemented on
`feature/wall-time`.** Updated 2026-09-03.

Getting trustworthy UTC onto every node is designed in
[`TimePropagation.md`](TimePropagation.md).

Its two firmware prerequisites -- adopting wall time, and enforcing the duty
cycle without silencing what matters -- are drafted separately in
[`WallTimeAndDutyCycle.md`](WallTimeAndDutyCycle.md). Both are worth building
whether or not TAK ever is.

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

1. **NMEA read on a second UART** and a parsed fix (RMC/GGA). Small.
2. **Adopt UTC from the fix** into the existing time source, and let LXMF
   announce a real timebase and RRC stamp real timestamps. Small, and the
   highest value per line in the whole document.
3. **Compact position encoding** and a send path to a gateway destination.
   Moderate; the destination and codec patterns already exist from RRC and LXMF.
4. **Blackbox CoT gateway** in Python: receive, expand to CoT XML, serve ATAK
   over TCP/multicast. Moderate, and entirely off-device.
5. **Rate policy and duty-cycle accounting** before any of it is used in anger.

Steps 1 and 2 are worth doing on their own merits. Steps 3 to 5 are TAK proper,
and should not start until the position budget in §2 is agreed as a product
constraint rather than discovered later.
