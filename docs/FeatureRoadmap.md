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

### 4. Bluetooth Low Energy overhaul — **after the flash-headroom work**

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

### 5. Propagation-node operational controls and occupancy page

Add provisioning controls for enable/disable, limits, occupancy and purge, then
surface those counters on a NomadNet page. This closes the operator-visibility
gap in [`docs/PropagationNodeTODO.md`](PropagationNodeTODO.md) without requiring
an RTC.

### 6. Resilience release gates and orphan recovery

Hardware-verify the existing announce jitter and duty-cycle telemetry, define a
production duty-cycle configuration, then implement the listen-first orphan
preset sweep from `docs/MeshResilience.md`.

This is more important than internet-oriented convenience features, but follows
SoftAP because resident attachment is useful immediately and the orphan state
machine needs longer fleet-level timing tests.

### 7. Wall-time adoption and propagation expiry

Adopt trustworthy wall time from an authenticated client request, then implement
message expiry. A Rev3 battery-backed RTC will improve cold-start behaviour, but
firmware time adoption can be designed and tested on Rev1/Rev2 first.

Peer sync remains after this work because it depends on stable time semantics
and introduces substantial LoRa airtime cost.

### 8. Resource API Stage 1

Build the host-side schema-to-OpenAPI/Swagger generator proposed in
[`docs/ResourceAPI.md`](ResourceAPI.md). It provides industrial evaluation value
without firmware or protocol changes. Defer collections, events and new device
verbs until the generated interface has real users.

### 9. Outbound TCP client

Add `TCPClientInterface` for normal-time off-site anchoring and monitoring. It
does not solve the local disaster path, so it follows SoftAP and orphan recovery.

### 10. Optional and research items

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
