# Unresolvable Peer Identities in NomadNet

Reported 2026-08-28. Two NomadNet users in an RRC room on the Rev 1 hub
`d36d1371...` could not start a direct conversation or open each other's pages:
the client warned that the peer's identity had to be queried first, the query was
made, and no answer arrived. The same warning appears under node-hosted NomadNet
sites. Columba resolves this with a fingerprint button; NomadNet appears to have
no equivalent that works.

Status: **backlog item, not yet diagnosed.** What follows is what has been ruled
out and what to test first, so the investigation does not start from zero.

---

## 1. Why an identity is needed at all

A Reticulum destination hash is derived from a public key but does not contain
it. To *encrypt* to a destination -- an LXMF message, a page request -- a node
must hold the peer's public key. Keys are learned from **announces**, and only
from announces.

This is why the RRC room does not help. The hub authenticates each member and
stamps a 16-byte identity hash on their messages, so the peer's name and hash
are visible in the room. None of that conveys a public key. Seeing someone in a
room and being able to message them are different problems, and the room solves
only the first.

## 2. What has been ruled out

**The firmware serves identities correctly.** microReticulum answers a path
request for a known destination by rebroadcasting the cached announce packet
from its destination table (`Transport.cpp`, `path_request()` ->
`destination_entry.announce_packet()`), and that packet carries the public key.

This is not theory; it is exercised on every acceptance run. `tools/rrc/probe.py`
calls `RNS.Identity.recall()` from an isolated Reticulum instance whose only
interface is a TCP client to a RAD, and it resolves -- including at **two hops**,
where the identity is served across the LoRa link from the far board. If a RAD
could not serve identities, none of the RRC acceptance would pass.

So the RADs are not the obvious suspect. Something upstream of that is failing.

## 3. First hypotheses, in order

1. **The peer never announced where the asker could hear it.** A path request
   can only be answered by a node that already holds the announce. If the peer's
   `lxmf.delivery` destination has not announced since the asker's instance
   joined -- or the announce did not propagate the full path -- there is nothing
   cached to serve, and the request is answered by silence rather than by an
   error. Silence is indistinguishable from a broken feature, which is exactly
   how this was reported.
   **Test:** have the peer announce explicitly from NomadNet, then retry
   immediately. If it works, this is the whole story and the fix is a product
   one, not a protocol one.

2. **The asker's client does not actually issue, or does not await, the path
   request.** The reporter notes Columba has a control that works and NomadNet
   has none that does. That points at a client gap rather than a network one.
   **Test:** from a plain Python instance on the same path, call
   `RNS.Transport.request_path(peer_hash)`, wait, and check
   `RNS.Identity.recall(peer_hash)`. If Python resolves what NomadNet cannot,
   the fault is in the client and our stack is clear.

3. **Announce propagation is being lost at a hop.** Our path and announce tables
   are bounded (`RNS_PATH_TABLE_SEGMENT_COUNT`, `URTN_PATH_TABLE_MAX_RECS`), and
   a Tailscale-attached client adds hops. If entries are being evicted, an
   announce may be cached briefly and gone by the time it is asked for.
   **Test:** compare a RAD's destination-table occupancy against its caps while
   the peers are attached.

## 4. Why this matters more than it looks

Every symptom in the report is **silent**. The client warns, the query goes out,
nothing comes back, and the user is left with a yellow banner and no way
forward. There is no error to search for and no counter that moves. This project
has lost the most time to exactly that shape of failure -- the unwritten
firmware hash, the roomless `/list`, the stack overflow that presented as a
malloc panic -- and the lesson each time was the same: make the component report
what it saw.

Whatever the cause turns out to be, the fix should include something that says
*why* no identity arrived: no cached announce, no path, or request never sent.

## 5. Practical workaround meanwhile

Have the peer announce from their client before the first contact, and keep
node announces frequent enough that a newly attached client learns the
neighbourhood quickly. Our nodes announce NomadNet every 5 minutes and the LXMF
propagation destination on boot plus every 30 -- which is fine for nodes and
says nothing about *client* destinations, which announce on their own schedule.
