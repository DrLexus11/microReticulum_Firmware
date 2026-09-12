# TAK delivery plan — the phasing, and what gates what

**This file is the single source of truth for what ships in which PR.** It
supersedes the A–J sequence table that used to close `TAKNative.md`. Design
rationale and acceptance evidence stay in `TAKNative.md` and
`TAKIntegrationPivots.md`; this file answers only *what is being built next and
why in that order*.

Agreed 2026-09-12. Revise it when the order changes — do not let the order live
only in a conversation.

---

## The decision behind the order

**Build the infrastructure first. Let the plugin fill what is left, which is
only ever the things TAK's own data model has no field for.**

The rule: *the layer a gap lives in decides the fix.* Transport semantics —
delivery, retry, ordering, offline queue — belong below ATAK, where every
client benefits and ATAK cannot tell the difference. Protocol surface —
missions, data packages — is infrastructure too, as a local shim, because that
is the only route that keeps ATAK's native tools working at all. Only
information TAK has no field for is the plugin's territory.

Two options were rejected explicitly:

**A plugin that replaces chat, teams, contacts and missions.** ATAK plugins are
Android-only. The fleet has iTAK and WinTAK operators, and this abandons them on
day one. It is also the largest build for the least return: our chat problems
are transport problems, and a new interface over a lossy transport is still
lossy.

**Infrastructure with no plugin at all.** Gets roughly 80%, and it is the right
80%, but it hits a wall where honest transport produces states ATAK cannot
render. Suppress room receipts to save the channel and ATAK shows no delivery
tick — which an operator reads as failure, when the truth is *delivered to 8 of
9, 1 queued until RAD-04 is back in range*. There is no CoT element for that
sentence. **The plugin's job is to stop the interface lying, not to replace
it.**

## Scope: what LoRa carries, what waits for HaLow

**On LoRa:** positioning, markers, drawings, routing, messaging. That is the
whole requirement, and it has to be solid rather than broad.

**HaLow phase:** images, data packages at full size, then voice, then possibly
video. A 1 MB package is 13.7 minutes of LoRa — a category difference, not a
tuning problem. Field duty-cycle approval is sought separately for a limited
voice and video trial; in the lab it all runs under the existing approval,
where gain is the only constraint.

---

## The sequence

| | | Contains | Gated on |
| --- | --- | --- | --- |
| **B** | *Membership and typed codecs* | Shipped. Close as is. | — |
| **C** | *Chat that survives a partition* | **Built.** Addressed chat on LXMF; room receipts suppressed at the endpoint | nothing |
| **D** | *Everything that does not fit one packet* | Tier 3 `Link`/`Resource` fragmentation; typed polyline codec for drawings; wire format for a blocked route edge | nothing |
| **E** | *The node knows where it is and what it can reach* | GNSS NMEA on the second UART; the relaying/boundary resolution; the ESP-NOW reset trigger; BLE proven as the endpoint's carrier | the two findings below |
| — | **Outdoor Test 1** | Range, disconnection, reconnection, with a mission executable at the far end | C + D + E |
| **F** | *The Reticulum ATAK plugin* | Delivery state, what is queued for whom, reachability and hops, fetch cost before spending it, propagation status, consent | Outdoor Test 1, and plugin know-how from the sibling repo |

C and D collapse into one PR cleanly if a single review is preferred. **E stays
separate regardless**, because it is not yet known whether it is a
configuration line or a fortnight.

### Why not one big PR

Items C through E are three unrelated areas touching different files, and one
of them is gated on a hardware investigation with no estimate. Bundling them
means one enormous review and a feature PR that cannot merge until a firmware
mystery is solved.

---

## What each PR closes

Gap identifiers match the standing record and `TAKNative.md`.

### PR C — chat that survives a partition

- **G1.** Chat has no delivery guarantee. The design calls tier 2 *must arrive*
  and names LXMF; the implementation is one bare `Packet(...).send()` at
  `tools/cot_bridge.py`, and the same on the Kotlin side. The typed codec made
  chat cheap and quietly moved it onto tier-1 transport. ATAK's delivery receipt
  is a *report* that something landed — it is not a retransmission mechanism,
  and ATAK never resends.
- **G4.** Nothing is queued for a peer out of range. `RRCBridge.cpp` already
  holds the primitive: a persisted per-room roster with a one-shot backfill
  flag. The CoT path does not use it. Chat frames already carry the author's
  send time, so a replayed backlog orders correctly.
- **G3.** Receipts are two-thirds of group chat. One line into a ten-person room
  costs 11.1 s of channel, of which 7.2 s is receipts carrying one bit each.
  Suppression sits where `forwardChat` already is.

Suppressing receipts creates the first state ATAK cannot render honestly. That
is not a side effect — it is PR F's opening brief, and it should be written
down as it appears.

#### What was built, 2026-09-12

A direct message goes by LXMF; a room line does not. That split is the one this
document already drew, and it is a property of broadcast rather than a
shortcut: a room has no member list to retry against and no receipt to wait
for.

**A direct message is also a real Columba message.** The frame rides upstream
LXMF's own payload pair — `FIELD_CUSTOM_TYPE` (0xFB) tags it `tak.chat.v1` and
`FIELD_CUSTOM_DATA` (0xFC) carries it — while the text travels as ordinary
content, so the same line lands in the recipient's Columba conversation and in
their ATAK. An operator who missed it in one can answer from the other. Room
traffic never becomes an LXMF message at all, so team chatter cannot bury
somebody's personal conversations.

Both flat, deliberately. The nested alternative under `FIELD_CUSTOM_META`
(0xFD) collides with the telemetry extras Columba already keeps there, and a
nested value has to be pre-shaped with backend-private helpers Columba's app
module cannot reach — so the Kotlin half could not have written the same shape.

**Proving it needed a check the round trip cannot make.** A direct message
arriving proves nothing: the bare-packet fallback delivers it too and looks
identical. `tak_team_acceptance.sh` asserts on the path taken, not just the
arrival, alongside a room receipt that must not arrive and a direct receipt
that must. Thirteen checks pass between two live bridges.

### First end-to-end run on hardware, 2026-09-12

Deck bridge to phone, Columba to ATAK, over a real link. Chat and four markers
rendered and delivered. Three faults came out of it that no bench test had
shown, all recorded in `CarriedIssues.md`:

- **ATAK's connection to the local endpoint flaps**, and everything written
  while it is down is silently lost. The rendering was correct the whole time
  and `-> ATAK: 637 bytes to 0 client(s)` was the only evidence. ATAK pings its
  server with `t-x-c-t`. Both endpoints now reply locally with `t-x-c-t-r`,
  serialize socket writes, and log disconnects and delivery loss. Regression
  tests cover the fix; the hardware stability run remains pending (issue 4).
- **A node that restarts is invisible for up to 30 minutes.** Both sides greet
  a *new* member, but a peer that rebooted is not new to anybody else, so
  nobody greets it and it can resolve no sender ids. `--announce-interval`
  shortens the window; the rule needs to change.
- **A direct message arrived headed by its recipient.** Fixed here — see below.

Still open, and honest about it: **a room line has no delivery guarantee and
PR C does not give it one.** Reliable multicast over a partitionable mesh needs
either an acknowledgement from every member or blind repetition. Neither is in
this PR, and the plan above never claimed otherwise.

### PR D — everything that does not fit one packet

- **G2.** There is no fragmentation anywhere in the CoT bridge; tier 3 was
  specified and never built. Anything over the 383 B MDU is refused outright,
  which is why **drawings do not cross at all** despite being on the LoRa
  requirement list.
- A typed polyline takes a drawing from 17 packets to 2 (6 311 B → ~584 B,
  5.26 s → 0.50 s, estimated from the corpus — there is no drawing fixture yet).
  It still needs fragmentation: 584 > 383.
- **The blocked-edge wire format belongs here.** Rubble reporting in the sibling
  repo is not a marker, it is an edge state change, and the format for "this
  edge is impassable" should be designed with the codecs rather than bolted on
  afterwards. See *The sibling repo* below.

Tier 3 is also the path images and data packages ride in the HaLow phase, so
this lays that foundation early rather than retrofitting it.

### PR E — the node knows where it is and what it can reach

- **GNSS.** `Position.h` already has the `GNSS` node-position kind; the NMEA
  reader on the second UART does not exist. `OnboardGNSS.md` deferred it as "not
  on the critical path for TAK" — that is no longer true. Outdoor Test 1 puts a
  stationary Rev 2 on a hill with no phone attached, which is exactly the
  unattended case the document reserved the module for.
- **The relaying boundary** and **the ESP-NOW reset trigger**, both below.
- **BLE as the endpoint's carrier.** The BLE peer protocol exists and is tested;
  it has not yet carried the CoT endpoint's traffic. The last bench run
  deliberately removed BLE to isolate the deck hop.

---

## Two findings that gate PR E

### 1. A board that is not in TNC mode does not relay

`RNode_Firmware.ino`:

```c
if (op_mode != MODE_TNC) {
  INFO("Not in TNC mode, transport will be disabled");
  reticulum.transport_enabled(false);
}
```

Such a board **accepts BLE peers, announces itself, is reachable as an endpoint,
and forwards nothing between its interfaces.** That is the reported symptom
exactly: the OZD board's panel showing two BLE peers (phone and Rev 1) while
nothing crosses to the main mesh.

The compiled default is `op_mode = MODE_HOST` (`Config.h`) while the
*provisioning* default is `MODE_TNC`. A board flashed but not fully provisioned
therefore lands in HOST and silently declines to relay, with every other
indicator looking healthy.

The BLE interface itself is not the suspect. It is a real `InterfaceImpl` with
`_IN` and `_OUT` true, and its own header records that packets crossed and
announces were relayed.

The MAC-ordering behaviour fits the same story: Rev 1 and the OZD board are both
Espressif (`0x80` prefix), a phone is `0xC0`–`0xFF`, so by
`shouldConnect = localMac < peerMac` the two boards pair with each other and the
phone always waits. The OZD board holding both is the expected shape — it simply
has to relay between them.

**Check before scheduling any work:** the NomadNet `/page/espnow.mu` page, line
`Relaying : yes/no`, on the OZD board and on Rev 1. If either reads `no`, that
is the whole explanation and it is configuration, not a defect.

### 2. ESP-NOW peer presence correlates with the watchdog resets

`CarriedIssues.md` §1 has had *cause found (TASK_WDT), trigger not isolated*
open since 2026-08-30, with resets at 240 s / 540 s / 2581 s / 11641 s —
irregular, which is what load-dependent starvation looks like.

Observed 2026-09-12: the OZD board averaged **~2 hours** with another OZD board
present as an ESP-NOW peer. With that peer unplugged it reached a day, and
**16 h 33 m** after a hand restart.

That is the first variable found that moves the reset interval. It belongs
**before** Outdoor Test 1, not inside it: a board that resets mid-test
invalidates every range and reconnection measurement taken, and there is no way
afterwards to tell which readings were poisoned.

---

## Later: an assistant in the loop

Raised 2026-09-12 and written up in `AssistantInTheLoop.md`: an operator asking
from inside chat what is around them, what a bearing is pointing at, what the
markers nearby are — and able to create markers and missions, with the model
configurable so volume can go to a cheaper one.

**Not scheduled, and deliberately last.** It is an interface onto the map and
building data rather than a foundation, so it cannot be better than that data;
built first it would sound confident and know nothing. It does, however, shape
two things already decided — chat as the transport it rides, and the
pull-for-one / push-for-the-area query API — so it is recorded now rather than
discovered later.

## The sibling repo — `urban-tak`, ATAK domain work, no Reticulum

`~/projects/urban-tak` owns the ATAK-side domain problem: urban navigation,
address lookup, rubble, and eventually building status. It has no Reticulum
dependency and **runs in parallel with PR C, starting immediately** — it is not
scheduled after this work.

It is a **public** repository. Nothing operational crosses into it: no fleet
secrets, node hashes, callsigns, IFAC passphrases, exercise coordinates or team
details, in any file or commit message.

**Why it starts now.** Nobody on this project has shipped an ATAK plugin, and PR
F is where that inexperience is most expensive: a bug there reads as "message
lost on a mesh" and costs days deciding whether it is the plugin or the radio.
The navigation plugin is the same skill on a problem with zero protocol
coupling, where a bug is "wrong route", visible instantly and debuggable at a
desk. It is also the only workstream not blocked on transport, and it carries
the longest lead time in the programme — an offline road graph and building
footprints for Istanbul is a data pipeline, and none of it depends on us.

The requirement, as stated: Istanbul, urban theatre, earthquake response.

1. **Road navigation.** Bloodhound gives straight-line bearing, which is useless
   between buildings. The one decision that matters on day one: **the router
   must accept dynamic edge penalties at runtime.** An engine that cannot makes
   item 3 unbuildable — rubble would decorate the map without changing a route.
   That constraint picks the engine, not benchmarks.
2. **Address lookup as a marker tool.** A non-meshed report names a street
   address. Command post staff pasting coordinates from a consumer map is slow
   and error-prone. Needs an offline geocoder over the same extract; Turkish
   addressing (mahalle / sokak / kapı no) is real work, not an import.
3. **Rubble reporting.** Not a marker — an edge state change the router
   consumes and the mesh replicates. **This is the one item that spans both
   repositories:** `urban-tak` owns the model and the routing response, this
   repo owns getting it to everyone, and the wire format is PR D's.

   **PR D has a dependency on that repo, not the other way round.** The stable
   edge identity — how a blocked edge is named so that two devices holding
   different builds of the road graph agree, and so a report survives a graph
   rebuild from a newer extract — is owed to this repo before the encoding is
   designed. It is recorded as a debt in `urban-tak/docs/MeshContract.md`. Ask
   for it rather than inventing one; an identifier chosen here that the router
   cannot resolve is worse than no identifier.

**Longer term:** clicking a building or road to report and query status, against
government feed data, with Reticulum as the query API over NomadNet.

The shape question worth settling before that API exists: an individual building
query is cheap — roughly 40 B out, 60 B back, about a second round trip at four
hops. "Status for every building on this block" is a different animal and wants
a pushed delta for the area. **Pull for the one building an operator tapped;
push for the neighbourhood they are standing in.**

---

## Standing constraints these plans assume

- Approved lab conditions: **no duty-cycle limiting**. Gain (21) is the only
  legal constraint. Field approval for voice and video is sought separately.
- Disaster-first: weigh re-meshing speed against airtime, and never inherit
  protocol politeness by default.
- Scope: microReticulum, its library, Columba, and the sibling ATAK repo. Other
  people's projects are not ours to fix.
- Push to `origin` (DrLexus11) only. `attermann` and `upstream` are
  push-disabled deliberately.
- Fleet secrets and IFAC passphrases are prompted on the terminal, never passed
  as command-line arguments.
