# PR 2: authenticated tasking over the RAD mesh

Status: scoped, implementation pending. Branch `feature/atak-tasking`, based
on `bbcc069` (merged position PR #21). Companion Columba branch:
`feature/tak-tasking`, based on `24b0a07f` from position reporting.

The first downlink should let a command-post operator send an addressed
go-to-point task with a short instruction. Columba presents the verified task
in a persistent inbox and lets its user accept or decline it. A task does not
start navigation or move a device automatically. Acceptance is a user decision,
distinct from packet delivery.

This is the next indoor PR. The infrastructure-free ESP-NOW/LoRa exercise
remains outdoor PR A and must pass before claiming field readiness. Markers
and LXMF/GeoChat bridging follow; voice and on-board GNSS remain separate.

## Two implementation findings

OTS publishes the firehose as a durable fanout exchange. In the local source,
`opentakserver/eud_handler/EudHandler.py::publish_cot()` publishes a JSON
envelope containing `uid` and `cot`. The separate parser exchange also gets
`user_id`; the firehose does not. A claimed UID is insufficient evidence that
an authorized operator issued a command. A trusted gateway must not sign every
matching firehose event and thereby make unauthenticated input trusted.

Columba exposes `RnsCore.observePackets()`, but generic destinations currently
do not feed it. The Python backend's `handlePacket()` only logs a placeholder;
the Kotlin backend declares a packet flow without emitting into it. Creating
and announcing a destination alone will not deliver tasks to an app consumer.
Both backend callback paths belong in this PR, with IPC delivery verified.

## Scope and trust boundary

1. Subscribe to OTS's firehose with bounded parsing, reconnect handling, and
   explicit selection of supported task events. Capture a real ATAK task event
   before fixing its type and recipient mapping; do not infer addressing from
   callsigns or the position codec's four-byte sender identifier.
2. Require authorization before signing. Initially, an imported firehose task
   can be staged for explicit approval by the local command-post operator.
   Automatic forwarding requires authenticated operator provenance available
   at this boundary and an explicit authorization policy. Broker access or a
   matching UID alone does not supply that provenance.
3. Encode one bounded binary task with a full recipient destination hash,
   task identifier, issue and expiry times, coordinates, short UTF-8
   instruction, and issuer identification. Sign the canonical bytes with a
   task-specific domain separator. Reuse the identity-signing primitives,
   keeping task authority separate from permission to supply time.
4. Verify against an explicitly trusted issuer key on the phone. Reject wrong
   recipients, bad signatures, unknown versions, malformed fields, expired
   tasks and unreasonable future timestamps before persistence or display.
   Persist replay protection across restarts; a duplicate must not create a
   fresh task, notification, or acceptance decision.
5. Wire incoming SINGLE-destination packets through both Columba backends and
   IPC. Persist verified tasks per local identity, provide trust configuration
   and an inbox, and distinguish received, accepted, declined and expired.
6. Return signed, recipient-bound task status to the command post. Show transport
   failure and missing acknowledgment honestly. Bound retries and deduplicate
   both directions; never label an enqueued packet as operator acceptance.

RADs forward encrypted Reticulum packets using their existing transports.
This task does not need a firmware position source, a new radio protocol, or
an executable-command interpreter on a RAD. Firmware-originated position is
still unproven and remains a separate acceptance item for peer position or
on-board GNSS.

A signed task is deliberately larger than a 21-byte position update. Measure
the complete encrypted packet size and airtime, including signatures and
Reticulum framing, before choosing the text limit and retry policy. Keep the
task inside a single packet and publish the actual budget.

## Merge gates

- Golden wire vectors shared by Python and Kotlin, including UTF-8 boundaries.
- Tamper, wrong-key, wrong-recipient, expiry, replay-after-restart and malformed
  input tests. Unknown or unauthenticated input must never become a trusted task.
- Receiver tests for both Columba backends and the process boundary.
- Packet size and retry budget measured with the actual task envelope.
- OTS to gateway to RAD to phone, then user response back to command post,
  demonstrated on hardware with the phone screen off before task arrival.
- Disconnect and reconnect exercise: no false acceptance and no duplicate task.
- Existing position reporting remains functional throughout the exercise.

Completing this PR establishes one task workflow, not full TAK compatibility.
Shared markers, chat, outdoor failover, and radio awareness retain their own
acceptance gates in the roadmap.
