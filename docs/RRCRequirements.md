# Embedded Reticulum Relay Chat Hub Requirements

Requirements for hosting an interoperable Reticulum Relay Chat (RRC) hub on an
IMPR-RAD-01 node. The intended minimum viable product is live group text chat
over the existing Reticulum mesh.

Status: **requirements approved; PR 1 protocol core merged in PR #6 and PR 2
embedded hub merged in PR #7. PR 3 is implemented on
`feature/rrc-client-interop`; its automated, Rev2 and stock-client/two-board
hardware acceptance is green.**
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

### 11.6 PR 3 Rev2 and stock-client acceptance evidence

On 2026-08-27, Rev2 was promoted to the same `RRC_PROTOCOL_CORE` and `RRC_HUB`
feature set as Rev1. The UART build uses 109,452 of 327,680 bytes of internal RAM
(33.4%) and 1,847,477 of 2,097,152 application bytes (88.1%). The generated app
image is 1,847,840 bytes with whole-file SHA-256
`af5db4d500c18ccee6064eeac74cf57fc42a8483dd770ded1363d44bbbb31ccc`.
After the deliberate no-reset UART upload and physical power cycle, `fixhash`
wrote the ESP image validation hash
`906f4fec15f5448cd272837a74c2b93d78756d17f8629877a2f131054650b2fc`.
Rev2 then returned to `hw_ready`, Wi-Fi, TCP and propagation-node service.

The persistent Rev2 hub destination is
`736043f85ba10cd1b8f01b6726c7bee9`. A direct one-hop run through Rev2 TCP passed
the complete two-identity lifecycle with exact messages `REV2-HUB-A-d1a1ba`,
`REV2-HUB-B-d1a1ba` and `REV2-HUB-RECONNECT-d1a1ba`.

For the mixed automated run, an isolated Reticulum instance had only a TCP
client to Rev2 at `192.168.1.88:4242`. It resolved Rev1's hub destination
`d36d1371772fca94fb6dc2522d1c4254` at two hops, proving that the path entered
Rev2 and crossed LoRa to Rev1. The first strict run reached reconnect but lost
the final relayed message; the probe deliberately does not retry room messages
and reported that exact step. Both boards remained healthy. A repeat passed the
full join, bidirectional message, abrupt disconnect, `PARTED`, same-identity
reconnect and final message sequence with:

- client A `892b8502760a18fe22e9b4f82272daea`;
- client B `8248885c04f7e4c8dbda26083947a305`; and
- exact markers `REV2-LORA-REV1-R2-A-932015`,
  `REV2-LORA-REV1-R2-B-932015` and
  `REV2-LORA-REV1-R2-RECONNECT-932015`.

Final stock-client acceptance used the same real topology. Android Eridanus,
identity/nickname `Lexus`, used Columba Shared Instance attached to Rev1 TCP at
`192.168.1.54:4242`. A separate stock NomadNet 1.2.8 profile had only the Rev2
TCP client interface, connected to the Rev1 firmware hub at two hops, and joined
`#rad01-pr3`. NomadNet received exact message `ERIDANUS-PR3-A-27AUG` from Lexus;
Eridanus received exact return message `NOMADNET-PR3-B-27AUG`. The isolated
NomadNet client was then deliberately terminated and both boards still passed
serial, IP, TCP and propagation-announce health checks.

Eridanus automatically sent `/who rad01-pr3` after joining. Reference `rrcd`
consumes this extension and returns a private member-list notice. The MVP hub
does not implement slash commands, so it relayed the request as ordinary room
text. This does not invalidate RRC v1 messaging, but it is a visible stock-client
compatibility blemish and is prioritised in the deferred backlog below.

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

Status: **merged in PR #7 after Rev1 acceptance.**

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

Status: **implemented with automated and stock-client hardware acceptance green
on `feature/rrc-client-interop`.**

- reusable headless RRC test tool;
- NomadNet and Eridanus/Columba instructions;
- Rev1/Rev2 mixed TCP/LoRa acceptance;
- final memory/flash/session-limit measurements; and
- updated operator documentation.

Merge gate: NomadNet and Eridanus exchange exact room messages across the real
two-board path, reconnect cleanly and leave both boards healthy.

## 12b. Loop task stack, and why the hub appeared unjoinable

Reticulum runs inside `loop()`, so every Link callback executes on the Arduino
loop task and then descends through `Packet`, `Destination`, `sha256` and the
allocator. Arduino's default for that task is 8 KB.

Measured with `uxTaskGetStackHighWaterMark()` on Rev 1 after raising the task to
16 KB: **the deepest observed path leaves 5,048 bytes free, so it consumes about
11.3 KB.** Against the 8 KB default that is an overflow of roughly 3 KB, and it
was reached whenever the hub answered `HELLO` -- building a `WELCOME` on top of
an already-decoded envelope, with a 431-byte encode buffer then on the stack.

The failure gave no useful signal. ESP-IDF's stack-overflow watchpoint fired
inside whatever allocated next, so the panic pointed at `malloc`:

```
Link::receive -> handle_packet -> send_welcome -> send_envelope
  -> RNS::Packet -> RNS::Destination -> sha256 -> Bytes::newData
  -> malloc -> poison_allocated_region -> PANIC
```

To a client this looked like a hub that announced normally and could never be
joined: NomadNet reported "identified, sending HELLO" and was disconnected,
Eridanus could not join at all, and every session, room and counter was wiped
roughly once a minute. It also invalidated a day of testing, because every
result was measured against a board rebooting underneath it.

Two consequences worth carrying forward:

- The encode buffer is no longer a stack frame, and the loop task now has 16 KB.
  The margin is reported as a device metric so it can be watched rather than
  discovered by a crash.
- **`printf` is a heavy stack consumer and must be treated as unsafe in callback
  context.** A single diagnostic `printf` added inside `handle_packet` during
  investigation panicked the board on its own. `LXMFPropagation.h` calls it from
  twenty-three places reachable inside a callback; those are affordable at 16 KB
  but should be converted to counters rather than relied upon.

## 11.7 Re-verified acceptance after the stack fix

The evidence in 11.5 and 11.6 was recorded before the loop-task stack overflow
in 12b was found, on boards that were panicking and rebooting roughly once a
minute. Those runs are retained as history but should not be treated as
acceptance. This section replaces them.

Re-run 2026-08-27 with both boards on the fixed build.

**Boards.** Rev 1 image 87.5% of the application partition, Rev 2 (UART) 88.3%.
Both report `hw_ready 1`, radio `online=1`, stable propagation destinations, and
no panic across the observation windows -- 250 s on Rev 1 and 220 s on Rev 2,
where the previous build crashed every ~60 s.

**Single-board acceptance.** The two-identity probe passes against Rev 1's hub
`d36d1371772fca94fb6dc2522d1c4254` and Rev 2's `736043f85ba10cd1b8f01b6726c7bee9`:
`WELCOME` with the configured limits, join, exact bidirectional messages with
authenticated attribution, abrupt disconnect, `PARTED`, same-identity reconnect
and a final relayed message.

**Two-board mixed TCP/LoRa acceptance.** From an isolated Reticulum instance
whose only interface is a TCP client to Rev 2, Rev 1's hub resolves at **two
hops** while Rev 2's resolves at one -- so the path is probe -> Rev 2 over TCP
-> Rev 1 over LoRa. The full lifecycle passes across it with markers
`REV2-LORA-REV1-A/B/RECONNECT`. This is the LoRa-hop evidence that earlier work
could only infer.

**Service commands.** `/list` returns `Registered public rooms` with the active
rooms and `/who` returns `members in <room>: <nick> (hex12), <full-hex>`; both
parse correctly with stock NomadNet's own `_parse_room_list_notice` and
`_parse_who_notice`.

**Telemetry after the runs.**

| | Rev 1 | Rev 2 |
| --- | ---: | ---: |
| Loop stack free, minimum | 5,048 / 16,384 | 5,268 / 16,384 |
| RRC accepted | 107 | 11 |
| RRC rejected | 0 | 0 |
| Malformed envelopes | 0 | 0 |

**Stock clients.** Confirmed by the operator against Rev 1: connects quickly,
lists the registered room, joins from both Eridanus and NomadNet, messages both
ways without loss. The `/who` quirk and the "missing required field" error are
both gone.

**A caveat on the probe.** During this work the probe failed repeatedly through
the Deck's shared Reticulum instance while passing four times in a row through
an isolated direct-TCP instance against the same firmware, with the hub's own
counters showing a healthy session and nothing refused. The shared instance had
absorbed a very large amount of link churn. Treat probe failures through it as
unproven until reproduced through an isolated configuration.

## 12a. Service replies and the room-list bound

`/list` and `/who` are answered as private `NOTICE` text in the formats stock
clients parse (`_parse_room_list_notice` and `_parse_who_notice` in NomadNet).
Both replies are a single packet, so both are bounded by the Link MDU.

They are **not** bounded by the advertised `max_msg_body_bytes`. That limit
exists to bound what clients may send; applying it to the hub's own replies
truncated the room list silently. Measured on Rev 1 at a 431-byte MDU with
sixteen rooms:

| Room name length | Reply bytes | Rooms listed |
| ---: | ---: | ---: |
| 8 | 166 | 16 of 16 |
| 14 | 256 | 16 of 16 |
| 40 (client limit 280) | 277 | 7 of 16 |
| 40 (MDU budget) | 359 | 9 of 16 |

So all sixteen rooms fit whenever names are roughly twenty characters or
shorter, which covers realistic use. Longer names truncate, and that is
inherent: sixteen forty-character names need about 660 bytes and cannot cross a
431-byte link in one packet.

Truncation cannot be signalled inside the reply, because the client treats every
line after the header as a room name and a marker would appear as a phantom
room. A multi-part reply is also unavailable: NomadNet replaces its whole room
list on each parsed notice, so a second notice would overwrite the first rather
than extend it. The hub therefore counts truncated replies instead, so an
operator can see that clients received a partial view rather than having to
infer it from a short list.

## 12c. Persistent rooms and the LXMF bridge (implemented)

RRC is ephemeral by design and that is correct for incident chat, but the stated
product use is group command, central command and general comms -- a backbone.
There the question "what did I miss while I was out of range?" is the entire
point, and mobility guarantees it will be asked: long-lived Links are the part of
this stack most sensitive to path change.

The answer is **not** to add history to RRC. The ecosystem already has
store-and-forward, stock clients speak it, we have implemented and accepted it,
and it survives a node going down. Adding a message store to the hub would
duplicate that and quietly break the ephemeral contract stock clients expect.

### Two classes of room

Rooms gain a persistence attribute, and the two behave differently on purpose:

| | Ephemeral room | Bridged room |
| --- | --- | --- |
| Created | on demand by any `JOIN`, as today | provisioned, exists at boot |
| Membership | dies with the Link | dies with the Link |
| Message history | none | mirrored to LXMF |
| Absent members | miss everything | receive via propagation |
| Example | `#incident-3f` | `general`, `command` |

Ad-hoc rooms keep today's behaviour exactly. A bootstrapped set -- `general` and
whatever a deployment needs -- is provisioned and bridged. This matters
operationally: a responder joining `general` for the first time should see what
command has said, while a room spun up for one stairwell should not accumulate
anything.

### What bridging means

For a bridged room, accepted room traffic is additionally packed as an LXMF
message and handed to the propagation store, addressed so that members who were
not connected receive it on their next sync. Live members still get the ordinary
RRC fanout; the bridge is for the absent.

### The four design points, as decided

- **Addressing.** One LXMF message per absent member, which was the option
  expected to be right for these fleet sizes. The source is the node's own
  `lxmf`/`delivery` destination, the room name is the message title, and the
  body is `<nick> text`, so a stock client shows each bridged room as its own
  conversation rather than one undifferentiated thread. A room delivery
  identity that members subscribe to remains the cheaper answer if a room ever
  outgrows per-member fanout.

- **Loop prevention.** Free in this direction, and the reason is worth stating
  rather than assuming: nothing in the bridge injects into RRC, and RRC ingress
  is only ever an envelope arriving on a Link. The moment LXMF replies are made
  to appear in the room, loop prevention stops being free and has to be built
  on LXMF's transient-id tracking, as originally described.

- **Store pressure.** Bridged traffic is capped at a quarter of the store
  (`RRC_BRIDGE_STORE_QUOTA`) and evicted against that quota before the store's
  own cap is consulted, so a busy room fills its own share and never a
  resident's mail. The quota counts what this node composed during the current
  uptime: a stored blob's source is inside its ciphertext, so after a reboot
  ours are indistinguishable and revert to oldest-first eviction. The store
  stays bounded either way; only the fairness lapses, and only until the room
  is next busy.

- **Provisioning.** RRC namespace 113, fields 10 (enable) and 11 (a
  comma-separated room list, `#` optional and names normalized as everywhere
  else in RRC). Both reboot-required, since the bridge is constructed with the
  hub. Metrics 48-51 report roster size, queue depth, deliveries and drops.

### The roster, which is what makes it work

Membership dies with the Link, so an absent member is by definition one the hub
holds no session for and can no longer ask anything of. The bridge therefore
keeps its own roster per bridged room, capturing each member's **public key**
from the Link when they join. That key is what lets the hub address someone
later, and taking it from the Link rather than from an announce means the hub
can reach any member it has ever met, even one whose announce it never heard.

The roster is persisted (`/rrc_roster`, debounced to at most one write per 30
seconds). This is not an optimisation: a restarted hub that has forgotten its
roster delivers nothing to anyone until every member has joined again, while
presenting exactly like a healthy idle bridge -- the silent-failure signature
that has cost this project more time than anything else.

Bounds, as everywhere else: `RRC_BRIDGE_MAX_ROOMS` (4), `RRC_BRIDGE_MAX_MEMBERS`
(16 per room) and `RRC_BRIDGE_QUEUE_DEPTH` (8 messages). A full roster drops the
newcomer rather than evicting an established member, on the grounds that a
command room's standing membership matters more than admitting the latest
arrival.

### Where the work happens

Composing costs a signature and an encryption per recipient, and it deliberately
does not happen on the path that handles room traffic: that path runs inside a
Reticulum callback, where this project has already lost a board to a stack
overflow. Accepting a message only resolves recipients and copies text; the
cryptography is done one recipient per iteration of the main loop.

`LXMFCompose.h` is the only place in the firmware that constructs LXMF, and it
should stay that way -- everything else treats messages as opaque, which is what
makes a propagation node fit on this hardware at all. Its layout is pinned to
the Python reference by `tests/test_lxmf_protocol.py`, because a malformed
composed message is worse than a malformed stored one: it syncs perfectly and is
then discarded inside someone else's client with no error reaching us.

### Verified end to end

On Rev 2, 2026-08-28. A member joined `command`, disconnected, a second member
posted while she was away, and she received the message by syncing the node's
propagation store with the reference LXMF client:

    from    : cd2e55faf7d4029e6af12ea2533abd98
    title   : 'command'
    content : '<bob> BRIDGE-TEST-...'
    signature : VALID

Hub metrics for the run were roster 2, delivered 1, dropped 0 -- one message
composed, addressed to the absent member only and not to the one who was
present and had already received the live fanout. The roster also survived a
reflash and reboot (`bridge roster loaded: 3 member(s)`), which is the property
that stops a restarted hub from silently delivering nothing.

**What that test caught.** The first run returned the correct title and content
with `signature: INVALID`, which was not a signing fault. LXMF validates by
recalling the source identity from an announce, and nothing had ever announced
the bridge's `lxmf`/`delivery` address, so the client could not attempt
validation and reported `SOURCE_UNKNOWN`. Both outcomes set
`signature_validated = False`, so a correctly signed message is
indistinguishable from a forged one unless `unverified_reason` is read. The
bridge now announces that address 45 seconds after boot and every 30 minutes
after, and the same test returns `signature: VALID`.

The lesson generalises: composing correctly is necessary and not sufficient. A
message can be byte-perfect and still be shown as untrusted because the
recipient has no way to learn who sent it.

### Not yet done

- **The reverse direction.** An LXMF reply does not appear in the room. Adding
  it brings the loop-prevention work described above.
- **Propagation stamps.** Composed messages carry a zero stamp, which is inert
  because a node strips the stamp before serving and these never cross an ingest
  gate. Peering (roadmap 4a) validates stamps and would reject them.

## 13. Explicitly deferred backlog

These are valid follow-ups, not hidden MVP requirements:

1. `RESOURCE_ENVELOPE` and Reticulum Resource transfer.
2. Persistent registered rooms, topics, keys, invites, operators and bans.
3. Minimal `/who` and `/names` compatibility. Eridanus was observed
   automatically sending `/who <room>` after `JOIN`; until the hub consumes it,
   the request appears as ordinary chat. Implement the bounded `rrcd`-compatible
   private member-list notice first, without pulling moderation into the MVP.
4. Other slash commands such as `/list`, `/mode` and `/stats`.
5. Server-side logging or an opt-in archive bot.
6. Offline history/replay. Prefer an explicit RRC-to-LXMF bridge rather than
   quietly changing RRC's ephemeral contract.
7. Direct notices and other `rrcd` extensions after capability negotiation.
8. Multiple/federated hubs. RRC intentionally defines no federation protocol.
9. Native RRC UI inside Columba. Eridanus covers Android for the MVP.

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
