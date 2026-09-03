# Mesh Direction: dense deployment, service relay, and the second radio

Written 2026-09-03, end of session, as the standing plan for the next one. It
carries three things: the ordered work to pick up tomorrow, the design position
on problems we have now diagnosed but not solved, and the product context that
changes what "correct" means from here.

Two products from now on:

- **IMPR-RAD** — the cheap, low-power ESP32 endpoint. One per apartment. LoRa,
  BLE to a resident's phone, ESP-NOW to its neighbours.
- **Vox** — a Raspberry Pi Zero 2 W HAT carrying a Morse Micro **802.11ah
  (HaLow)** radio plus instrumentation. A high-level IP radio doing meshing and
  coordination. The reference point is a civilian MPU5.

Everything below is written with both in mind, because several problems that
are unsolvable on RAD alone are straightforward once Vox exists.

---

## 1. Work order for the next session

Ranked, with the reasoning that produced the ranking. Items 1 and 2 are
prerequisites for trusting any measurement of items 3 onward.

1. **A repeatable measurement harness.** Fixed packet counts, both directions,
   counters read from both ends, run over ten minutes rather than one. Today
   Rev 1's own link setup drifted from 0.37 s median to 0.96 s with no code
   change on that path, and OZD-01 went from answering first try to failing
   4/4 within minutes. Every A/B comparison made over a few minutes today is
   therefore unfalsifiable, including my own claim that unicast fan-out made
   things worse. **Nothing else on this list is worth doing until a result can
   be believed.**

2. **Move Rev 1's AP to a clean channel and re-baseline.** ESP-NOW is locked to
   the station's channel, so the mesh currently inherits whatever congestion
   the home network has on channel 9. Free, and it may account for a large part
   of what we have been chasing.

3. **Stop discarding a whole packet when one fragment is lost.** The
   reassembler is strictly sequential, so at three fragments a ~10% frame loss
   becomes ~30% packet loss. Either retransmit the fragment or tolerate gaps.

4. **Make the sender stop being serial.** It sends one frame per firmware loop
   iteration and waits for the completion callback before the next. This is why
   naive per-peer fan-out backfires: addressing more peers multiplies
   head-of-line blocking instead of adding redundancy. A dedicated FreeRTOS
   task, or draining several frames per loop, must land *before* any per-peer
   scheme is retried.

5. **Acknowledge the downstream direction.** A leaf pinned to a parent unicasts,
   so upstream is ACKed and retried in hardware. The hub is pinned to nothing,
   so everything it sends downstream is broadcast: one attempt, no
   acknowledgement. That asymmetry is exactly the observed shape — announces
   arrive reliably, paths stay fresh, links fail to establish.

6. **Then, and only then, per-peer unicast fan-out**, with peers that are
   failing skipped rather than blocking the queue.

Also outstanding and blocking everyone else: **`da0cc62` and `856ad17` are
unpushed** on the microReticulum fork. Nine commits on `feature/wall-time`
depend on a revision no clean checkout can fetch.

---

## 2. Relaying services across the mesh, not just time

TAK needs wall time, and today wall time reached a node only because a human
ran `tools/set_node_time.py` at it. That does not survive contact with fifty
apartments. The mechanism is right; the distribution model is missing.

**What already generalises.** `/time` lives on Transport's remote-management
destination and rides an authenticated Reticulum Link, so it is already
indifferent to whether it crosses LoRa, ESP-NOW, BLE, UDP or TCP. Any future
service — position reports, telemetry, config, a TAK feed — should be another
request path on that same destination rather than a new transport. That part
needs no work; it needs to be written down as the pattern and followed.

**What is missing is diffusion.** Time should spread hop by hop: a node with a
good clock offers it to its neighbours, and a neighbour adopts it when the
offered source is strictly better than its own. That is NTP's stratum model
carried over Reticulum Links instead of IP, and it needs three things we do not
have:

- **A stratum number**, not just the `WallTimeSource` enum. GNSS or NTP is
  stratum 1; a node that learned from one is 2; and so on. Adoption requires a
  strictly better stratum, which is also what stops two nodes handing time back
  and forth forever.
- **An automatic offer/accept exchange** between neighbours, replacing the
  manual client.
- **A trust model that scales.** This is the hard part and the reason to think
  about it now.

**Trust is the blocker, and the allow list is the wrong answer.** Today every
management request is `ALLOW_LIST`, and an empty list denies everyone — silently,
before the handler runs. That is precisely what made OZD-02 unable to be given
time at all. Requiring every node to allow-list every other node does not scale
past a bench, let alone an apartment block.

The proposal: **an IFAC-authenticated peer is already a member of this network.**
IFAC is the admission control; passing it is what "belongs here" means. Time
adoption should therefore be gated on IFAC membership plus the existing safety
rules — never backwards, bounded forward step, strictly better stratum,
provenance recorded — and not on a per-identity allow list. Writing the clock
stays privileged in the sense that it is bounded and auditable; it stops being
privileged in the sense that only three hand-entered hashes may do it.

**Vox changes the shape of this.** A Pi runs a real clock, real NTP when any
backhaul exists, and can carry a GNSS receiver trivially. One Vox per building
is a stratum-1 source for every RAD around it, which turns time from a manual
step into a property of having a Vox nearby.

---

## 3. Dense, uncontrolled deployment — the apartment problem

The deployment model is: a node per apartment, placed wherever the resident
puts it, several per large house for coverage, with overlapping range between
neighbours. Nothing about placement is under our control. Getting this right on
day zero is worth more than any amount of bench tuning, and it breaks several
assumptions in what we built today.

**What today's design assumes.** A single pinned parent chosen by a one-bit
rank (`CAP_UPSTREAM`), which produces a tree. That is correct for a sparse mesh
with one obvious hub. In a dense block it degrades:

- **Many candidates all set the bit.** The tie-break falls back to whichever
  answered first, which is the failure we just removed at the orphan level
  reappearing one level up. Needs a real link metric — RSSI at minimum, ideally
  a delivery-ratio estimate, with hysteresis so a parent is not swapped for a
  marginally better one.
- **Broadcast cost scales with density.** Every node in earshot hears every
  broadcast, and the ones that relay multiply it. Rule 2 from
  `ESPNowPeerProtocol.md` (a node with no upstream stops repeating) helps, but
  in a block where many nodes have upstream it does nothing.
- **Parent flapping** between equally good candidates, each flap costing a
  re-announce and a path change through the whole mesh.

**And the constraint that actually decides it: channel.** ESP-NOW must sit on
the station's Wi-Fi channel. In an apartment block every resident's AP is on a
different channel, so two neighbouring nodes that are each associated to their
own home network **cannot hear each other at all**. Our fixtures mesh today only
because they are deliberately unassociated and free to hop. This is not a tuning
problem; it is a property of the radio.

That leaves four options, and only one of them is good:

| Option | Consequence |
| --- | --- |
| Nodes never associate to resident Wi-Fi | Clean single-channel mesh, but no backhaul and no NTP |
| Nodes associate | Mesh only with neighbours who happen to share a channel — i.e. almost never |
| Time-slice between the AP and a fleet channel | Complex, lossy, and drops frames in both roles |
| **Carry the mesh on a different band entirely** | Works, and we are already building the radio for it |

**So the honest conclusion is that ESP-NOW is not the backbone.** It is a
local, last-resort recovery transport for a node that has lost everything else
— which is what the recovery state machine already treats it as. The backbone
for dense deployment is sub-GHz: LoRa today, and **HaLow on Vox**, which is a
separate band from the 2.4 GHz the residents' networks occupy, so association
and meshing stop competing for the same radio.

This also resolves the Bluetooth trade recorded in `ESPNowPeerProtocol.md`. On
one ESP32, BLE and a low-latency ESP-NOW mesh cannot coexist, because
Wi-Fi/BT coexistence forces the modem sleep that costs us 50–180 ms per round
trip and loses broadcast frames. If ESP-NOW is demoted to last-resort
recovery, **BLE wins that argument on RAD without further debate** — the phone
link matters more than the latency of a fallback path.

---

## 4. Where we could push Reticulum itself

We are already well past the upstream branch. The items below are ranked by
what the two products actually need, not by novelty.

**1. Multi-interface, quality-aware path selection.** Reticulum learns paths
from announces by hop count alone, keeps whichever copy arrives first, and never
re-evaluates. A fleet with LoRa, ESP-NOW, BLE and HaLow on the same nodes is
exactly the case that model handles worst: a three-hop HaLow path at megabits
loses to a two-hop LoRa path at 10 kbps, forever. An ETX-style metric with
per-interface cost, and re-evaluation when a better announce arrives, is the
single highest-value contribution we could make — and it is a genuine upstream
contribution, not a fork-local hack.

**2. Link-state telemetry and a mesh map.** Neighbour tables, RSSI, delivery
ratio, parent choice, exported as data. Operators need to see the mesh, and it
is the same data a TAK overlay would render. It also makes item 1 measurable
instead of theoretical.

**3. Time diffusion with stratum** — section 2 above. Small, and it retires
three deferred items.

**4. Airtime-aware scheduling with priority classes.** Already specified in
`WallTimeAndDutyCycle.md`. Becomes urgent the moment position beaconing starts,
because that is the workload that blows a duty cycle.

**5. Opportunistic store-and-forward under mobility.** LXMF already tolerates an
unreachable destination; extending that to deliberate data-muling — a node
carried between two disconnected clusters — is real DTN capability and fits the
disaster model exactly.

**MANET, honestly.** What we are converging on is a MANET whether or not we call
it one: neighbour discovery, parent selection by rank, link metrics, and
loop-free routing. The right move is not to write a routing protocol from
scratch but to let each layer do what it is good at — batman-adv or Babel over
HaLow on Vox, where there is a real IP stack and CPU to run it, and keep
Reticulum as the end-to-end identity, encryption and store-and-forward layer
above it. Reticulum is not a good radio routing protocol and does not try to be.

---

## 5. Vox, and how the two products divide

| | IMPR-RAD | Vox |
| --- | --- | --- |
| Compute | ESP32(-S3) | RPi Zero 2 W, full Linux |
| Radios | LoRa, BLE, 2.4 GHz Wi-Fi/ESP-NOW | 802.11ah HaLow (sub-GHz), Wi-Fi, BLE |
| Role | Endpoint per apartment; phone link | Building//area coordinator; IP backbone |
| Reticulum | microReticulum (C++) | Reference RNS (Python), nomadnet, LXMF |
| TAK | Compact position source | **The CoT gateway** |
| Power | Battery-friendly | ~1 W class, needs planning |

The TAK architecture already anticipated Vox without naming it.
`TAKCapability.md` §3 specifies a "blackbox gateway (Linux, has IP)" that
receives compact positions and expands them to CoT XML for ATAK clients. **Vox
is that gateway** — and because it also carries a HaLow backbone, the link
between gateways stops being the bottleneck the LoRa budget in that document
describes. Ten nodes at one position report per minute fits LoRa; on HaLow the
constraint effectively disappears and the interesting limits become power and
regulatory duty cycle instead.

Open questions to settle before Vox hardware is committed:

- **Regulatory duty cycle on sub-GHz.** EU 863–868 MHz carries duty-cycle
  limits per sub-band. HaLow's capacity does not exempt it, and we already have
  an unenforced duty cycle on LoRa (`RADIO_DUTY_CYCLE_LONGTERM = 0.0f`). Settle
  the band plan and the enforcement model together, once, for both radios.
- **Which mesh layer runs on HaLow.** 802.11ah gives IP; the routing above it is
  a choice (batman-adv, Babel, or plain infrastructure mode with Vox as AP).
  This decides how much of section 4 item 1 we need.
- **Power budget.** A Zero 2 W is roughly an order of magnitude above an ESP32.
  That decides whether Vox is mains-expected with battery ride-through, or
  genuinely portable.
- **How a RAD reaches a Vox.** LoRa is the obvious answer today and needs no new
  radio on either side. Worth confirming before assuming ESP-NOW has any role
  in the RAD-to-Vox link.
