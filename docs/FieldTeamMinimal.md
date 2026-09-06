# The minimal field exercise: two people, two RADs, two phones

No internet, no central command, no Vox. Two people walking around who can see
each other, message, and mark a spot — staying connected because one radio or
the other still reaches.

Scoped 2026-09-06. Vox is months out (design, fab, assembly), so this is what
can be run with hardware in hand.

---

## 1. What already exists

More than the TAK work suggested, because Columba got there first:

| Capability | Where it lives | State |
| --- | --- | --- |
| See each other on a map | `LocationSharingManager` → `ReceivedLocationDao` → `MapScreen` | works |
| Offline maps | MapLibre, `MapLibreOfflineManager` | works |
| Messaging | LXMF | works |
| Phone ↔ RAD | BLE (`BLEInterface`, `ColumbaRNodeInterface`) | works |
| LoRa mesh | IFAC-gated, provisioned | works |
| ESP-NOW peer discovery, no infrastructure | `request_recovery_scan()` sweeps 1–13 when STA and unconnected | works on the bench |
| Trustworthy clock | signed time propagation, three authorities | works, hardware-verified |

A correction to something said earlier in this project: **ESP-NOW does not need
an access point.** `request_recovery_scan()` requires exactly `WIFI_MODE_STA`
and *not connected*, then sweeps channels to find a peer — which is the state a
node provisioned by `tools/provision_node.py` is already in, because it clears
the stored SSID. The "not a backbone" note was about range and walls, not about
needing infrastructure.

It is still one hop with no forwarding: ESP-NOW moves frames between radios on
one channel and Reticulum does all routing above it. Fine for two people,
something to re-examine at four.

## 2. What is missing

1. **Nothing proves the failover.** ESP-NOW has only ever been seen attaching to
   a parent on the bench, never RAD-to-RAD in a field with no AP anywhere, and
   the ESP-NOW → LoRa transition has never been measured at all. Every feature
   below assumes it works.
2. **Seeing each other costs too much.** Location sharing rides LXMF telemetry:
   a msgpack payload with lat/lng/acc/ts/expires and icon appearance, plus
   LXMF's 64-byte signature and headers, plus link establishment — a few hundred
   bytes every 60 s per recipient. The compact codec built for the CoT gateway
   carries the same fix in **twenty bytes**. It was pointed at a gateway; a peer
   is the same problem.
3. **No shared markers.** Location sharing puts *people* on the map. There is no
   "drop a point and the other person sees it", which is half of what the
   exercise is for.
4. **No radio awareness.** Neither the phone nor the panel says which interface
   a peer is currently reachable on, so during the exercise nobody can tell
   whether they are on ESP-NOW or LoRa except by reading serial logs.

## 3. The radios, and what each dictates

| | ESP-NOW | LoRa |
| --- | --- | --- |
| Band | 2.4 GHz | 868 MHz |
| Rate | 100 kbps (measured, MTU 564) | 10.9 kbps @ SF7/BW250 |
| Range | ~100 m line of sight, far less through bodies and walls | ~1–3 km line of sight |
| Needs infrastructure | no | no |
| Carries voice | plausibly — Codec2 at 3.2 kbps is 3% of it | no — it would take the whole channel |

Which radio is reachable dictates what is available, and that is the point
rather than a limitation. Position and messages must work on either. Voice, if
it is ever built, is an ESP-NOW-only capability that appears and disappears as
people move — and it should be presented that way rather than failing
mysteriously.

## 4. The roadmap

Four PRs. The first one builds nothing.

### PR A — Prove the premise

Before adding features to a link nobody has demonstrated: take two RADs and two
phones outside and establish, with numbers,

- that two RADs with cleared SSIDs find each other over ESP-NOW with no AP in
  range at all;
- the distance at which that link actually drops, walking, with the radios worn
  rather than held up;
- that Reticulum re-routes to LoRa when it does, and how long that takes;
- that it comes back when the two converge again;
- what messages and location updates are lost across each transition.

Expect fixes to fall out of this rather than a clean pass. Whatever they are,
they belong in this PR, and everything after it depends on the answer. If
failover does not work, markers do not matter.

### PR B — Peer position on the compact path

Point the twenty-byte codec at a peer instead of a gateway: the position report
already exists and already unicasts, and with two people the destination list is
two entries long. Columba gains a receiver that feeds the same
`ReceivedLocationDao` the map already renders, so nothing above changes.

Keeps LXMF telemetry as-is for the infrastructure case; adds a cheap path for
the constrained one. Roughly twenty-five times less on air per update, and no
link establishment to fail while walking.

### PR C — Shared markers

A compact marker encoding beside the position one — a point, a type, a short
label, a timestamp — and the Columba UI to drop one and see the other person's.
Same discipline as position: fixed layout, no msgpack, expanded only where
bandwidth is free.

This is the "ping the field" half of the exercise.

### PR D — Radio awareness and field readiness

So the exercise can be read while it happens rather than reconstructed from
logs afterwards:

- which interface each peer is currently reachable on, in Columba and on the
  panel;
- when a peer was last heard, in seconds, not "recently";
- offline map coverage confirmed before leaving;
- battery figures for a RAD and a phone over the intended duration.

### Then run it

Two people, two RADs, two phones. Start together, separate until ESP-NOW drops,
keep messaging and marking over LoRa, converge again. The measurement that
matters is the transition, in both directions — everything else is a link test.

## 5. What this defers

The CoT and TAK work from `TAKCapability.md` §7 is **not on this path** — but it
is not parked either, which an earlier draft of this document got wrong. It has
its own track indoors, where the deck is a command post over the LAN and
ATAK-CIV is already installed: see [`IndoorTAKLab.md`](IndoorTAKLab.md). What
waits for Vox is TAK *outdoors*, with a stationary operator and no
infrastructure — [`TAKFieldExercise.md`](TAKFieldExercise.md) for that shape,
and [`OnboardGNSS.md`](OnboardGNSS.md) for the module that would let a RAD
report its own position rather than borrowing a phone's.

The two tracks share every compact encoding. A marker is the same bytes on the
radio whether ATAK dropped it indoors or Columba dropped it in a field, so
building them once serves both.

Building it first was not wasted, and not only because the codec and its
three-way cross-check are what PR B and PR C are made of: the indoor lab can
exercise the whole of it today. What the two-person exercise does not need is a
CoT gateway *in the field*, which is a narrower claim than the one this document
first made.
