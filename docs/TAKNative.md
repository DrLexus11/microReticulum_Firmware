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

Generic compression is not enough. CoT XML gzips well -- roughly 4 to 5 times --
but 4x on a 6 KB drawing is still a second of air, and 4x on a chat receipt is
still absurd for a message that carries one bit of meaning. A codec that knows
what the fields mean gets 15 to 40 times.

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
to rebuild them. Typed codec where we have one, gzipped CoT where we do not.

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
key, not a registration. Reticulum's Transport already relays between members
through whichever RADs are in between.

Routing peer traffic through command instead would put a node that may be ten
kilometres away, or destroyed, in the path between two people standing next to
each other; it doubles the airtime of every peer exchange; and it makes the one
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

The most native endpoint conceivable is an ATAK plugin that registers a
Reticulum transport in ATAK's own comms menu, so the mesh appears where an
operator already looks for it. It needs the plugin SDK and version-matched
signed builds, it is months out, and it changes none of the above -- it replaces
the localhost socket with an in-process one.

## The outdoor minimal test

Two Rev2s on LoRa with the large antennas, one at the deck and one walking out
with the phone on BLE:

```
waydroid ATAK ─127.0.0.1─ deck endpoint ─ RNS ─USB─ Rev2 ═LoRa═ Rev2 ─BLE─ phone RNS host ─127.0.0.1─ ATAK
```

Duplex chat, markers and drawings, with neither end configured to point at the
other. This is the first outdoor goal, ahead of the two-person exercise, because
it isolates the protocol from the mesh: one hop, one radio, no roaming.

Its prerequisite is already committed -- ESP-NOW recovery used to disable
Reticulum forwarding on a node without its own upstream, which is precisely the
walking node, and its phone could have announced happily into a node that had
stopped relaying for it.

## Sequence

Each of these is a PR, and each leaves something demonstrable behind.

| | | Unlocks |
| --- | --- | --- |
| **A** | Local CoT endpoint on both ends; group destinations; tier 2 as gzipped CoT over LXMF | **The indoor test.** Chat, markers and drawings both ways, no server address typed anywhere. Every CoT type works, none of them cheaply. |
| **B** | Typed codecs: position, chat and receipts, point markers | 850 of the 853 observed events. Makes A affordable on LoRa. |
| **C** | Drawings: typed geometry codec, and tier 2 spill to `Resource` past the MTU | **The outdoor minimal test** passes here. |
| **D** | Tier 3: descriptors, thumbnails, fetch-on-demand, cost consent | QuickPic and data packages. |
| **E** | Teams and missions as group destinations, with membership and mission content | Mission-based operation. |
| **F** | Per-class radio routing | The research item above. Until it lands, tier 3 is manual. |
| **V** | Voice | Measurement first; see the roadmap. Nothing about it is started. |

A is the large one and the one worth doing next: it is what turns a position
demo into TAK.

