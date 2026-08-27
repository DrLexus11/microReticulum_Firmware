# Embedded Reticulum Relay Chat Hub Requirements

Requirements for hosting an interoperable Reticulum Relay Chat (RRC) hub on an
IMPR-RAD-01 node. The intended minimum viable product is live group text chat
over the existing Reticulum mesh.

Status: **requirements approved; PR 1 protocol core merged and PR 2 embedded
hub implemented on `feature/rrc-embedded-hub` with Rev1 automated and physical
power-cycle acceptance green. PR 3 stock-client/two-board acceptance remains.**
Written 2026-08-26 and updated 2026-08-27. The recommended implementation order
is recorded in
[`docs/FeatureRoadmap.md`](FeatureRoadmap.md).

## 1. Product goal

A RAD-01 shall host an RRC v1 hub that stock clients can discover, connect to,
join a room on and use for near-real-time group text chat. It must work over any
path Reticulum can establish, including mixed TCP-to-LoRa paths.

The MVP is deliberately a live chat relay rather than another store-and-forward
system:

- clients establish encrypted Reticulum Links to the hub;
- clients join named rooms;
- the hub forwards room traffic to members currently connected; and
- membership and messages disappear when clients disconnect or the hub reboots.

This matches RRC's intended semantics. It does not replace LXMF or the ESP32
propagation node. LXMF remains the correct path for offline delivery, while RRC
serves incident coordination among participants who are online at the same
time.

References:

- [RRC protocol specification](https://rrc.kc1awv.net/1-RRC-protocol.html)
- [RRC message and session behaviour](https://rrc.kc1awv.net/2-RRC-message-types.html)
- [RRC wire encoding](https://rrc.kc1awv.net/3-RRC-wire-encoding.html)
- [rrcd reference hub](https://github.com/kc1awv/rrcd)

## 2. Client strategy

No Columba fork is required for the MVP.

- **NomadNet** is the primary desktop client and protocol oracle. The installed
  NomadNet 1.2.8 already contains a complete RRC v1 client.
- **Eridanus** is the primary Android group-chat client. It can use a Reticulum
  shared instance hosted by Columba 2.x or Sideband.
- **Columba** remains the phone's Reticulum interface and LXMF client. It does
  not need an RRC user interface if Eridanus attaches to its shared instance.
- A small headless Python RRC probe shall provide deterministic automation and
  a second independent identity during tests.

Relevant clients:

- [NomadNet](https://github.com/markqvist/NomadNet)
- [Eridanus](https://github.com/torlando-tech/eridanus)
- [Columba](https://github.com/torlando-tech/columba)

If the deployed Columba build does not expose **Shared Instance**, use Sideband
or update to a Columba 2.x beta that does. Adding group chat directly to Columba
is a separate mobile-product decision and is not firmware scope.

## 3. Architecture

The hub is an application service above Reticulum, not a new Interface and not
a modification to Reticulum routing:

```text
NomadNet -----------\
                     \     encrypted RNS Links       ephemeral rooms
Eridanus/Android -----+----------------------------> RAD-01 RRC hub
                     /                                  `rrc.hub`
headless test probe -/
```

The firmware shall create an inbound `SINGLE` Destination with the application
name and aspects `rrc.hub`. It shall use a persistent identity so the hub
destination hash remains stable across reboot and firmware updates. Reuse the
node's existing persistent Reticulum identity unless implementation work finds
a concrete isolation requirement for a separate key.

The hub shall announce enough compact application data for compatible clients
to identify it as RRC v1. Follow the convention implemented by `rrcd` and
Eridanus:

```text
{"proto": "rrc", "v": 1, "hub": "<configured name>"}
```

The announce payload is CBOR. Protocol envelopes use unsigned integer keys;
the string keys above apply only to the discovery convention.

## 4. Reticulum Link requirements

Links are required for RRC. They provide the encrypted, authenticated,
bidirectional session on which RRC packets travel.

For every incoming Link the hub shall:

1. enforce the configured concurrent-session cap, tearing down excess Links;
2. install packet, remote-identified and closed callbacks;
3. wait for a proven remote identity before accepting application traffic;
4. require an initial `HELLO` before room or message operations, while treating
   a retry as idempotent if the client's `WELCOME` was lost;
5. remove the session from every room when its Link closes; and
6. ensure every Link and session object is released after close or timeout.

The hub shall have separate timeouts for:

- Link established but client not identified;
- identified client that has not sent `HELLO`; and
- an active client that no longer responds at the application layer.

`microReticulum::Link::start_watchdog()` is currently a TODO. The hub must
therefore enforce its own bounded handshake and idle-session cleanup instead of
assuming the library will reap every stale Link. If repeated connect/drop tests
show that `teardown()` does not remove closed Links from Transport, fix that in
the microReticulum fork as a separate prerequisite PR.

RRC `PING`/`PONG` is an application responsiveness check rather than a routing
keepalive. A missed response may be used as the hub's resource-management reason
to close an idle session, but must not be presented as proof that the identity
has left the wider mesh. A session may have at most one unanswered hub `PING`;
the scheduler must not send another or extend the original PONG deadline while
that check is pending, even when the configured PING interval is shorter than
the PONG timeout.

## 5. Wire protocol requirements

All messages shall use RRC protocol version 1 and be encoded as CBOR maps with
unsigned integer keys. Key ordering is irrelevant. Unknown keys and unknown
message types must be ignored.

Required envelope fields:

| Key | Field | Requirement |
| --- | --- | --- |
| `0` | version | unsigned integer, exactly `1` |
| `1` | type | unsigned integer |
| `2` | message id | exactly 8 random bytes |
| `3` | timestamp | unsigned integer milliseconds; advisory |
| `4` | source | exactly 16 bytes |
| `5` | room | optional UTF-8 text |
| `6` | body | type depends on message |
| `7` | nickname | optional UTF-8 text; never an identity |

Required message types:

| Value | Type | MVP behaviour |
| --- | --- | --- |
| `1` | `HELLO` | establish application session |
| `2` | `WELCOME` | advertise hub name, version, capabilities and limits |
| `10` | `JOIN` | join or create an ephemeral room |
| `11` | `JOINED` | confirm join; optional advisory member list |
| `12` | `PART` | leave a room |
| `13` | `PARTED` | confirm or notify departure |
| `20` | `MSG` | forward room text unchanged |
| `21` | `NOTICE` | forward informational room content |
| `22` | `ACTION` | forward action/emote room content |
| `30` | `PING` | respond with `PONG`, echoing the body |
| `31` | `PONG` | clear the session's pending responsiveness check |
| `40` | `ERROR` | return a short human-readable refusal where safe |

The hub shall preserve the client's message id, timestamp and body when it
forwards room content. It shall normalize the room and stamp the authenticated
source and accepted nickname before forwarding.

For the MVP, `MSG`, `NOTICE` and `ACTION` bodies must be UTF-8 text strings.
The RRC specification permits structured bodies, but accepting recursively
nested network-controlled objects adds memory risk without helping the group
text-chat goal. Unknown optional `HELLO` and `WELCOME` body keys remain ignored.

Follow the capability and limit-map convention shared by NomadNet, `rrcd` and
Eridanus. In `WELCOME` body key `2`, advertise action support with capability
key `1 = true` and omit or set Resource support key `0 = false`. In body key
`3`, use integer limit keys `0` nickname bytes, `1` room-name bytes, `2`
message-body bytes, `3` rooms per session and `4` messages per minute.

Reticulum Resources and the extended `RESOURCE_ENVELOPE` type are not required
for the MVP. Every encoded core envelope must fit `Link::get_mdu()` before send.
Large content is rejected rather than partially sent.

## 6. Room and message behaviour

- Room comparison shall be case-insensitive; normalize to lowercase internally.
- Joining a missing room creates it on demand.
- A room with no remaining members shall be deleted.
- Membership belongs to a Link session and is never restored implicitly.
- A repeated `HELLO` on an already welcomed session must not reset memberships.
  It is answered with the same effective `WELCOME` because stock NomadNet retries
  `HELLO` until it observes that response; this is idempotent recovery, not a new
  application session.
- A successful `JOIN` returns `JOINED` to the joiner and may notify existing
  members.
- A clean `PART` or closed Link removes the member and may send `PARTED` to the
  remaining members.
- `MSG`, `NOTICE` and `ACTION` are sent only by a member to a room it has joined.
- Accepted room content is forwarded to every current room member, including
  the sender, matching the existing clients' echo expectations.
- Nicknames are trimmed, bounded display hints. Duplicate nicknames are valid;
  the 16-byte Reticulum identity hash remains authoritative.
- Slash commands, room registration, topics and moderation are not interpreted
  in the MVP. A hub shall not accidentally grant authority based on a nickname.

The firmware has no persistent wall clock. Incoming client timestamps are
forwarded unchanged and never used for authority, ordering, expiry or rate
calculation. Hub-generated timestamps use Unix milliseconds when wall time is
available and `0` otherwise; uptime must not masquerade as Unix time. The field
is advisory, so the missing RTC does not block RRC.

## 7. Initial resource limits

The implementation shall make all allocations bounded. These are starting
defaults, not unmeasured promises; hardware profiling may lower them before
merge. Limits visible to clients shall be included in `WELCOME`.

| Limit | Initial value | Reason |
| --- | ---: | --- |
| Concurrent sessions | 8 | bounded ESP32 Link/session state |
| Rooms per session | 4 | adequate for the MVP without unbounded sets |
| Total active rooms | 16 | bounds global room metadata |
| Nickname | 32 UTF-8 bytes | reference-client default |
| Room name | 64 UTF-8 bytes | reference-client default |
| Message body | 280 bytes | fits a 431-byte Link MDU with maximum room/nickname fields |
| Messages per minute per session | 60 | conservative starting point for mixed LoRa paths |
| Identify/HELLO deadline | 30 seconds each | closes incomplete sessions |
| Hub PING interval | 60 seconds | responsiveness and cleanup cadence |
| PONG deadline | 30 seconds | bounded stale-session retention |

The complete encoded envelope, not only its body, must fit the active Link MDU.
At microReticulum's current 431-byte Link MDU, the fixed 43-byte envelope plus a
64-byte room, 32-byte nickname and 280-byte body encodes to approximately 429
bytes. Recalculate this from the encoded result rather than relying on the
estimate, since a negotiated lower MDU must reduce the accepted body size.
Reject before allocation or forwarding when the encoded message cannot fit.

Use fixed-capacity or explicitly capped containers for sessions, rooms and
memberships. Prefer PSRAM for non-cryptographic bulk state where the existing
allocator policy permits it, but do not use PSRAM as an excuse for an unbounded
network-controlled collection.

## 8. Security requirements

The Link's proven remote identity is the only authoritative client identity.
The `K_SRC` field is untrusted input.

The hub shall:

- ignore packets until `Link::get_remote_identity()` succeeds;
- overwrite every forwarded client `K_SRC` with that identity's 16-byte hash;
- reject malformed fixed-size fields and invalid UTF-8;
- reject pre-`HELLO` operations other than `HELLO`;
- reject messages for rooms the session has not joined;
- rate-limit all application messages except the `PONG` needed for cleanup;
- bound decode depth, collection sizes and input bytes before allocation;
- avoid logging message bodies, passphrases or full identity material by
  default; and
- clear all membership and rate state when a Link closes.

RRC Link encryption protects traffic between each client and the hub. It is not
end-to-end encryption among room participants: the hub sees content in order to
relay it. Operators and product material must say this plainly.

For a closed deployment, run RRC over the IFAC-secured interfaces and secure
node posture described in [`docs/PrivateMesh.md`](PrivateMesh.md). IFAC controls
network admission; it does not change the hub's obligation to authenticate the
Link identity and constrain each session.

## 9. Provisioning and operator visibility

Add a dedicated, persistent RRC provisioning namespace with at least:

- enabled;
- hub display name;
- announce interval;
- maximum concurrent sessions;
- maximum rooms per session;
- maximum message body bytes;
- rate limit; and
- PING interval and timeout.

Changes that create/destroy the Destination or materially resize static state
may be reboot-required. Invalid combinations must be rejected at commit rather
than silently clamped.

Expose read-only operational fields through provisioning and the NomadNet
status surface:

- hub destination hash and enabled/running state;
- connected and identified sessions;
- active rooms and memberships;
- messages accepted, forwarded, rejected and rate-limited;
- malformed envelopes;
- Link/HELLO/PONG timeouts; and
- current configured limits.

Do not expose room message bodies through status pages.

## 10. Implementation constraints

- Use the TinyCBOR API. The current PlatformIO ESP32-S3 framework contains a
  prebuilt `libcbor.a`, but its exported `cbor.h` façade refers to source headers
  missing from the installed framework package. PR 1 therefore pins
  `sgoudsme/tinycbor@0.6.2` only in the isolated native-test environment. Before
  enabling RRC in an embedded target, PR 2 must either supply matching headers
  for the framework binary or compile the pinned package, and must verify that
  only one TinyCBOR implementation is linked. PR 2 resolves this by setting the
  Rev1 environment to compatibility mode `off` and compiling
  `sgoudsme/tinycbor@0.6.2` source with its matching headers; the dependency and
  compile logs show that implementation in the final link.
- Wrap only the CBOR types used by RRC v1: unsigned integers, byte strings,
  UTF-8 strings, arrays, maps, booleans and null where needed.
- Keep protocol parsing independent from Arduino and Reticulum objects so it can
  run in native tests.
- Keep room/session policy independent from callback glue for the same reason.
- Reticulum callbacks are function pointers in microReticulum. Use a bounded
  manager keyed by Link id rather than captured lambdas or unbounded globals.
- Do not enable Resource transfer merely because microReticulum exposes it.
  The propagation-node work has already measured stalls above roughly 8 KiB;
  RRC core messages do not need that risk.
- Measure final flash, internal heap, PSRAM use and Link cleanup on both RAD
  builds. Rev2's application UART problem is a separate diagnostic issue and
  must not be hidden inside the RRC feature.

## 11. Verification plan

### 11.1 Native protocol tests

Test the codec and state engine without hardware:

1. encode/decode golden envelopes generated independently with NomadNet's
   Python CBOR implementation;
2. accept arbitrary CBOR key ordering and ignore unknown keys/types;
3. reject wrong version, fixed-field sizes, malformed CBOR and invalid UTF-8;
4. prove `K_SRC` spoofing is overwritten by the authenticated identity;
5. exercise HELLO gating and duplicate HELLO policy;
6. exercise join, broadcast, part and abrupt Link-close cleanup;
7. enforce every configured count, byte and rate limit;
8. prove encoded-size rejection at the Link MDU boundary; and
9. repeat connect/close cycles until leaks or stale memberships would be
   visible.

Run the suite from the repository root with:

```console
pio test -e rrc-native
```

The test target uses the normal host development headers when present. On the
Steam Deck development image, where SteamOS supplies the glibc runtime without
`/usr/include`, its pre-build helper falls back to the active Conda sysroot.

### 11.2 Automated Reticulum interoperability

Use `rrcd` as the behavioural oracle and build a headless two-client probe with
two persistent identities. Against the firmware hub:

1. both clients discover or receive the hub hash;
2. both establish and identify Links;
3. both receive `WELCOME` and join the same room;
4. each sends a labelled UTF-8 message observed exactly by both clients;
5. join/part/member notifications are well formed;
6. spoof, non-member, malformed, oversize and rate-limit cases are rejected;
7. disconnect/reconnect creates clean new membership; and
8. hub destination hash remains stable after power cycle.

### 11.3 Real-client and hardware acceptance

NomadNet alone proves UI compatibility but cannot prove many-to-many relay.
Final acceptance therefore uses two real clients and both boards:

- Rev1 hosts the first firmware RRC hub and remains the primary development
  target.
- Android runs Eridanus through Columba Shared Instance and attaches through
  Rev1 TCP.
- Deck runs NomadNet under a different identity through Rev2 and the LoRa path.
- Both clients join the same room and exchange exact labelled messages in both
  directions.
- Repeat after a hub power cycle and after deliberately dropping one client.
- Confirm the board remains reachable, radio counters advance, sessions return
  to zero after disconnect, and no active Link accumulates.

Rev2 only needs promotion to the RRC build after native and Rev1 acceptance are
green. Runtime control may use TCP/provisioning while its application UART
output is investigated separately.

### 11.4 Client compatibility evidence before the embedded hub

On 2026-08-26, stock NomadNet and Eridanus joined the same Deck-hosted RRC
channel and exchanged messages successfully. Eridanus used the Columba Shared
Instance while Columba was attached to Rev1's TCP server at `192.168.1.54`.

This proves the selected desktop/Android client pair interoperates and that the
Shared Instance arrangement works. Depending on the Deck's active Reticulum
path, the encrypted RRC Link may also have traversed Rev1 as a transport. It
does **not** prove the firmware hub: the RRC destination and room service were
still hosted on the Deck, Rev1 did not terminate `rrc.hub`, and LXMF was not the
carrier for the RRC messages. PR 2 acceptance must repeat the exchange with the
destination hosted by Rev1.

### 11.5 PR 2 Rev1 acceptance evidence

Automated acceptance on 2026-08-27 used the firmware-hosted destination
`d36d1371772fca94fb6dc2522d1c4254` over Rev1's UDP Reticulum interface. Its
announce application data decoded to
`{"proto":"rrc","v":1,"hub":"IMPR-RAD RRC"}` and the hash remained unchanged
across flashes, software reboots and a physical power cycle.

- Two independent identities established Links, received `WELCOME` with the
  configured limits, joined `#rad01-acceptance`, and both observed exact messages
  `REV1-RRC-A-d394fd` and `REV1-RRC-B-8d7b05` from both senders. Authenticated
  source hashes matched each Link identity. `PARTED` reached the sender and peer.
- Negative cases passed for identified-but-pre-HELLO traffic, malformed CBOR,
  non-member send, forged `K_SRC` and nickname, 281-byte body rejection, and
  silent handling of an unknown type.
- A temporary provisioned rate of 10/minute was advertised in `WELCOME`. After
  the already-consumed setup tokens, four more room messages were accepted and
  the next received `ERROR: rate limited`. Telemetry recorded 13 RX, 7 accepted,
  5 forwarded, 5 rejected, 1 rate-limited and 2 malformed envelopes, then the
  production default of 60/minute was restored and rebooted.
- A remotely observed identified/no-HELLO Link raised sessions from the live
  baseline of one to two, then returned exactly to baseline after the 30-second
  deadline and incremented `hello_timeouts`. This avoids the native-USB reset
  caused by opening Rev1's serial provisioning port and proves bounded cleanup.
- Native protocol/state tests pass 17/17 with ASan/UBSan. The Rev1 image uses
  108,948 of 327,680 bytes of internal RAM (33.2%) and 1,831,501 of 2,097,152
  application bytes (87.3%). The final image's whole-file SHA-256 is
  `e6d588c6c9dcb7164d2fa4c04841b4ffd370c045629b78ef6b4b6acdfc164eb8`.
  Its ESP app image has the hash-appended flag set, so the partition digest
  written to the board for boot validation is the image's appended SHA-256,
  `295fa3113363f284569222da3467d63d13b5c8fdfc0c808501fce5eac4d67ead`.
  `firmware_image.esp_image_sha256()` independently reproduced that value from
  the built artifact.
- On that final image, the authorized NomadNet device page reported the stable
  destination, running state, all configured limits and live counters. A valid
  `HELLO` sent before Link identification produced no response; after identifying
  on the same Link, the hub returned `WELCOME` with body/room/rate limits of
  280 bytes, 4 rooms and 60 messages/minute.
- After a physical Rev1 power cycle, the device page initially reported zero
  sessions, rooms and memberships; the hub retained destination
  `d36d1371772fca94fb6dc2522d1c4254`, the 60/minute setting and all other
  configured limits. A fresh identified Link received the expected `WELCOME`.
  Once the stock client reconnected, a separate cleanup probe raised the live
  session baseline from one to two without joining a room, then teardown returned
  sessions, identified sessions, rooms and memberships exactly to the live
  `1/1/1/1` baseline.

First Link requests were intermittently lost even while path discovery remained
healthy; a refreshed/retried request then established normally. NomadNet already
retries both Link connection and HELLO. This is recorded as transport-loss
evidence, not attributed to the RRC state machine.

## 12. PR plan and merge gates

### PR 1 — protocol core

Status: **merged in PR #6.**

- RRC constants and typed envelope representation;
- minimal TinyCBOR wrapper;
- validation and encoded-size accounting;
- bounded room/session state engine; and
- native golden-vector and policy tests.

Merge gate: native tests pass without Arduino hardware and match Python-generated
wire vectors.

### PR 2 — embedded hub

Status: **implemented, Rev1 acceptance green and ready to merge on
`feature/rrc-embedded-hub`.**

- persistent `rrc.hub` Destination and discovery announce;
- Link callbacks, identity/HELLO gating and cleanup;
- room traffic forwarding;
- provisioning, limits and operational counters; and
- Rev1 flash and automated two-client acceptance.

Merge gate: exact bidirectional group messages, negative cases, power-cycle
identity stability and zero stale sessions on Rev1.

If Link teardown or stale cleanup fails, stop and repair microReticulum in a
small separate PR before extending RRC scope.

### PR 3 — client and two-board interoperability

Status: **pending after PR 2.**

- reusable headless RRC test tool;
- NomadNet and Eridanus/Columba instructions;
- Rev1/Rev2 mixed TCP/LoRa acceptance;
- final memory/flash/session-limit measurements; and
- updated operator documentation.

Merge gate: NomadNet and Eridanus exchange exact room messages across the real
two-board path, reconnect cleanly and leave both boards healthy.

## 13. Explicitly deferred backlog

These are valid follow-ups, not hidden MVP requirements:

1. `RESOURCE_ENVELOPE` and Reticulum Resource transfer.
2. Persistent registered rooms, topics, keys, invites, operators and bans.
3. Slash commands such as `/who`, `/list`, `/mode` and `/stats`.
4. Server-side logging or an opt-in archive bot.
5. Offline history/replay. Prefer an explicit RRC-to-LXMF bridge rather than
   quietly changing RRC's ephemeral contract.
6. Direct notices and other `rrcd` extensions after capability negotiation.
7. Multiple/federated hubs. RRC intentionally defines no federation protocol.
8. Native RRC UI inside Columba. Eridanus covers Android for the MVP.

## 14. Definition of done

The feature is complete only when all of the following are true:

- stock NomadNet and Eridanus can use it without patches;
- two different identities exchange exact messages in one room over hardware;
- Link identity, not envelope source or nickname, controls attribution;
- all network-controlled memory has an enforced cap;
- malformed, oversized, pre-handshake and rate-limited traffic cannot destabilise
  the node;
- sessions and rooms are released after disconnect and repeated reconnects;
- hub identity and configuration survive reboot;
- final firmware hashes and resource measurements are recorded; and
- Rev1 and Rev2 remain healthy after the mixed-path acceptance run.
