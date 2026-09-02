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

### 6. Library pin — **resolved**

Outbound sync depends on a fix in `Link::receive` so a response of any msgpack
type is decoded, not only `bin`. That is `b06ab0b` in the fork
(`DrLexus11/microReticulum`), merged to `master` via PR #2 as `d22d441`.

`platformio.ini` pins `d22d441` — the fork's mainline, not a PR branch, so no
squash or branch deletion can strip it later.

Verified rather than assumed: the cached dependency was deleted so PlatformIO
refetched from scratch, installing `microReticulum@0.5.0+sha.d22d441`. The fix is
present in the fetched source, the bin-only decode is absent, and all three
environments build from that cold fetch.

The merge also brought `180c31a`, which touches provisioning persistence and
reboot handling — the channel this project uses for every hardware measurement.
Checked on hardware after the pin bump: provisioning answers on ns108 and ns115,
persisted configuration survived the reflash (the static peer hash was still
set), and the node ran clean with no resets.

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

---

## From the ESP-NOW / BLE peer fixture (this branch)

### 9. `sample_loop_stack()` only runs while the radio is online

`sample_loop_stack()` is called from inside `if (radio_online)` in `loop()`
([`RNode_Firmware.ino`](../RNode_Firmware.ino)). Two consequences:

- On a target with no radio fitted — `ozdisan-esp32-espnow` — it never runs at
  all, and `loop_stack_free_min` keeps its `0xFFFFFFFF` sentinel. If served, the
  metric would read as 4294967295 bytes of free stack. The OZD compact profile
  omits ns108 entirely for heap headroom, so it does not expose this bad value.
- On the RAD boards it does run, but only samples passes where the radio was
  up, so the recorded minimum is not the worst case across every pass, which is
  what its comment claims it is.

**To fix:** move the call out of the `radio_online` block, next to the other
per-pass instrumentation. One line, and it makes the metric mean what it says.
Deliberately not done here: it changes a shared code path on the RAD boards,
and this branch is scoped to the ESP-NOW and BLE fixture.

### 10. The deck-side BLE peer client negotiates only a 23-byte MTU

[`tools/ble/BLEPeerClientInterface.py`](../tools/ble/BLEPeerClientInterface.py)
connects at BlueZ's default ATT MTU. `_acquire_mtu()` is attempted and does not
take effect, so every Reticulum packet is split into 15-byte fragments: a
544-byte NomadNet page becomes 37 writes. It works — the page fetch that proves
the whole chain was carried this way — but it is far slower than the link
allows, and it is not what Columba does. Android requests 517.

**To fix:** drive the MTU exchange explicitly on the BlueZ backend, or connect
through a bleak version where `_acquire_mtu()` is effective. Firmware side needs
nothing: `NimBLEDevice::setMTU(BLE_PEER_MAX_ATTR)` already asks for 512, and the
central is the side that decides.

### 11. Rev 1's watchdog breadcrumb reports a phase that does not exist

Rev 1 reports `PROV_METRICS_DEV_WDTPHASE = 18` after a `TASK_WDT` reset, and
`LOOP_PHASE_COUNT` is 15. Nothing in `loop_phase()` can write 18, so either the
board is running an image that is not built from this tree, or the RTC
breadcrumb is not surviving the reset intact — in which case every phase it has
ever named is suspect.

**Not investigated here**, deliberately: the watchdog itself is scheduled
separately. Recorded because the breadcrumb is the instrument that work will
depend on, and an instrument that reports impossible values has to be checked
before it is trusted. See [`CarriedIssues.md`](CarriedIssues.md) §1.

### 12. LittleFS hashlist rotation can unlink a segment with an open descriptor

During the 600-second OZD no-metrics soak, LittleFS twice reported:

```
Failed to unlink path "./hashlist_store/seg1.dat". Has open FD.
```

The node continued running and did not reset, so this is separate from the
device-metrics watchdog failure. Check the microStore hashlist segment
close/rotation ordering before treating the warning as harmless; a failed
unlink can leave stale storage behind even when runtime traffic continues.
