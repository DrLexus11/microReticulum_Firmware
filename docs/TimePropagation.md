# Time Propagation: a credible source, spread peer to peer

How a disaster mesh gets trustworthy UTC onto every node without infrastructure,
without a per-identity allow list, and without trusting the nodes in between.

This is the prerequisite for TAK (`TAKCapability.md`) and for message expiry. The
clock plumbing is already built and shipping on `feature/wall-time` -- a separate
wall clock that never disturbs Transport's timers, persistence, provenance, and
the safety rules. What is missing is **distribution**.

Status: **steps 1-5 implemented and hardware-verified**, 2026-09-05.
Written 2026-09-04 from what the bench taught us; the results below are
measured, not projected.

---

## What today's implementation gets right, and where it stops

Right: `/time` on Transport's remote-management destination rides an
authenticated Reticulum Link, so it already works over LoRa, ESP-NOW, BLE, UDP
or TCP without knowing which. The safety rules are enforced in the library, not
the caller -- never backwards, bounded forward step, provenance recorded,
`unknown` expressible as zero rather than uptime.

Where it stops: **a human has to run a CLI at each node**, and the gate is a
per-identity allow list. Neither survives fifty apartments.

Two bench results shape everything below.

**An empty allow list denies everyone, silently.** OZD-02 could not be given
time at all until its build was changed, and the refusal happens before the
handler runs, so it is indistinguishable from a node being switched off.
Requiring every node to allow-list every other node does not scale, and it fails
closed in exactly the situation where time matters most.

**A restored clock degrades without bound and still reports "known".** OZD-02
was measured 48 minutes behind Rev 1 after a night on `persisted`. Each reboot
loses up to the persist interval plus the powered-off time, and nothing ever
corrects it. A confidently wrong clock is worse than an honest unknown, because
expiry and CoT staleness both act on it silently.

---

## The idea: make the assertion self-authenticating

The instinct is to build a chain of trust hop by hop -- A trusts B, B trusts C.
Resist it. It scales badly, it fails closed, and every relay becomes a place to
lie.

Instead, **sign the time itself**. An authority signs a small assertion; any node
can carry it; the receiver verifies the signature against a pre-provisioned
authority key. A relay that has been captured can drop or delay an assertion but
cannot forge one, so **no trust is needed in the nodes between you and the
source**. That is what makes free peer-to-peer propagation safe.

```
TimeAssertion            ~72 bytes, one LoRa packet, one ESP-NOW fragment
  unix_ms      u64       the authority's UTC at signing
  nonce        u64       echoed from a solicit, or 0 for a broadcast
  valid_for_s  u32       how long this may be treated as fresh
  stratum      u8        1 = GNSS or internet NTP; n+1 = learned at n
  source       u8        GNSS / NTP / operator
  authority    16 bytes  which key signed this
  signature    Ed25519   over all of the above
```

Reticulum already gives us the primitives: `Identity` is Ed25519, signing and
verification are in the library, and the wire encoding is msgpack as everywhere
else.

## The hard part is replay, not forgery

A signed timestamp does not prove *current* time. An attacker can rebroadcast a
year-old assertion, perfectly valid, perfectly signed. **A node with no clock
cannot tell the difference** -- that is the whole difficulty, and any design that
skips it is broken.

So there are two modes, and they need different defences.

### Bootstrap — the node has no time

It cannot judge freshness, so freshness must be proved to it. The node picks a
random nonce and solicits; the assertion it accepts must carry that nonce back
inside the signature. A replayed assertion has the wrong nonce and is rejected.
This is Roughtime's construction, and it is the only honest way to bootstrap.

The solicit is routed over the mesh like any other request -- through as many
untrusted hops as necessary, because only the authority's signature matters.
Cost is one small round trip; seconds over LoRa, which is nothing for something
a node does once per boot.

### Maintenance — the node already has time

Now freshness is cheap, because the node has a monotonic clock and the existing
rule already defeats replay: **time never goes backwards.** An old assertion
proposes a time earlier than the node's own and is refused by the rule that is
already implemented and already fired correctly on the bench (`result:
backwards`, on a value 235 ms stale).

So maintenance needs no round trip at all. An authority announces a signed
assertion periodically; anyone may relay it; nodes adopt it only if it is
forward, within the bounded step, and from a better stratum. At ~72 bytes on a
30-minute cadence this is free even inside a 1% duty cycle.

## Trust anchors: what "credible" means

The authority public keys are provisioned onto boards the way the IFAC
passphrase already is -- at deployment, out of band, as a small set rather than
one. Several authorities means losing one does not orphan the mesh, and a node
accepts the best stratum among those it can verify.

**IFAC membership is not sufficient on its own** for originating time, though it
remains the admission control for the network. A member can relay; only an
authority can originate. That split is the point: it lets us drop the
per-identity allow list from the time path entirely without lowering the bar for
what may set a clock.

Where authorities run: **Vox**. A Pi with NTP when any backhaul exists and a
GNSS receiver when it does not is the natural stratum-1 source, one per
building. Until Vox exists, the deck and any Wi-Fi-connected RAD can hold an
authority key.

## Say how good the clock is, not just what it says

Every consumer -- LXMF expiry, CoT staleness, stamp validation -- needs to know
how much to believe. So expose alongside the time:

- **stratum** — hops from a real reference
- **age since last verification** — not since last *adoption*. Today's `Sync
  age` reports the latter, so Rev 1 showed 11 hours while its NTP was working
  perfectly: corrections under the 1000 ms threshold are skipped without
  recording that a check happened and agreed. A healthy node and a stale one
  look identical, which makes the field worse than absent.
- **a confidence bound** — roughly stratum plus elapsed drift allowance, so a
  caller can ask "is this good to a second, a minute, or not at all?"

And keep `unknown` expressible. A node that cannot verify time must say so
rather than serving a plausible number.

---

## Order of work

1. **Stratum, verification age, and confidence.** No new protocol; fixes the
   misleading `Sync age` and makes `persisted` visibly stale instead of
   silently wrong. Small, and it retires the two bench findings above.
2. **Assertion format, signing and verification** in microReticulum, plus a
   provisioning field for authority public keys. Self-contained and testable
   natively.
3. **Nonce-challenged solicit** on the remote-management destination, gated on
   signature rather than the allow list. This is what finally makes a node
   without a clock able to get one unattended.
4. **Periodic signed announce and relay.** Cheap, needs no trust in relays, and
   turns time into a property of being near an authority.
5. **Authority daemon** on Vox and the deck: NTP or GNSS in, signed assertions
   out. Off-device, ordinary Python. `tools/time_authority.py`.

Steps 1 and 2 are worth doing regardless of TAK. Step 3 is the one that removes
the manual CLI. Nothing here needs a hardware change: an RTC would only shorten
the bootstrap window, and it cannot originate time.

## What this deliberately does not do

- **No hop-by-hop trust chain.** Relays are untrusted by construction.
- **No consensus.** Nodes take the best verifiable stratum; they do not vote.
  Byzantine agreement over a duty-cycled radio is not a good trade.
- **No clock discipline or slewing.** We step forward under bounds. Sub-second
  accuracy is not required by anything we serve; minutes are the scale that
  matters for expiry and staleness.

---

## What steps 4 and 5 turned out to be

**The carrier is an announce, and that was the whole design.** An authority owns
a destination on `rnstransport.time.assertion` and announces it with the
assertion as app data. Reticulum's existing machinery then does distribution,
hop counting and deduplication, and every relay in between is untrusted by
construction. No flooding protocol of our own, no relay state, no new packet
type.

Two consequences worth stating, because they simplify the format the design
sketched above:

- **No second signature layer is needed on this carrier.** A Reticulum announce
  signs `destination_hash || public_key || name_hash || random_hash || ratchet
  || app_data`, and Transport validates it before a handler ever sees it. The
  assertion still carries its own Ed25519 signature over a domain-separated,
  fixed-width message, because that is what makes it portable to a carrier that
  is not an announce -- but on this one it is belt and braces, not the only
  belt.
- **No `authority` field on the wire.** The announce cryptographically binds the
  identity that signed it, so a field naming the authority could only ever
  agree or disagree with something already proven. It goes back in the day an
  assertion travels over something other than an announce.

### A library defect this exposed

microReticulum only calls `Identity::remember()` for a destination it does not
already know -- a deliberate divergence from Python, to spare the flash -- and
announce handlers were being handed `Identity::recall_app_data()`. So a handler
saw whatever the **first** announce from a destination ever carried, and never
anything after it. Time beacons would have delivered one timestamp forever:
a permanent replay, built in.

The same defect means a node that changes its name is invisible to every peer
that already knew it, which is worth knowing independently of time.

Fixed by handing handlers the app data parsed from the announce in front of
them (`Identity::announce_app_data`), falling back to the cache. The storage
policy is untouched, so the flash-wear reasoning still holds.

### Bench results, 2026-09-05

One deck authority at stratum 1 (`tools/time_authority.py`, NTP), one relay
(Rev 1, Wi-Fi + LoRa), one receiver (IMPR-RAD-01-REV2-2, LoRa only, no Wi-Fi
credentials, two hops from the deck).

| what was proven | evidence |
|---|---|
| solicited path works unattended on a fresh node | `[timesync] adopted UTC 1788592170500 ms from peer at stratum 1 (now 2)` |
| assertions cross an untrusted relay | `Heard: 3  Verified: 3` on a node with no route of its own to the authority |
| agreement refreshes confidence without adopting | `Verified: 34 s ago` while `Adopted: 0` |
| a listen-only node with only a restored clock corrects itself | `[timebeacon] adopted UTC 1788592757819 ms from authority <5b197a1c413f8ab9> at stratum 1`, recovering 5m28s of drift with **no time peer configured and no link opened** |
| provenance stays honest | the page reports `Source: signed-beacon`, never `ntp` |
| an unprovisioned signer is refused | `Refused: 1 not an authority` after emitting from a fresh identity |

That fourth row is the one that matters. It is the OZD-01 case: a node that is
unreachable, that no console and no link can get to, and whose OLED shows an
uptime counter where a clock should be. It can now be told the time by
something it merely overhears, and cannot be lied to by whatever carried it.

### What is still not solved

A node with **no** clock at all still cannot distinguish a live assertion from
a recording -- only the nonce challenge can, and that needs a link. Such a node
bootstraps to "probably right" and says so. Anti-rollback keeps a replay from
ever moving it backwards, and the first real assertion after that fixes it.
