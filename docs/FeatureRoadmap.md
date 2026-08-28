# Firmware Feature Roadmap

Cross-feature priority for work that remains after the propagation-node and
private-mesh milestones. Detailed design and acceptance evidence remain in each
feature document; this file answers which implementation should happen next.

Status: **current recommendation**, updated 2026-08-27 after RRC PR 3 hardware
acceptance, the Bluetooth overhaul proposal and the flash-headroom measurements. The protocol core and embedded hub are merged. The reusable probe,
Rev2 promotion, two-hop automated run and stock NomadNet/Eridanus exchange are
green on PR 3. Automatic disaster SoftAP is next after this PR merges.

## Completed foundations

1. Inbound TCP Reticulum client attachment and two-board routing.
2. ESP32 LXMF propagation node with stock Columba interoperability.
3. Python-compatible LoRa IFAC.
4. TCP/UDP IFAC and recoverable secure-node posture.
5. Provisioning schema, transactional configuration and remote-management
   authorisation.

These are foundations for the remaining features rather than competing backlog
items.

## Recommended implementation order

### 1. Embedded RRC group chat — **active; PR 3 acceptance green**

Implement the scoped live group-chat hub in
[`docs/RRCRequirements.md`](RRCRequirements.md).

Why first:

- it is the requested product-facing MVP;
- NomadNet and Eridanus already provide desktop and Android clients;
- Reticulum Links and the mixed TCP/LoRa transport path already exist;
- it reuses the just-completed private-mesh controls; and
- it exercises long-lived Links, which are foundational for later interactive
  services.

Deliver it as the three PRs defined in the requirements document. PR 1 and PR 2
are merged. PR 3 adds the reusable probe, promotes Rev2 to the same hub build,
and has passed automated and stock NomadNet/Eridanus mixed-board acceptance. Do
not expand it into history, Resources, moderation or a Columba fork during the
MVP.

### 2. Automatic disaster SoftAP — **implemented and accepted; PSK policy outstanding**

**Correction, 2026-08-27.** This item claimed the fallback was unimplemented. It
is not: `Remote.h` has carried the state machine for some time --
`wifi_build_ap_ssid()`, `wifi_remote_start_ap_fallback()`, the station-failure
timer, the retry window and the bounded deferral while clients are associated.
The roadmap simply was not updated when it landed, and the stale line was read
as a plan. Believing a roadmap over the source is how work gets scheduled twice.

**Update, 2026-08-28.** Two of the three remaining items are now done, and this
entry has been corrected a second time rather than left to go stale again:

1. ~~A provisioning surface.~~ **Done.** The three timers are live-apply fields
   in `PROV_NS_NETWORK`, in seconds, alongside read-only AP state. Verified by
   writing them on hardware without a reboot, and by an out-of-range write being
   refused rather than clamped.
2. ~~Deliberate acceptance.~~ **Done.** Automatic fallback on station failure,
   the AP observed from a host's own WiFi scan, LoRa working alongside it, and
   the return path in both forms -- deferring while a client is attached, then
   overriding that deferral at the ceiling. Evidence in
   [`docs/MeshResilience.md`](MeshResilience.md).
3. **A per-node PSK policy** -- still outstanding. The MAC-derived key is a
   speed bump, not a credential; see `docs/PrivateMesh.md` §6a. Secure-node
   interaction is also still untested.

Why it was placed second:

- it makes RRC, LXMF and NomadNet usable from phones after the local router has
  failed;
- it is directly valuable to the disaster-communications product;
- it builds on working TCP attachment instead of adding a new mesh transport;
  and
- it is smaller and less speculative than the Resource API or peer sync.

Acceptance must cover AP/STA transitions, randomized recovery timing, client
reconnection, secure-node interaction and coexistence with LoRa. The fallback
MAC-derived PSK is not a true credential; secure deployments need an explicit
per-node policy.

### 3. Flash headroom: repartition and low-hanging trims — **before the BLE work**

Reclaim application space per [`docs/FlashHeadroom.md`](FlashHeadroom.md).

Why before Bluetooth:

- the boards carry 8 MB of flash and the partition table maps only 4 MB, so the
  application is squeezed into 2 MB while roughly half the chip is unused and
  the filesystem beside it sits 83% empty;
- `impr-rad01-rev2` is already at **90.2%** of its partition, which is tight
  enough that the next feature of any size does not fit;
- the BLE overhaul needs somewhere to land, and doing the partition first means
  it is not competing for the last 10%; and
- `RAD01_NO_BLE` does not currently compile, and repairing it is the only way to
  measure what the Bluetooth stack actually costs -- so the BLE work's own
  business case depends on this item.

Scope, in order:

1. A custom 8 MB partition table giving 3-4 MB application and 3-4 MB
   filesystem. Acceptance must cover the migration: LittleFS is erased, so
   provisioning configuration and the LXMF store need a backup and restore
   path, while NVS stays at `0x9000` and the device identity survives.
2. Fix `RAD01_NO_BLE` by moving the device name out from behind the Bluetooth
   guard, then record the measured size of a Bluetooth-free image.
3. Leave the RNS log level alone for now. It is worth 39 KB and costs the
   on-device diagnostics that found this year's worst bug; revisit only if the
   partition work slips.
4. Evaluate `-flto` once, as a measurement rather than a default.

Explicitly **not** in scope: `-Os` and `--gc-sections` are already applied. The
`-O0` visible on the compile line is overridden by a later `-Os`, and proposing
those flags is a false lead this document exists to close.

### 4. RRC persistent rooms and the LXMF bridge — **critical for the command backbone**

Give rooms a persistence attribute and bridge the persistent ones to LXMF, per
[`docs/RRCRequirements.md`](RRCRequirements.md) §12c. Ad-hoc rooms keep today's
ephemeral behaviour; a provisioned set such as `general` is bridged, so members
who were out of range receive what they missed on their next sync.

Why this is ahead of the Bluetooth work:

- the stated product is group command and central command over this mesh, and a
  command net where "what did I miss" has no answer is not a command net;
- mobility makes it certain rather than hypothetical -- RRC's long-lived Links
  are the most path-sensitive part of the stack, so responders *will* drop and
  rejoin;
- it needs no new protocol. LXMF store-and-forward is built, accepted and spoken
  by stock clients, and the propagation node is running on both boards; and
- Bluetooth improves how a phone attaches, which matters less than whether the
  messages were there when it did.

Deliberately **not** in scope: adding a message store to the RRC hub. That
duplicates LXMF and breaks the ephemeral contract stock clients rely on.

Open questions are recorded in §12c -- per-member versus per-room addressing,
loop prevention, and the fact that a busy bridged room and residents' personal
mail would compete for one 512 KB store. That last one needs a policy before
this ships.

### 4a. Propagation node peering between RADs — **pairs with item 4**

LXMF already supports propagation nodes syncing to each other: `autopeer`,
`autopeer_maxdepth`, static peers and the `/offer` path, with each node offering
transient ids and peers requesting what they lack. Our firmware **declines all
peer offers** -- a deliberate v1 choice recorded in `docs/Messaging.md`, on the
grounds that a Linux node's 500 MB backlog would evict residents' messages from a
512 KB store.

That reasoning holds for a Linux peer and not for the case we actually have: two
RADs in one deployment, with identical caps, where a message stored on Rev 1 is
currently invisible to a client syncing with Rev 2. Clients pick one propagation
node; without peering, which one they picked silently decides what they receive.

Scope: accept peer sync selectively -- bounded by hop depth via
`autopeer_maxdepth`, and with locally-originated messages preferred over
peer-received ones during eviction, so a peer's backlog cannot displace the
people actually attached to this node.

### 5. Bluetooth Low Energy overhaul — **after the flash-headroom work**

Replace the Bluedroid BLE implementation with a NimBLE one that a phone can pair
from its own Bluetooth settings, per
[`docs/BluetoothOverhaul.md`](BluetoothOverhaul.md).

Why here:

- it is the only phone attachment path that needs no access point, no DHCP and
  no surviving building infrastructure, which makes it the most disaster-
  appropriate attachment of all;
- the current implementation cannot be paired without nRF Connect, because it
  never requests bonding and declares no IO capability -- a responder cannot be
  asked to install a developer tool;
- it is expected to *return* flash headroom rather than consume it, since
  Bluedroid is already linked into images sitting at 87-88% of the application
  partition; and
- Meshtastic runs the target configuration on the same silicon, so there is a
  behavioural oracle rather than a specification to guess at.

It is placed after SoftAP because SoftAP is smaller, reuses the working TCP
attachment path, and serves the same goal for a phone that still has WiFi. BLE
is the larger build and carries a WiFi coexistence risk that is better measured
once the SoftAP transitions are understood.

It now also follows the flash-headroom work in item 3, for two reasons. The
application partition is at 90.2% on Rev 2, so a stack replacement has nowhere
to land until the partition is enlarged. And the claim that NimBLE *returns*
headroom is currently unmeasured -- `RAD01_NO_BLE` does not compile, so nobody
has built a Bluetooth-free image to compare against. Item 3 repairs that target
and produces the number, which is this item's own business case.

Deliver it in the two stages that document defines: pairing and link stability
first, proven from stock OS settings and held for at least 30 minutes; the
Reticulum `BLEInterface` only after that. A partial port that pairs and then
drops is worse than the current state, because it looks like a broken product
rather than a missing feature.

### 6. Propagation-node operational controls and occupancy page

Add provisioning controls for enable/disable, limits, occupancy and purge, then
surface those counters on a NomadNet page. This closes the operator-visibility
gap in [`docs/PropagationNodeTODO.md`](PropagationNodeTODO.md) without requiring
an RTC.

### 7. Resilience release gates and orphan recovery

Hardware-verify the existing announce jitter and duty-cycle telemetry, define a
production duty-cycle configuration, then implement the listen-first orphan
preset sweep from `docs/MeshResilience.md`.

This is more important than internet-oriented convenience features, but follows
SoftAP because resident attachment is useful immediately and the orphan state
machine needs longer fleet-level timing tests.

### 8. Wall-time adoption and propagation expiry

Adopt trustworthy wall time from an authenticated client request, then implement
message expiry. A Rev3 battery-backed RTC will improve cold-start behaviour, but
firmware time adoption can be designed and tested on Rev1/Rev2 first.

Peer sync remains after this work because it depends on stable time semantics
and introduces substantial LoRa airtime cost.

### 9. Resource API Stage 1

Build the host-side schema-to-OpenAPI/Swagger generator proposed in
[`docs/ResourceAPI.md`](ResourceAPI.md). It provides industrial evaluation value
without firmware or protocol changes. Defer collections, events and new device
verbs until the generated interface has real users.

### 10. Outbound TCP client

Add `TCPClientInterface` for normal-time off-site anchoring and monitoring. It
does not solve the local disaster path, so it follows SoftAP and orphan recovery.

### 10a. Peer identity resolution for NomadNet users — **backlog, investigate before the next field test**

Two NomadNet users in the same RRC room could not message each other or open
each other's pages: the client asks for the peer's identity, the query goes out,
and nothing comes back. See
[`docs/IdentityResolutionBacklog.md`](IdentityResolutionBacklog.md).

Not obviously ours -- the firmware demonstrably serves cached announces in
response to path requests, including across the LoRa hop, which every RRC
acceptance run exercises. The likely causes are a client that never announced
where the asker could hear it, or a NomadNet gap where Columba has a working
control and NomadNet does not.

It is on the roadmap rather than left as a note because the failure is entirely
silent: a yellow banner, no error, no counter, no way forward for the user. That
is the same shape as the worst bugs this project has hit, and it will be
reported again by the next person who tries to message someone they just met in
a room.

### 10b. Internet gatewaying — **analysis done, no work scheduled**

Whether a connected node can share access with the mesh. See
[`docs/InternetGateway.md`](InternetGateway.md): Reticulum does not carry IP, so
the answer is application-level gateway services rather than connectivity, and
the bandwidth arithmetic rules out anything but text. An LXMF bridge to email or
SMS is the highest-value piece and belongs on the blackbox, not the RAD.

### Already solved upstream — do not build these

Recorded because both were about to be scoped as work, and both already exist.

**Direct-then-propagate delivery.** The choice between a direct and a propagated
message is not one the sender should have to make, and LXMF agrees: NomadNet's
`try_propagation_on_send_fail` retries a failed direct message as a propagated
one automatically, and it **defaults to on** (`NomadNetworkApp.py` sets
`try_propagation_on_fail = True`; `Conversation.py` performs the retry by
resetting the attempt counter and switching `desired_method` to `PROPAGATED`).
Duplicate suppression is already handled by transient-id tracking, so the same
message arriving twice is discarded rather than shown twice.

If it appears not to work, the cause is almost always that no outbound
propagation node is configured -- exactly the condition recorded in
`docs/Messaging.md` §2, where clients had the option enabled and had nothing to
fall back to. Both RADs are propagation nodes now, so the remedy is client
configuration rather than firmware.

**Propagation-node peering.** LXMF nodes do sync to each other -- see item 4a.
The mechanism is push-based offer-and-request rather than a DNS-style zone
transfer, but the effect is what was being asked for. It is our firmware that
declines to participate, not the protocol that lacks it.

### 11. Optional and research items

- RRC resources, moderation, persistent rooms and offline bridge.
- Propagation-node peer sync.
- Native Columba RRC UI.
- BLE GATT node-to-node transport.
- Resource transfers above the currently reliable measured range.
- Full embedded Resource API extensions.

These need evidence of demand or completion of their prerequisites before they
become scheduled features.

## Cross-cutting work that should not wait for a feature slot

The following are defects or release gates rather than product features. Fix or
verify them alongside the nearest affected PR:

1. Diagnose Rev2 application-mode UART transmit silence with a controlled
   parent-versus-pinned microReticulum A/B image. Keep the reproducible
   dependency pin unless that test proves otherwise.
2. Run the 19 microReticulum IFAC native cases in CI or on a host with a complete
   native toolchain.
3. Verify Link teardown under repeated RRC reconnects; repair the no-op Link
   watchdog/lifecycle upstream if closed Links accumulate.
4. Enforce a deployment-appropriate LoRa duty cycle before field release.
5. Validate propagation stamps before advertising their anti-spam cost as
   enforced.
6. Make `ACCEPT_APP` Resource rejection effective in microReticulum and
   investigate the measured transfer stalls above roughly 8 KiB.

## Priority rule

Choose the next item by dependency and disaster value, not by novelty:

1. finish the active feature and its real-client hardware acceptance;
2. prefer a feature that makes existing capabilities usable during an outage;
3. close security, bounded-memory and recoverability gates before adding surface
   area; and
4. defer features blocked by unavailable hardware or unstable semantics rather
   than implementing an untestable approximation.

Under that rule, finish review and merge of **RRC client and two-board
interoperability PR 3**. The next feature branch should then be **automatic
disaster SoftAP**.
