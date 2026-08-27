# Firmware Feature Roadmap

Cross-feature priority for work that remains after the propagation-node and
private-mesh milestones. Detailed design and acceptance evidence remain in each
feature document; this file answers which implementation should happen next.

Status: **current recommendation**, updated 2026-08-27 after RRC PR 3 hardware
acceptance. The protocol core and embedded hub are merged. The reusable probe,
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

### 2. Automatic disaster SoftAP — **next after RRC**

Implement the SoftAP state and provisioning described in
[`docs/MeshResilience.md`](MeshResilience.md). The TCP server is already present;
the missing product behaviour is for a node to raise a local attachment network
when building infrastructure is unavailable.

Why second:

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

### 3. Propagation-node operational controls and occupancy page

Add provisioning controls for enable/disable, limits, occupancy and purge, then
surface those counters on a NomadNet page. This closes the operator-visibility
gap in [`docs/PropagationNodeTODO.md`](PropagationNodeTODO.md) without requiring
an RTC.

### 4. Resilience release gates and orphan recovery

Hardware-verify the existing announce jitter and duty-cycle telemetry, define a
production duty-cycle configuration, then implement the listen-first orphan
preset sweep from `docs/MeshResilience.md`.

This is more important than internet-oriented convenience features, but follows
SoftAP because resident attachment is useful immediately and the orphan state
machine needs longer fleet-level timing tests.

### 5. Wall-time adoption and propagation expiry

Adopt trustworthy wall time from an authenticated client request, then implement
message expiry. A Rev3 battery-backed RTC will improve cold-start behaviour, but
firmware time adoption can be designed and tested on Rev1/Rev2 first.

Peer sync remains after this work because it depends on stable time semantics
and introduces substantial LoRa airtime cost.

### 6. Resource API Stage 1

Build the host-side schema-to-OpenAPI/Swagger generator proposed in
[`docs/ResourceAPI.md`](ResourceAPI.md). It provides industrial evaluation value
without firmware or protocol changes. Defer collections, events and new device
verbs until the generated interface has real users.

### 7. Outbound TCP client

Add `TCPClientInterface` for normal-time off-site anchoring and monitoring. It
does not solve the local disaster path, so it follows SoftAP and orphan recovery.

### 8. Optional and research items

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
