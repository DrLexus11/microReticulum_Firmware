# Backlog

Deferred items, recorded so a later merge inherits the reasoning rather than
rediscovering it. Each says what is known, what was decided, and what would
change the decision.

Topic-scoped backlogs that predate this file and are still live:

- [`BridgeBacklog.md`](BridgeBacklog.md) — RRC-to-LXMF bridge
- [`IdentityResolutionBacklog.md`](IdentityResolutionBacklog.md) — NomadNet peers
  that cannot resolve each other
- [`CarriedIssues.md`](CarriedIssues.md) — faults that outlive a branch

---

## From propagation-node peering (roadmap 4a)

### 1. The peering cost is assumed, not read from the peer

We generate the peering key at `LXMF_PN_PEERING_COST` (18), which is our own
advertised cost and happens to match lxmd's default. The peer's real cost is in
its announce app_data, which we do not parse.

A peer demanding a *higher* cost rejects every offer with `ERROR_INVALID_KEY`
(0xf3), and the symptom is indistinguishable from sending no key at all. A peer
demanding lower costs us wasted work, which is harmless.

**To fix:** parse the announce app_data (element 5, `[stamp_cost, flexibility,
peering_cost]`) and generate against that. The announce is already received; only
the parsing is missing. Worth doing before peering with anything other than a
default lxmd.

### 2. We do not validate an inbound peering key

`lxmf_offer_request()` accepts any offer without checking the key. This is
deliberate: admission on our side is the store share, which is a guarantee about
our own storage rather than a claim about the peer, and it holds whatever the
peer's backlog looks like. LXMF peers do not require us to challenge them.

**What would change it:** wanting to rate-limit or exclude unknown nodes rather
than merely bound what they can occupy. Note validation costs a 6400-byte
workblock plus a hash per offer, per peer.

### 3. The wanted-count bound is pessimistic

An offer asks for at most `sync_limit / per_message_limit` messages — 2 at the
current 8 KB / 4 KB settings — because it must assume every message is maximum
size. Real messages are far smaller, so a 128-message backlog takes 64 rounds.

**To fix:** raise `LXMF_PN_SYNC_LIMIT_KB`, which is the lever. Asking for more
than the sync limit is not an option: `lxmf_resource_started()` would then refuse
the transfer it just invited.

### 4. Announce-based peer discovery is unreliable in this port

`Transport::inbound` dispatches announce handlers only inside the `should_add`
branch — a repeat announce from a path already known never reaches a handler, and
`PATH_RESPONSE` is skipped entirely. A node that learns a peer's path by
requesting it, which is what happens when it first delivers a message there,
silently consumes its only discovery opportunity. Measured: one announce of any
aspect reached a handler in 3.5 minutes, and none was a propagation announce.

Static peer configuration (ns115 field 1) is therefore the mechanism, with
announce discovery as an opportunistic extra. lxmd has static peers for the same
reason.

**To fix properly:** an upstream change so handlers see announces that do not
update the path table. Until then, configure peers.

### 5. Peer sync has no runtime control

Interval, hop depth and burst size are compile-time constants. There is no
provisioning field to disable peering or retune it on a deployed node — only the
static peer hash is settable.

**Worth adding when** a deployment needs to turn sync off without reflashing, or
to widen the interval on a duty-cycle-constrained link.

### 6. The pin follows a fork branch with an open PR — **resolved, with a caveat**

Outbound sync depends on a fix in `Link::receive` so a response of any msgpack
type is decoded, not only `bin`. That is `b06ab0b` in the fork
(`DrLexus11/microReticulum`), on `fix/provisioning-persistence-errors`, and also
the head of the fork's **PR #2**.

`platformio.ini` now pins `b06ab0b` in all 29 places, verified rather than
assumed: the cached dependency was deleted so PlatformIO refetched from scratch,
installing `microReticulum@0.5.0+sha.b06ab0b`. The fix is present in the fetched
source, the bin-only decode is absent, and all three environments build from that
cold fetch.

**The caveat:** this pins a commit on a PR branch, not on the fork's mainline. If
PR #2 is squashed or rebased on merge, `b06ab0b` becomes unreachable and every
build here fails at dependency resolution — loudly, at least, rather than
silently. Re-pin to the merged commit at that point.

---

## From the BLE peer interface (PR #14/#15)

### 7. Reassembly is single-buffered, so one peer at a time

`BLEPeerInterface` holds one inbound reassembly buffer with no per-peer keying.
Fine for one phone, which is the deployment today. A second concurrent peer would
interleave fragments into one buffer and corrupt both streams.

**To fix:** key reassembly by peer identity, which the interface already learns
from the handshake.

### 8. `paths=0/2000` reads the wrong container

The path-count metric reports zero on a node that demonstrably routes, so it is
reading something other than the live path table. It has misled at least one
diagnosis already.

**To fix or drop.** A metric that is confidently wrong is worse than no metric.
