# Native TAK over Reticulum

How the RAD mesh carries TAK itself, rather than one hand-built message type at
a time. Written after measuring real ATAK traffic; the numbers below are from
this lab, not from a specification.

## What is wrong with what we have

Columba computes a position, encodes it in 21 bytes, and the deck's gateway
renders it as CoT for ATAK. That works. It also describes the entire capability:
one event type, one direction, and only after typing the deck's IP address into
ATAK as a server.

Every further feature on the same pattern -- markers, drawings, chat, missions --
would be another bespoke encoder on the phone and another bespoke renderer on
the deck, and each would arrive with its own half of a protocol. That is the
jerry-rigging. The fix is not more codecs. It is to stop treating the deck as
the place where TAK happens.

## The measurement that decides the design

853 real events that ATAK authored in this lab, as stored by OpenTAKServer:

| CoT type | What it is | Count | Min B | Avg B | Max B |
| --- | --- | ---: | ---: | ---: | ---: |
| `a-f-G-U-C` | position (PLI) | 799 | 414 | 629 | 742 |
| `b-m-p-s-p-i` | pointer / SPI | 35 | 556 | 558 | 558 |
| `b-t-f` | GeoChat message | 6 | 937 | 991 | 1025 |
| `b-t-f-r` | chat read receipt | 4 | 836 | 842 | 848 |
| `b-t-f-d` | chat delivery receipt | 4 | 836 | 837 | 839 |
| `u-d-f-m` | freehand drawing | 2 | 2838 | 4575 | 6311 |
| `u-d-r` | rectangle | 1 | 957 | 957 | 957 |
| `b-m-p-s-m` | marker | 1 | 838 | 838 | 838 |
| `b-m-p-c-cp` | route checkpoint | 1 | 753 | 753 | 753 |

A Reticulum packet carries **383 bytes** encrypted (`MTU` 500, `ENCRYPTED_MDU`
383). **Nothing ATAK emits fits in one packet as XML.** Its smallest observed
event is 414 bytes.

At SF7/BW250/CR4:5, using the firmware's own airtime model:

| | On the wire | Packets | Airtime |
| --- | ---: | ---: | ---: |
| Position, as CoT XML | 629 B | 2 | 501 ms |
| Position, our binary codec | 21 B | 1 | 36 ms |
| Pointer / SPI | 558 B | 2 | 449 ms |
| One chat line, with both receipts | 2670 B | 7 | **2.1 s** |
| One freehand drawing | 6311 B | 17 | **5.0 s** |
| A 1 MB data package | 1 MB | 2738 | **13.7 min** |

Computed with `tools/position_budget.py`, which is the firmware's own
`packet_airtime_ms()`, over `ENCRYPTED_MDU`-sized fragments plus header.

Two conclusions follow, and they are not the same conclusion.

Generic compression is not enough, and it is weaker than it sounds. Measured
across 400 real events rather than assumed:

| | Mean event | Ratio |
| --- | ---: | ---: |
| Raw CoT | 582 B | — |
| Deflate, no dictionary | 378 B | 1.5x |
| Deflate, **curated dictionary** | 232 B | **2.5x** |
| Deflate, dictionary trained on captured traffic | 87 B | 6.7x |

A single event is a few hundred bytes, which is far too little history for
deflate to work with — hence 1.5x, and hence an earlier draft of this document
claiming "4 to 5 times" being simply wrong. What CoT has instead is enormous
repetition *between* events, which a preset dictionary converts into the 2.5x
that `tools/cot_tier2.py` ships.

The trained dictionary compresses best and is not used: it embeds the
callsigns, coordinates and device identifiers it was trained on, and the
dictionary ships to every node on the mesh.

2.5x is worth more than the ratio suggests, because the threshold matters more
than the factor: it takes the mean event from 582 bytes to 232, and a Reticulum
packet carries 383. Most CoT stops needing two packets — 189 ms of airtime
instead of 466. But 2.5x on a 6 KB drawing is still seconds of air, and no
ratio makes a chat receipt sensible for a message carrying one bit of meaning.
A codec that knows what the fields mean gets 15 to 40 times.

Typed codecs alone are not enough either. There are hundreds of CoT types and
plugins invent more. Anything without a typed codec must still work, or the
answer to "can we send drawings" stays "not yet" for ever.

So: **typed codecs for the traffic that dominates, compressed passthrough for
everything else.** Nothing is unsupported; the common cases are cheap.

## Decision 1 -- the server moves onto every device

ATAK connects to `127.0.0.1`. Each participant runs a local CoT endpoint backed
by Reticulum:

- on the phone, inside the RNS host service Columba already runs;
- on the deck, the same endpoint in Python, which waydroid's ATAK points at.

Both implement the same spec. Nobody types anybody else's address, the deck
stops being infrastructure and becomes a participant with a bigger screen, and
an exercise no longer has a component whose loss ends it.

This is how the mesh-TAK bridges that exist already work -- goTenna's and
Meshtastic's ATAK plugins both terminate CoT locally and carry something smaller
over the radio. The difference here is what sits underneath: Reticulum brings
routing, end-to-end encryption and resource transfer that neither of those has.

## Decision 2 -- three traffic classes, not one tunnel

**Tier 1, ephemeral and latest-wins.** Position, pointer/SPI. A raw `Packet` on
a group destination. Loss is fine because a fresher one is seconds away, so it
buys nothing to make delivery reliable. Typed binary. This is what the existing
21-byte position codec already is; the tier just gives it company.

**Tier 2, must arrive.** Chat, markers, drawings, tasks, mission changes. LXMF,
which already has receipts, propagation nodes and store-and-forward -- exactly
the semantics ATAK's own delivery receipts are asking for, and the reason not
to rebuild them. Typed codec where we have one, dictionary-compressed CoT where
we do not.

**Tier 2 has an addressed half and a broadcast half, and only one of them can
keep the promise.** LXMF delivers to a single destination that has an identity;
there is no LXMF message to a `GROUP`. So tier 2 splits:

| | Carrier | Guarantee |
| --- | --- | --- |
| Addressed -- tasking to command, direct chat | LXMF | Receipts, retry, store-and-forward |
| Team broadcast -- a marker dropped for everyone | `Packet` on the group destination | Best effort, and that is all |

This is a property of broadcast rather than a shortcut. A group has no member
list to retry against and no receipt to wait for; reliable multicast over a
partitionable mesh means either an acknowledgement from every member -- which
requires knowing who they are, which a key-derived group deliberately does not
-- or repetition, which spends airtime on the assumption that someone missed it.

What it means in practice: **a marker broadcast to the team can be lost, and
nothing will say so.** Tolerable indoors and for the exercise, where a second
marker costs a gesture. Not tolerable for tasking, which is why tasking is
addressed LXMF with the acceptance record behind it and is not on this path at
all. If broadcast markers need to arrive, the honest options are an
application-level repeat with a sequence number, or sending to known members
individually -- both are real work and neither is in A.

**Tier 3, bulk.** QuickPic images, data packages, drawings past the MTU. Do not
push these. Send a **descriptor** -- uid, name, mime, size, content hash, and a
thumbnail small enough for tier 2 -- and let each receiver **fetch on demand**
over a Reticulum `Link` using `Resource`, which does the segmenting, compression
and retransmission already. The UI shows what the fetch will cost before it
starts, and a fetch is interruptible.

Fetch-on-demand is not an optimisation here, it is the only safe design. One
1 MB data package pushed to a team is fourteen minutes during which nobody's
position, chat or task moves. Pull means the cost is paid by whoever wants the
file, when they want it, and can be declined.

## Decision 3 -- groups, not a star through central command

The instinct to distinguish *send* from *broadcast* is right. Routing everything
through central command's LXMF address to achieve it is not.

Reticulum has both already:

| TAK concept | Reticulum primitive |
| --- | --- |
| send to contact | `SINGLE` destination |
| broadcast, team channel | `GROUP` destination with a shared key |
| mission | `GROUP` destination plus a content store |
| central command | a member of the groups, not a hop in them |

A team name plus the fleet secret derives the group key, so joining a team is a
key, not a registration.

### A GROUP destination does not cross a hop

An earlier version of this section said Reticulum's Transport relays between
members through whichever RADs are in between. **It does not.** That is true of
`SINGLE` destinations, which is why tasking works at three hops; it is not true
of groups, and the group design rested on it.

In both implementations, a `GROUP` destination is excluded by name from
path-table routing -- `Transport.py:1138`, `Transport.cpp:1231` -- so a group
packet never receives transport headers. Relaying requires
`packet.transport_id == Transport.identity.hash`, which such a packet never
carries, so no intermediate node forwards it. Both then drop `GROUP` packets
with `hops > 1` outright. The only broadcast rebroadcast either implementation
performs is for `PLAIN`.

Measured on the bench, 2026-09-11:

| Path | Hops | Result |
| --- | --- | --- |
| Phone -> RAD Rev2 -> ... -> deck (BLE) | 3 | nothing arrives |
| Phone -> deck daemon -> bridge (shared instance) | 2 | nothing arrives |
| Phone -> bridge owning its own interface | 1 | works, both directions |

The second row is the one to remember. The deck's Reticulum runs as a **shared
instance**, so a program connected to it sits one hop behind the daemon that
owns the interfaces. A peer one hop from the daemon is therefore two hops from
the bridge, and every group packet died there -- on the same machine, over
loopback. The wire was never the problem: the deck's TCP interface gained 660
bytes for five markers that were then dropped on delivery.

So the constraint is not "groups do not cross a radio". It is **groups reach
only peers on the same interface as the sender**, and any intermediary at all
spends the single hop, including one on the same host.

What this costs the design:

- The bridge cannot be a shared-instance client. It owns its own Reticulum
  instance and its own interface, and peers connect to that.
- The intended field topology -- phone, RAD, HaLow, RAD, phone -- is four hops.
  Group broadcast does not survive it. **Neither does two phones through a
  single RAD**, which is two hops.
- Anything that must reach a team more than one hop away needs a different
  carrier. The options are unchanged from the tier 2 note above: addressed
  fan-out to members learned from announces, which routes properly and costs
  N times the airtime and needs the membership a key-derived group deliberately
  avoids; or flooding `GROUP` in our own firmware with hashlist dedup, which
  works only because our boards are the only relays in the field topology and
  diverges from upstream RNS.

This is undecided and blocks the outdoor exercise, not the indoor one. It does
not affect tasking, which is addressed.

The reasoning for groups over a star still holds, and the hop limit above does
not rescue the star: routing peer traffic through command would put a node that
may be ten kilometres away, or destroyed, in the path between two people
standing next to each other; it doubles the airtime of every peer exchange; and it makes the one
asset most likely to be targeted the one whose loss silences everybody. A mesh
that fails when its centre fails is not a disaster network.

Command still gets everything it needs -- it is in every group -- and it keeps
the one thing it should have alone: the authority key that signs tasking.

## Radio selection

Not a global preference. A per-class one.

**LoRa is the backbone.** It is the only link that survives separation, and it
is what a walking team member will actually be on. ESP-NOW is channel-locked to
the station's AP and cannot mesh across structures, so it is not a substitute.

**ESP-NOW is the bulk lane.** 100 kbps against LoRa's 10.9, available only when
nodes are adjacent -- which is exactly the situation in which moving a data
package is reasonable.

So tiers 1 and 2 go wherever Transport routes them, and tier 3 waits for
adjacency or asks permission.

**Open problem, stated honestly:** Reticulum selects paths by hop count and has
no notion of bandwidth, and offers no clean way to say "send this one over that
interface". Getting tier 3 onto ESP-NOW deliberately needs either interface-cost
work in the library or an adjacency check against our own ESP-NOW peer table
before opening the Link. This is not solved, and the roadmap treats it as
research rather than as a task with a known shape.

## What this leaves Columba doing

Two things, neither of them TAK:

1. running Reticulum on the phone, and
2. hosting the local CoT endpoint.

The protocol -- codecs, tiers, addressing, golden vectors -- is a module with no
Columba UI in it, shared with the Python command post through cross-implemented
test vectors, exactly as the position and task codecs already are. That boundary
is what makes "we cannot depend on Columba" true in the way that matters: the
stack can be re-hosted in a standalone bridge app, or in an ATAK plugin, without
touching the protocol.

## The ATAK plugin: decided, and deliberately later

**Decision: a thin plugin, after the protocol is stable, binding to Columba
rather than carrying its own stack.**

The expensive part already exists. `IRnsCore.aidl` is a 47-method interface and
`ReticulumService` already runs in its own `:reticulum` process, bound over AIDL
from `:app` -- a real cross-process boundary that has been carrying traffic for
months. Between today and a plugin binding to it stand `exported="false"` and a
permission, not a new subsystem.

*Not* by turning on RNS's shared instance. `share_instance = No` today, and
loopback is not sandboxed between Android apps: opening port 37428 would let
every app on the handset transmit and receive as this mesh identity, with no
authentication. The AIDL service behind a permission is the same idea done
safely.

**It does not reduce Columba's settings.** That is worth stating because it is
the reason people reach for a plugin first. The settings exist because the
identity model is synthesised, not because ATAK cannot reach them; build the
plugin without the pivots in [TAKIntegrationPivots.md](TAKIntegrationPivots.md)
and the same fields simply move. With the pivots, the gateway hash, callsign and
team all disappear on their own, and what remains is the split worth having:
Columba owns radio configuration, ATAK owns TAK configuration.

What the plugin does buy is three things a localhost CoT stream structurally
cannot:

1. **Interception before serialisation.** Inside ATAK, CoT arrives as objects.
   Encoding straight from that model into the compact codec means XML is never
   produced on the phone at all -- and XML size is the whole airtime problem.
2. **Consent before airtime.** A drawing is five seconds of the shared channel
   and a data package is fourteen minutes. Across a CoT socket we can only send
   or drop; in-process we can ask, at the moment the operator commits.
3. **Mesh state as first-class UI** -- reachability, pending fetches, which
   radio a peer is on. CoT has no way to express any of it.

Against that: plugins are version- and signature-locked, so every ATAK release
can break the build; it is a separate toolchain; and it does nothing for the
deck, which needs the localhost endpoint regardless. Both are maintained either
way.

**What this changes now.** One thing: the protocol module consumes `IRnsCore`
across the AIDL surface, as a plugin would, rather than reaching into Columba
internals. Then the plugin is a re-host -- run the same module in ATAK's process
and export the service -- instead of a second implementation of every codec to
keep in sync. A fat plugin carrying its own Reticulum is the outcome to avoid.

## The outdoor minimal test

**Corrected 2026-09-10.** An earlier draft of this section had two Rev2s on
LoRa. That is the *bench* topology, and it is what the acceptance runs used.
The field kit is not LoRa at all:

```
Columba + ATAK ─BLE─► IMPR-RAD ─Wi-Fi─► VOX-Mini ═HaLow═► VOX-Mini ─Wi-Fi─► IMPR-RAD ─BLE─► Columba + ATAK
```

VOX-Mini is the RPi Zero 2 W with the HaLow uHAT and GNSS, and it hosts the
Wi-Fi the RAD associates to. **The RADs are compiled without LoRa** and act as
BLE-to-Wi-Fi bridges; each side of the exercise is an identical pair.

Three consequences, and the first two are corrections to this document.

**HaLow is the field backbone, not LoRa.** The radio-selection section below
says "LoRa is the backbone", and that remains true of the lab fleet and of any
deployment without a VOX. It is not true of the field kit, where LoRa is absent
by choice.

**The airtime arguments do not bind the field kit.** Every budget in this
document is SF7/BW250 at 10.9 kbps. The narrowest link in the field kit is BLE
or HaLow at range — two orders of magnitude more, as the HaLow table below
sets out. Tier 3 becomes exercisable in the field far earlier than its position
in the sequence suggests, because what gated it was bandwidth and the bandwidth
arrives with VOX.

The tiering itself still holds, and this is the test of it: the compact codecs
cost nothing when bandwidth is plentiful, a HaLow link at range still degrades
toward its 150 kbps floor, and the lab fleet stays LoRa. A design that had to be
unpicked when the bandwidth arrived would have been the wrong design.

**The 863–868 MHz band conflict is retired.** This document warned that a HaLow
module in that band would share it with the fleet's LoRa radios, transmitter
inches from receiver. With the field RADs built without LoRa there is no
co-located LoRa transmitter in the kit, so the constraint applies only to
operating a VOX inside the lab alongside the LoRa fleet — a siting question
rather than a hardware-selection one.

**GNSS arrives on VOX rather than on the RAD.** `docs/OnboardGNSS.md` defers a
GNSS module for the RAD, and the firmware's position path has never had a
source registered — `node_position_register()` is called nowhere. VOX-Mini
carries GNSS, so the field kit gets a position source without that deferred work
being done, and the unexercised firmware path stays unexercised until a RAD
needs to report on its own.

Duty cycle is by permission, as in the lab: the current approval covers lab
conditions, and field testing will carry its own. Nothing in this design assumes
otherwise — the airtime accounting exists so the cost is known, not because a
regulator is enforcing it here.

Afterwards, a CM5 IO board with HaLow and GNSS built in and full-size antennas
replaces the Zero 2 W. That is a hardware refresh under the same protocol, which
is the point of keeping the protocol independent of the radio.

## Sequence

Each of these is a PR, and each leaves something demonstrable behind.

| | | Unlocks |
| --- | --- | --- |
| **A** | Local CoT endpoint on both ends; group destinations; dictionary-compressed CoT broadcast on the group | **The indoor test.** Chat, markers and drawings both ways, no server address typed anywhere. Every CoT type works, none of them cheaply. |
| **B** | Typed codecs: position, chat and receipts, point markers | 850 of the 853 observed events. Makes A affordable on LoRa. |
| **C** | Drawings: typed geometry codec, and tier 2 spill to `Resource` past the MTU | **The outdoor minimal test** passes here. |
| **D** | Tier 3: descriptors, thumbnails, fetch-on-demand, cost consent | QuickPic and data packages. |
| **E** | Teams and missions as group destinations, with membership and mission content | Mission-based operation. |
| **F** | Per-class radio routing | The research item above. Until it lands, tier 3 is manual. |
| **V** | Voice | Measurement first; see the roadmap. Nothing about it is started. |

## Acceptance record -- PR A, 2026-09-11

CoT crossed the mesh in both directions between the deck bridge and Columba's
endpoint, with ATAK pointed at nothing but `127.0.0.1`. Driven by
`tools/tak_cot_acceptance.sh`, which stands in for ATAK at both ends through
`adb forward`, so the loop is observable without a map on screen.

```
== deck bridge is listening      PASS
== phone endpoint is reachable   PASS
== round trip
   deck -> phone     arrived
   phone -> deck     arrived
PASS: CoT crossed the mesh in both directions
```

What that establishes: the framer, the tier 2 codec, the group key derivation
and the UID rewrite all work across the Python and Kotlin implementations on
real hardware. The phone derived the team address `88a2f27d…14053a`
independently and reached the same value as the deck -- the two derivations
agreeing against each other rather than against a stored vector.

### Two defects the run found

**The shared instance cost a hop.** Covered above; it is why the bridge now
refuses to start as a shared-instance client.

**Every node reported the same UID.** Both ends built their ATAK UID from the
*group* destination, which every member of a team derives identically, so the
deck and the phone reported themselves under one identifier:

```
deck's self-report,  as the phone saw it : urtn-88a2f27d…14053a
phone's self-report, as the deck saw it  : urtn-88a2f27d…14053a
```

ATAK draws that as one track teleporting between two positions -- exactly the
outcome pivot 1 exists to prevent, produced by the code meant to implement it.
The old gateway's 32-bit truncation at least told two nodes apart. The UID now
comes from a `SINGLE` destination on each node's own identity:

```
deck  : urtn-a8d8a8a16c9955965d7aaffa0d73f8a7
phone : urtn-da4d8be3a2260eb1e6ca927bd7da293a
```

Both defects were invisible to the unit tests and to the golden vectors,
because both sides agreed perfectly -- on the wrong thing. The vectors pin
agreement between implementations; they cannot pin agreement with the intent.

### Not established

- Anything beyond one hop. The topology under test was phone to bridge over a
  direct TCP interface, chosen because it is the only shape group traffic
  survives. BLE and the RAD path are untested for CoT and, per the hop finding,
  cannot work as designed.
- ATAK itself was not in the loop. The harness speaks the same socket ATAK
  does, and the fixture events came from a real ATAK capture, but a map has not
  yet drawn one of these markers.
- Airtime. The path was TCP over Wi-Fi, so nothing here measures what a marker
  costs on LoRa or HaLow.
- **Reaching a peer by its UID.** The UID now decodes to a real `SINGLE`
  destination that the node answers on, which is what pivot 1 asked for -- but
  neither end announces that destination yet, so no peer has a path to it and
  nothing can actually be sent there. The UID is derivable and correct; it is
  not yet useful. `announce_payload` / `announcePayload` exist and are tested
  on both sides with no caller, waiting for this.

  Left out of A deliberately: an announce needs a callsign to carry, which
  means a setting on both ends, and addressing individual peers is what B and E
  are for. Worth knowing before reading the UID work as finished.

## Acceptance record -- PR B, 2026-09-11

Membership replaces the group address, and position leaves the tier 2 path.
Verified deck-to-deck between two bridges on separate Reticulum instances, and
then deck-to-phone with Columba on the same team.

### Mutual discovery

```
phone : Team member ALPHA is urtn-45a0122763d01e4bc3f6fca2d59073db
ALPHA : team member COLUMBA is urtn-da4d8be3a2260eb1e6ca927bd7da293a
```

Neither was configured with the other. Each announces its own node destination
carrying an HMAC of the team, and each recognised the other's.

### Traffic, both directions

| | Sent | Arrived as |
| --- | --- | --- |
| Deck marker -> phone | `a-h-G`, uid `deck-marker-1` | same uid, `40.9601, 29.1002` |
| Phone position -> deck | `a-f-G-U-C`, uid `ANDROID-PHONE` | uid `urtn-da4d8be3…`, `40.9549, 29.0934` |
| Phone position -> deck | second report | uid `urtn-da4d8be3…`, `40.9750000, 29.1150000` |

The third row is the one worth reading closely. `40.9549` is ATAK's own string,
carried through tier 2 as CoT; `40.9750000` is seven-decimal reconstruction from
the twenty-one byte codec. The first position went as CoT because the endpoint
had not yet learned the ATAK UID, and the second took the typed path -- so the
routing decision is visible in the output rather than asserted.

A marker keeps its own UID and a self-report gets the node's, which is pivot 1
holding across a real exchange.

### Chat, added 2026-09-11

A GeoChat line crossed in both directions between the phone and the deck, each
side rendering the other's callsign from the member registry rather than a raw
identifier:

```
deck -> phone   type=b-t-f  from=ALPHA    text='where u at'
phone -> deck   type=b-t-f  from=COLUMBA  text='on my way'
```

The measurement behind the codec is worth restating because it is stronger than
"chat is expensive": a real GeoChat line from this lab compresses to **402
bytes** against a 383-byte MDU. On tier 2 chat was not costly, it was
**undeliverable** -- and after the frame bound was added it is refused outright,
which before that fix meant an exception the client loop read as a dead socket.

    message   1100 B raw   tier 2 refused   chat  45 B,  71 ms
    receipt    879 B raw   tier 2 345 B     chat  35 B,  64 ms

Real ATAK was connected to the phone's endpoint during this run -- the log line
`This ATAK calls itself ANDROID-…` is the endpoint learning it -- though the
events under test were injected rather than typed into the app.

### What this run does not establish

- **Airtime.** The path was TCP over Wi-Fi. The 85%-against-32% figures that
  justify the typed codec are computed from the firmware's model, not measured
  on a radio.
- **More than one hop.** Both topologies were one hop by construction. Multi-hop
  is the whole reason for pivot 5 and is still untested end to end.
- **A real ATAK.** The harness speaks the same socket, and the events come from
  a real ATAK capture, but no map has drawn one of these.
- **Chat and receipts.** 10 of the 853 observed events still have no typed
  codec and fall through to tier 2.

### Two things found while running it

The backends drop announces whose aspect they do not recognise, and neither knew
the TAK node aspect. Team announces would have been discarded before reaching
the app, and a team would never have discovered itself with nothing logged to
say why. The two aspect lists -- Kotlin and Python -- were "kept in sync by
hand"; there is now a test that reads both.

A bridge started under `nohup` does not print its closing counters on SIGINT, so
the send/receive/suppressed figures are only available from a foreground run.
Not fixed; recorded so the next person does not read an empty summary as zero
traffic.

## After C: the plugin, and what HaLow changes

### The plugin is real, and C is the right gate

By the end of C the protocol is settled — identity, group addressing, tier 2,
and typed codecs for position, chat, markers and drawings. A plugin arriving
then is a **UI and interception layer over a stable wire format**, which is a
few weeks of work. A plugin arriving before it would be co-designing a protocol
inside a version-locked, signature-locked toolchain, debugging both at once.

The IPC it needs already exists and is already exercised in production:
`IRnsCore` (47 methods), `IRnsLxmf`, `IRnsTelemetry`, `IRnsTelephony`,
`IRnsTransportAdmin`, spoken across a real process boundary to `:reticulum`.
Between today and a plugin binding it stand `exported="false"` and a permission.

What it can offer that a localhost CoT socket structurally cannot:

- **The mesh, as a map layer and a panel.** Peers, hop count, which radio, last
  heard, whether the command post is reachable. CoT has no vocabulary for any
  of it, so today it is invisible no matter how well the mesh is working.
- **Direct messaging from inside ATAK**, over `IRnsLxmf` — store-and-forward,
  receipts and propagation already built.
- **Consent before airtime.** A drawing is seconds of a shared channel and a
  data package is minutes. Across a socket we can only send or drop; in-process
  we can ask, at the moment the operator commits.
- **Interception before serialisation**, so CoT XML is never produced on the
  phone at all.

### RRC first needs Eridanus and Columba to be one app

There is no RRC in Columba's AIDL surface today, and RRC lives in Eridanus as a
separate application. A plugin cannot reach it without binding two apps, two
RNS hosts and two identities — which is the arrangement the thin-plugin
decision exists to avoid.

So **merging Eridanus into Columba is a prerequisite for RRC in ATAK**, not a
tidying exercise. It is what produces one RNS host, one identity, and an
`IRnsRrc` alongside the interfaces above. Voice is the same shape and further
along: `IRnsTelephony` already exists for LXST.

### What HaLow changes, and what it does not

Everything in tier 3 is bandwidth-bound, and the numbers are not close:

| | LoRa SF7/BW250 | HaLow (802.11ah) |
| --- | ---: | ---: |
| Throughput | ~10.9 kbps | ~150 kbps to several Mbps |
| 1 MB data package | **13.7 min** | seconds |
| 200 KB QuickPic image | ~2.5 min | ~2 s |
| Codec2 voice at 3.2 kbps | 29% of the channel | negligible |

So the honest framing is that **tier 3 is designed now and becomes ordinary
later**. Descriptors, thumbnails and fetch-on-demand are worth building against
LoRa because they are what make an attachment *possible* there at all; on HaLow
the same design simply stops hurting. Nothing about it needs redesigning when
the bandwidth arrives — which is the test of whether the tiering was right.

Voice is the one that changes category rather than degree. On LoRa it starves
the position reports the map depends on; on HaLow it is unremarkable. It stays
scheduled behind measurement either way.

**One spec question to settle before the HAT is chosen.** If the HaLow module is
the 863–868 MHz variant it shares the band with the fleet's LoRa radios, with a
transmitter inches from a receiver and ETSI duty-cycle limits across both. That
decision may pick the HAT, and it is cheaper to answer on paper than after a
fabrication run.

### Sequence

| | | Depends on |
| --- | --- | --- |
| **G** | Merge Eridanus into Columba: one RNS host, one identity, `IRnsRrc` | nothing — can start whenever |
| **H** | Export the RNS service behind a permission | G, so the surface is complete when it opens |
| **I** | **Thin ATAK plugin**: mesh layer, peer panel, LXMF and RRC messaging, consent dialogs | C and H |
| **J** | Tier 3 over HaLow: data packages, images at full size | D, and Vox hardware |
| **V2** | Voice over HaLow | V1 measurement, G, and Vox hardware |

G and H are software and gated on nothing but time. I is the payoff. J and V2
wait on Vox, which is months out — but they wait on *hardware*, not on design,
and that is the point of doing the tiering now.

A is the large one and the one worth doing next: it is what turns a position
demo into TAK.

