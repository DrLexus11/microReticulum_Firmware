# A Resource API over Reticulum

Design notes for exposing devices as browsable, schema-described, validated
resources over Reticulum — with an eye to industrial/IoT use.

Status: **design proposal, nothing implemented.** Written 2026-08-22.

---

## 1. The premise, and why it is closer than it looks

The obvious framing is "let's build a REST-like API for Reticulum devices". The
more useful framing is that **most of it already exists in the Provisioning
protocol** (`docs/Provisioning.md`), and is already running on ESP32 hardware
today. Provisioning is not merely "a config protocol" — structurally it is a
schema-driven, validated, cache-aware, transactional RPC:

| REST/HTTP concept        | Provisioning equivalent                                     |
| ------------------------ | ----------------------------------------------------------- |
| OpenAPI document         | `GetSchema` — typed fields, ranges, enums with labels, flags |
| `GET`                    | `GetState` + `NamespaceFilter` (sparse fetch)                |
| `ETag` / `304 Not Modified` | `PriorHash` → `Unchanged`; `SchemaHash` for the schema    |
| `PATCH`                  | `SetState` (writes a draft, does not apply)                  |
| `400 / 403 / 404 / 409`  | `MalformedRequest`, `ReadOnly`, `UnknownField`, `ConstraintViolation` |
| Authentication           | Identity-based `ALLOW_LIST` + link encryption, no PKI        |
| *(no REST equivalent)*   | **`Commit` / `Discard` — transactional multi-field writes**  |

The transactional part is genuinely ahead of REST rather than behind it. Setting
five radio parameters that must change together is a single atomic commit, not
five requests that can half-succeed. That property is worth a great deal in
industrial settings and is awkward to retrofit onto HTTP.

Introspection already works in practice, not just on paper. On 2026-08-22 the
`Radio Preset` field — its id, its enum values and their human labels — was
discovered on a live device purely by calling `GetSchema`, with no
documentation consulted. That is the Swagger experience already functioning;
what is missing is a face for it.

**So this is not a from-scratch protocol project. It is (a) generalising
Provisioning beyond configuration, and (b) building the browsable front end.**

## 2. What is actually missing

1. **Arbitrary resources.** Namespaces model *configuration groups*. An API
   needs collections (N sensors, N ports, N jobs), not just a fixed field set.
2. **Richer verbs.** `command_void` exists for imperative gestures. A general
   API wants `Invoke` with typed arguments and a typed return, plus `Delete`.
3. **Events / subscriptions.** This matters more than REST-ness. Polling is the
   wrong model when a single request costs seconds of airtime. A
   subscribe/notify verb is the highest-value protocol addition for telemetry.
4. **A human-facing browser.** Two flavours, see §4.
5. **Host-side validation** generated from the same schema, so tooling rejects
   an out-of-range value *before* spending airtime discovering the device
   rejects it too.

## 3. Constraints that must shape the design

These are measured on this hardware, not assumed.

**Airtime is the binding constraint, not CPU or RAM.** At the current working
point (SF7 / BW250 / CR4:5, ~11 kbps raw) a ~500-byte page takes **4–5 seconds**
over two hops. A typical OpenAPI document is tens of kilobytes — *minutes* over
LoRa. This is survivable only because `SchemaHash` allows a client to fetch the
schema **once, ever**, and thereafter validate its cache with a single small
request. Any design that re-fetches schema per session is dead on arrival.

**Do not render HTML on the MCU.** Over a link where bytes cost seconds, HTML is
5–10× the payload of micron for identical content. Render micron on-device
(cheap, already done in `Pages.h`) and HTML at a gateway that already holds the
data.

**Memory is tight and has bitten us.** The ESP32-S3 runs ~50% free heap with
PSRAM spill-over already configured. Schema tables must stay static and built
once at registration, exactly as Provisioning does now. MsgPack only on device;
no JSON parser on the MCU.

**Duty cycle is currently unenforced.** `st_airtime_limit` / `lt_airtime_limit`
are `0.0` (disabled). A two-node bench is fine; an EU 868 industrial deployment
must enforce the 1% duty cycle. The mechanism exists and is simply switched off
— that must change before anything ships as "industrial".

**This is supervisory control, not closed-loop control.** Multi-second latency,
half-duplex collisions, no real-time guarantees. Excellent for telemetry,
configuration and human/minute-scale commands (SCADA-ish). Unsuitable for
interlocks, motion control, or anything safety-rated. This should be stated
plainly to anyone evaluating it industrially.

## 4. Proposed architecture

Layered so that each stage is independently useful and none of the early ones
touch the firmware.

### Stage 1 — Schema → OpenAPI + Swagger UI (host-side, zero MCU cost)

A host tool calls `GetSchema` once, caches by `SchemaHash`, and emits an
OpenAPI document; any standard Swagger UI then browses it. No protocol change,
no firmware change, no airtime beyond the one-time schema fetch.

This is the highest value-per-effort item by a wide margin and validates the
entire concept. If it feels wrong in use, the cost was a weekend rather than a
protocol.

### Stage 2 — Shared validation

Generate host-side validators from the same schema. One source of truth, two
enforcement points (host and MCU). Prevents burning airtime on requests that
were always going to be rejected.

### Stage 3 — Schema → micron renderer (NomadNet "Swagger")

`Pages.h` already hand-renders micron and emits links such as
`` `[• Interface`:/page/device.mu`c=interfaces]` ``. A generic renderer that
walks the schema and emits micron forms gives a browsable, editable device UI
from Sideband/NomadNet with no host involved. Moderate MCU cost; the schema is
already in memory.

### Stage 4 — Protocol extensions

Collections, `Invoke` with typed arguments, `Delete`, and subscribe/notify.
This is the real "feature of our own" and the bulk of the work. Deferring it
until stages 1–3 are in use means it gets designed against real usage rather
than speculation.

## 5. Why Reticulum, honestly

The differentiator is *not* that the API is RESTful. It is that Reticulum
provides:

- **Encryption and identity with no CA and no PKI.** Self-sovereign keys;
  per-handler `ALLOW_LIST` authorisation by identity hash.
- **No addressing infrastructure.** No DHCP, VLANs, static routes, or NAT
  traversal to arrange.
- **Medium independence and self-healing routing** — the same API over LoRa,
  serial, TCP, or packet radio, with transport nodes relaying automatically.

An industrial customer who has fought PKI and VLANs to get one sensor talking
will care far more about that than about verbs.

## 5a. Where this sits relative to NomadNet and IFAC

Three surfaces are easy to conflate. They are complementary, and the API must
not invent its own answer to a question already answered elsewhere:

- **NomadNet pages** — the human-facing surface. Micron markup, browsed from
  Sideband/Columba. Already implemented.
- **This resource API** — the machine-facing surface. MsgPack, typed schema,
  validation, transactional commit.
- **IFAC** — who may exchange packets on an interface at all. Not implemented;
  see `docs/PrivateMesh.md`, which is the next substantial piece of work.

**Authorisation must be shared, not duplicated.** NomadNet pages gate access by
proven identity against `ALLOW_LIST`
(`RNS::Transport::remote_management_allowed()`). The API must use that same
mechanism. A device with two different answers to "who may change this" has a
security bug waiting to be found by whoever holds the weaker one.

**IFAC does not remove the need for it.** Being on the network is not the same
as being authorised to reconfigure a radio, and on a public mesh anyone can
reach the node anyway. IFAC reduces exposure; identity authorisation is what
actually decides who may act.

**Ordering.** If government adoption progresses, IFAC is likely a hard
procurement requirement and blocks; the resource API is a differentiator for the
industrial thread but blocks nothing. That argues for IFAC first, with the API's
Stage 1 (host-side schema to OpenAPI, zero firmware cost) running alongside it
since the two do not touch the same code.

## 6. Non-goals

- Closed-loop or safety-rated control.
- HTML rendering on the microcontroller.
- Full OpenAPI/JSON-Schema fidelity on device — the *host* speaks OpenAPI; the
  device speaks compact MsgPack and a field table.
- Replacing Provisioning. This extends it; config remains a namespace like any
  other.
