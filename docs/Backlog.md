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

### 7. Bluedroid reassembly is single-buffered

The new NimBLE backend resolves this for the OZD fixture: it has a bounded
per-connection identity, sequence and reassembly slot, defaults to seven links,
continues advertising below that capacity, and fans packets across the shared
Reticulum interface.

The proven Bluedroid backend used by the RAD targets still has one inbound
reassembly buffer. Extending it needs a separate hardware acceptance pass so the
OZD work does not perturb that stable implementation.

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

---

## From the Columba multi-peer investigation (2026-09-02)

Measured against a Samsung A54 running Columba, with adb logcat on the phone
and serial on the boards. Several earlier drafts of this section were wrong;
what follows is what the two logs actually show, side by side.

### 12. The 1 s post-handshake teardown was ours, and is fixed

Columba connected to the Ozdisan board, completed the whole handshake, and
closed the link almost exactly one second later, repeatably:

```
connected -> CCCD 0x0001 -> identity accepted -> 1.0 s -> CCCD 0x0000 -> disconnect (531)
```

`531` is `0x200 + 0x13`, remote-user-terminated: a deliberate close. The phone
log names the reason:

```
Connecting to 40:91:51:9B:2D:D2...
Deduplication: disconnecting central connection to 40:91:51:9B:2D:D2
```

`resolveDualConnectionAction()` is reached only when ONE address holds both a
central and a peripheral role. It held both because this board was dialling the
phone at the same moment the phone was dialling it. Columba resolved the
collision and closed a link -- and the one it closed was the working one.

**Cause: we dialled a peer that was already dialling us.** Not NimBLE, not the
peer protocol, and not Columba.

**Fix, in `adopt_stable_high_address()`:** take a static random address (top two
bits set) derived from the factory MAC. That sorts above any phone address, so
`should_connect()` declines and we wait. It puts this board in the position
Rev 1 already occupies by accident of numbering -- see item 13.

### 13. The MAC-based connect election does not work against Android

`KotlinBLEBridge.shouldConnect()` compares `bluetoothAdapter?.address`. Android
returns `02:00:00:00:00:00` to unprivileged apps -- Columba's own test file
names that value -- and the method gives it no special handling. It parses to a
value below every real address, so **a phone always elects itself central**.

The election therefore cannot prevent collisions: it only avoids them when the
node's own address happens to sort high. Rev 1's Espressif public address
`80:b5:4e:..` does, so it never dials, only one link is ever made, dedup returns
`NONE`, and its Columba sessions run for hours. The Ozdisan board's
`40:91:51:..` does not, so it dialled -- which is the whole of item 12.

Note also that Columba's two rules disagree: connect elects by MAC, dedup
resolves by Reticulum identity (`keeping peripheral (local=1e100317.. >
peer=0056bb6a..)`). Any future node must not rely on the MAC rule to keep it out
of the collision path; sorting above the peer is what actually does that.

### 14. Rotating our BLE address is harmful -- do not reintroduce it

Rotating made Columba fire `onDeviceDiscovered` again, which looked like a fix
for item 15. It is not: **Columba's candidate list never prunes.** Measured, one
board presenting as four addresses:

```
Discovered new device: 40:91:51:9B:2D:D2 (OZD-ARD-01)
Discovered new device: F0:69:4E:F4:04:B2 (OZD-ARD-01)
Discovered new device: C3:E9:E0:F4:30:D4 (OZD-ARD-01)
Discovered new device: ED:08:F5:41:E9:A1 (OZD-ARD-01)

selected 7 peers to connect from  9 candidates
selected 7 peers to connect from 10 candidates
selected 7 peers to connect from 11 candidates
selected 7 peers to connect from 12 candidates
```

Every rotation adds a permanent phantom peer against a cap of seven, so the
ghosts consume exactly the budget multi-peer operation needs. Removed. The
address must be stable; item 12's fix derives it from the MAC for that reason.

### 15. Columba offers an address to its driver only once

`BleScanner.kt` fires `onDeviceDiscovered` only when the address is absent from
its map; otherwise it updates RSSI silently. That callback is the only path to
the Python driver, and the driver is the only caller of `connect()`.

Entries are evicted when a connection *ends* (`Removed device from cache ...
will be rediscovered`), so this is harmless for a node that has connected at
least once. It is not harmless for a node first seen while unconnectable: with
the phone touching the board at -38 dBm, 85 s of logcat showed only
`Updated device`, never a discovery, and no connection was ever attempted. Only
a force-stop of the app cleared it.

**Proper fix is client-side:** evict entries not seen recently, or re-fire on
rediscovery while unconnected. There is no sound firmware-side workaround --
rotating the address is item 14.

### 16. Our outbound dial to a phone times out (BLE_HS_ETIMEOUT, reason 13) -- OPEN

Every central attempt from the Ozdisan board to the phone failed after exactly
the 10 s connect timeout, at every signal level tested, including -53 dBm with
the two devices touching:

```
[blepeer] discovered 8c:6a:3b:82:0e:6c (rssi=-53); connecting as central
[blepeer] central connect to 8c:6a:3b:82:0e:6c failed, reason=13; retry in 10000ms
```

Not signal, and not the peer's health: a BlueZ central on the deck connects to
that same phone and reads its 16-byte identity without trouble.

After item 12's fix this board no longer dials phones, so it does not block
Columba. It still matters for **board-to-board BLE peering**, where one side
must be the central.

**Candidates, untested:**
- the phone is simultaneously scanning and initiating, and its controller does
  not accept an inbound connection in that window;
- `8c:6a:3b:82:0e:6c` is stale by the time the dial runs -- it is the address
  this board sees, and the deck never observes it, so the two may be looking at
  different advertising sets;
- Wi-Fi and ESP-NOW coexistence starving the initiator on a single radio.

**How to tell them apart:** log the advertisement age at dial time; try a dial
with ESP-NOW stopped; and compare against a dial to a second RAD rather than a
phone, which removes Android from the question entirely.

### 17. A Rev 1 serving BLE peers cannot also run ESP-NOW

The peer interface costs roughly 15 KB. `impr-rad01-rev1-portable` unflags
`ESPNOW_TRANSPORT`, `UDP_TRANSPORT` and `TCP_SERVER_TRANSPORT` -- that is how it
affords it, not product segmentation. Measured: adding `-DBLE_PEER_TRANSPORT` to
the full `impr-rad01-rev1` build took free heap from ~30 KB to ~16 KB and turned
a board with 16,000 s of uptime into one taking a task watchdog every ~180 s.
The portable build runs at ~35 KB and holds.

Consequence for the planned chain: "the Ozdisan board reaches the mesh through
Rev 1's ESP-NOW" and "Rev 1 also serves Columba over BLE" cannot both be true on
current Rev 1 hardware. Decide which role Rev 1 plays before building on it.

### 18. RRC client integration is assessed but deferred

Bringing Eridanus's RRC client into Columba was assessed on 2026-09-03 and is
recorded in [`EridanusRRCIntegration.md`](EridanusRRCIntegration.md): identical
toolchains, shared module architecture, no RRC code in Columba to conflict with,
and screens that already implement the intended hub/room/chat flow. The one real
gap is that Columba exposes no generic Resource primitive.

Deferred deliberately. This branch is scoped to the BLE peer interface and
complete meshing over ESP-NOW.
