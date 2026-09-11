# Partition tolerance, and the pivots to make before they get expensive

Two questions, one root. The mesh, the EUDs and ATAK each name the same human
being differently, and nothing reconciles them. Everything that feels
jerry-rigged downstream — tracks that cannot be addressed, teams that cannot be
verified, a rejoin that does not repair the map — inherits from that.

Companion to [TAKNative.md](TAKNative.md), which sets out the transport tiers
and the group-destination addressing this depends on.

## Part 1 — When the command post is out of range

### What already holds, and why

The local-endpoint decision in TAKNative.md is what makes this survivable at
all. ATAK connects to `127.0.0.1`, so a partition never appears to ATAK as a
disconnected server — the app keeps running and the data simply thins out. Had
ATAK been pointed at the deck's address, losing the deck would end the exercise
for everyone, including two members standing next to each other.

Group destinations finish the argument. Team traffic is addressed to a group,
not relayed through command, so members still see each other with command
absent. Reticulum's Transport routes between them through whatever RADs are in
between. **Nothing needs building for peers to keep working while separated.**

### What actually breaks, tier by tier

**Tier 1 — position, pointer.** Self-repairing. Members in the partition see
each other; command does not. On rejoin the next report rebuilds the picture
within one reporting interval. Nothing to build.

**Tier 2 — chat, markers, drawings, tasks.** This is the real gap. A marker
drawn while separated must reach command when the partition heals. LXMF already
answers this with **propagation nodes**: a node holds messages for peers it
cannot currently reach and delivers them on return. Every RAD can be one.

> **Prerequisite, and it is not currently proven.** The last time propagation
> was exercised here (2026-08-30) inbound `/offer` worked and the outbound half
> did not complete. No propagation node has run since — `~/.lxmd/config` is
> untouched from that date and `lxmd` is not running — so the defect is neither
> fixed nor confirmed, merely unobserved. Tier-2 partition tolerance rests
> entirely on this working, which makes standing it back up and establishing
> where it actually fails a prerequisite rather than a background task.

**Tier 3 — attachments, data packages.** Degrades correctly by construction:
the descriptor arrives over tier 2, the fetch fails while the holder is
unreachable, and it can be retried later. What it needs is a **visible pending
state** rather than a silent failure, so an operator knows a photo exists and
is not yet here.

**Missions.** The only case with genuine divergence: two people edit while
separated. TAK's own mission model keeps a change log, and mirroring that —
append-only entries with per-item versions — makes rejoin a merge rather than a
conflict. Last-writer-wins per item is the acceptable floor.

### What has to be built: anti-entropy on rejoin

Reticulum re-establishes paths on its own; that is not the missing piece. The
missing piece is that **ATAK's model is a stream of events, not synchronised
state**. Nothing in CoT says "here is everything I have". Rejoin therefore
restores the link and leaves both sides with whatever they each happened to
hear.

So each participant keeps a compact digest — per item, an id, a version and a
timestamp — for markers, drawings and mission content. On noticing a peer has
become reachable again, the two exchange digests and pull only the difference.
For a few hundred items that digest is a few kilobytes, and it is the single
highest-value thing to build for field use: without it, the picture after a
rejoin is silently wrong, which is worse than visibly empty.

This is only possible if items have **stable identifiers**, which is Part 2.

### What the operator must see

A stale marker rendered as current is the failure mode that gets people hurt.
Two rules:

1. When a peer becomes unreachable, stop refreshing its tracks and let CoT's
   own `stale` attribute age them out. Never re-stamp a track we have not
   actually heard.
2. Surface reachability directly — "command post: unreachable 4 min", "3 peers
   meshed". This is the radio-awareness work; a partition the operator cannot
   see is indistinguishable from a quiet net.

## Part 2 — The pivots, cheapest now

### The disconnection, named

One person currently has three unrelated names:

| Layer | Name | Where it comes from |
| --- | --- | --- |
| RAD | Reticulum destination hash | the node's identity |
| Columba | LXMF identity hash | the person's identity |
| ATAK | UID + callsign | **invented at the gateway** |

`cot_gateway.py` builds the UID as `"urtn-%08x" % fix.sender_id` and the
callsign from three bytes of the same value. `sender_id` is a **32-bit
truncation of a 128-bit identity**, synthesised in one direction, at one place,
with nothing able to check it.

Everything follows from that. A UID cannot be verified, so a track cannot be
trusted. It is not derivable in reverse, so a task cannot be addressed to a
callsign. It is not stable across a re-provision, so history does not survive.
And it cannot carry a team, so teams cannot be authoritative.

### Pivot 1 — root the EUD identity in the Reticulum identity

Derive the ATAK UID deterministically from the full destination hash, and carry
callsign and team as **signed attributes in the Reticulum announce**, which is
what announce app_data is for. Then a UID is stable for the life of the
identity, a callsign resolves to a destination by lookup rather than by guess,
team membership is verifiable, and `sender_id` disappears from the wire.

**Why now.** Every stored track UID in every ATAK on the network derives from
`sender_id`. Changing it later is a wire-version bump *and* a re-key of every
EUD's history in every client. Today that is five boards and two phones. After
one exercise it is a migration with real data in it. **This is the pivot to
make first**, and it is a precondition for the rejoin sync in Part 1 — you
cannot reconcile state whose identifiers are not stable.

### Pivot 2 — stop treating position as a separate feature

Position has its own destination, its own "gateway hash" setting, its own UI
card and its own send path. Under the tiered design it is one message type on a
group destination like any other.

**Why now.** Columba's position branch is still unmerged. Merging it as it
stands ships the *gateway* concept to users — a setting the tiered design does
not have — and converts a refactor into a migration with a settings screen to
apologise for. Either fold it into the group-destination work before merge, or
merge it knowing the card is temporary and say so in the UI.

### Pivot 3 — one addressing model, decided before markers exist

Tasking currently addresses a per-peer `SINGLE` destination. Markers will want
groups. Chat will want LXMF. Left alone, that is three addressing schemes with
three trust models and three sets of edge cases.

Decide instead that **every message is a destination plus a tier**, and that
tasking is simply "tier 2, SINGLE, signed". Concretely: the task codec's
`recipient` should generalise to a destination of either kind, so a task
broadcast to a team needs no new format.

**Why now.** The task wire format is v1 and unshipped. A destination-kind byte
costs one byte today and a version bump plus dual-path verification later.

### Pivot 4 — let the RAD say what it can see

A RAD knows its ESP-NOW peers, its LoRa neighbours and its link quality, and
tells the phone none of it. Partition detection, reachability display and radio
awareness all want that table. The firmware already half-exposes it through the
pages mechanism.

**Why now.** Cheap while the ESP-NOW and BLE forwarding work is fresh in the
tree; a separate excavation once it is cold.

### Pivot 5 — the team is a membership set, not an address

Measured 2026-09-11, and it invalidates the transport Decision 3 assumed: **a
`GROUP` destination reaches only peers on the same interface as the sender.**
Both implementations exclude `GROUP` from path-table routing by name, so a
group packet never carries transport headers, nothing relays it, and both drop
it above one hop. Details and the bench table are in `TAKNative.md`.

The consequence is not "groups do not cross a radio". It is that **any**
intermediary spends the single hop -- including a shared Reticulum instance on
the same machine, which is how this was found. The intended field topology
(phone, RAD, HaLow, RAD, phone) is four hops. Two phones through one RAD is
two. Neither carries group traffic at all.

#### The two ways out, costed

A tier 2 marker is a 128-byte frame, 168 bytes on air with Reticulum framing,
**132 ms** per transmission at SF7/BW250. Flooding costs one transmission per
node; addressed fan-out costs one routed unicast per peer, each traversing
every hop of its path:

| Team | Flood | Addressed fan-out |
| --- | --- | --- |
| 2 nodes, 2 hops | 263 ms | 263 ms |
| 4 nodes, 3 hops | 526 ms | 1 184 ms |
| 6 nodes, 4 hops | 789 ms | 2 631 ms |
| 10 nodes, 4 hops | 1 316 ms | 4 736 ms |

Flooding is three to four times cheaper on LoRa, and that is a real argument,
not a rounding error. On HaLow the same 10-node marker is 90 ms flooded against
323 ms addressed, and the question stops mattering.

#### Recommendation: addressed fan-out

Four reasons, in the order they actually weigh:

**Flooding cannot be made reliable; addressed can be made cheaper.** Tier 2
promised must-arrive. A flood is best effort with wider reach -- it does not
acquire receipts or retry by spreading further. Addressed delivery starts
reliable and can be optimised later (batching, suppressing peers that already
acknowledged). The migration runs the right way only one of these two ways.

**The deck is upstream RNS and always will be.** Flooding works only where our
firmware is the relay. Put the deck in the middle of a path -- which is what a
command post *is* -- and group traffic silently dies again, at precisely the
seam that already cost an afternoon. A design whose correctness depends on
which implementation happens to be in the middle is a design that fails
quietly, in the field, once.

**The airtime gap is paid on the rare thing.** A marker is an operator action,
not a beacon. Five seconds of channel for a hostile marker reaching ten people
is affordable. What is *not* affordable is fanning out position: 20 bytes plus
framing is ~60 ms, and ten nodes reporting once a minute at four hops would be
37 % of the channel. So position does not fan out -- it stays gateway-bound or
one-hop, exactly where it already is. The tiering in `TAKNative.md` already
says this; the hop limit just makes it binding.

**We need announces anyway.** The UID from pivot 1 decodes to a real
destination that nothing announces, so no peer has a path to it. The membership
list that fan-out needs and the announce that makes pivot 1 true are the same
piece of work.

#### What it changes

The team stops being an address and becomes a membership set: the fleet secret
still gates who can find and decrypt a team's traffic, but delivery is to each
member's `SINGLE` destination, which routes. The `GROUP` destination becomes
vestigial as a transport and should be retired rather than left as a trap that
works on the bench and not in the field.

**Why now.** Before PR B. Markers and peer position are the first things built
on this, and building them on a broadcast that cannot leave the room means
writing them twice.

#### The honest counter-case

If the exercise is LoRa-only and teams grow past six, flooding's advantage
compounds and this recommendation gets expensive. It still does not become
reliable -- so the answer there is flooding *plus* addressed retry for what
matters, which is more machinery than either option alone, not less. Worth
revisiting only with a measured team size and a LoRa-only decision.

### The counter-argument

Pivot 5 is the expensive one and the only one already forced: the measurement
is not a judgement call, and PR A ships a transport that works at one hop and
nowhere else.

Pivots 1 and 3 are wire-format changes, and position v2 is already running on
hardware. Making them means re-provisioning the UID mapping and re-flashing.
That is a real cost and it is being paid deliberately: at five boards and two
phones it is an afternoon, and it is the last moment at which it is only an
afternoon.

## Scheduling

Both parts fold into the TAKNative.md sequence rather than forming a track of
their own.

| | Added work | Why there |
| --- | --- | --- |
| **Before A** | **Pivot 1** — identity rooting; **Pivot 3** — destination-kind in the task codec | Both are wire-format. They must precede anything that stores an id or ships a format. |
| **A** | Pivot 2 — position folded onto the same path as everything else | Same PR that introduces the tiering. |
| **Before B** | **Pivot 5** — retire the group destination as a transport; announce the node destination; build the member list | Measured, not optional: group traffic does not leave one interface. Markers and peer position are the first things built on it, and building them on a broadcast that cannot leave the room means writing them twice. |
| **B** | Peer reachability from the RAD (Pivot 4) | Needs the neighbour table; unblocks the operator display. |
| **C** | **Digest exchange and anti-entropy sync on rejoin** | First tier-2 content worth reconciling. Requires Pivot 1. |
| **C** | Stale-handling rule: never re-stamp an unheard track | Same PR; it is the safety half of the same behaviour. |
| **D** | Pending-fetch state for unreachable attachments | Falls out of tier 3. |
| **E** | Mission change log and per-item versions | Mission divergence only exists once missions do. |
| **Blocking** | **Stand up an LXMF propagation node and establish its real state** | Tier 2 partition tolerance rests on it entirely, and its last observed state is a week-old half-failure on a node that is no longer running. |

The outdoor minimal test in TAKNative.md does not exercise any of this — it is
one hop with both ends in range. **A partition test is a separate exercise**:
walk out of LoRa range with two members meshed to each other, keep working,
walk back, and verify the command post's picture is correct afterwards rather
than merely reconnected. That is the acceptance gate for Part 1, and it should
be run before any claim of field readiness.
