# Wall time and duty-cycle enforcement

The two prerequisites that block TAK, and that each pay for themselves without
it. Neither needs a board revision.

Status: **proposal, nothing implemented.** Written 2026-09-03.

Related: [`TAKCapability.md`](TAKCapability.md) (the analysis that surfaced
both), [`FeatureRoadmap.md`](FeatureRoadmap.md) item 8, and
[`RRCRequirements.md`](RRCRequirements.md) §6.

---

## Why these two, and why together

[`TAKCapability.md`](TAKCapability.md) concludes that ATAK support is tractable
because the client never speaks Reticulum: a gateway expands compact positions
into CoT XML. What that analysis does not say loudly enough is that **two
firmware gaps block it before any TAK code is written**, and both are already
costing us elsewhere.

They are independent -- either can be built without the other -- and both are
firmware, not hardware. A Rev3 RTC helps one of them slightly and is not on the
critical path for either.

---

# Part 1 -- Wall time

## The root cause, in one line

```cpp
// microReticulum, Utilities/OS.h -- embedded:
inline static double time() { return (double)(ltime() / 1000.0); }   // uptime
// the same call, native:
inline static double time() { gettimeofday(...); }                   // wall time
```

`OS::time()` returns **uptime** on ESP32. Everything downstream inherits that,
and there is no `configTime`, no SNTP and no time adoption anywhere in the
firmware.

## What it already costs

Three separate features are deferred or dishonest because of it:

- **LXMF propagation** announces uptime as its timebase
  (`LXMFPropagation.h`), so two nodes cannot compare ages at all.
- **Message expiry is unimplementable.** Ageing needs a clock; we evict on
  capacity only, where upstream Python expires at 30 days.
- **RRC hub timestamps are sent as 0** on purpose, because uptime must not
  masquerade as Unix time.

That last one is the right call and must survive whatever replaces it: a node
that does not know the time should say so, not guess.

For TAK specifically the requirement is harder than "nice to have". **CoT events
are timestamped by definition** -- `time`, `start` and `stale`, all absolute UTC.
`stale` is what makes ATAK drop a track it can no longer trust. A marker that
never goes stale is worse than no marker, because it renders a team member's last
known position as current. A gateway cannot synthesise this: only the sender
knows when the fix was taken.

## Design

Keep the monotonic clock. Add an offset.

- `millis()` remains the source for every interval, timeout and backoff. Nothing
  that measures duration should ever read wall time.
- A `wall_offset` is set when a trusted source supplies UTC, and persisted to
  `/time_offset` -- the file the Ozdisan partition table header already
  anticipates and instructs operators to preserve.
- `OS::time()` returns `uptime + wall_offset` once the offset is known, and
  continues to report uptime with an explicit "time unknown" flag until then.

Callers must be able to ask **whether** the time is known, not just what it is.
Every current caller that sends 0 rather than a fake timestamp is doing the right
thing and should switch to the real value only when the flag says it may.

## Where time comes from

In priority order. These are complementary, not alternatives.

| Source | Cost | Available when |
| --- | --- | --- |
| **NTP over Wi-Fi** | ~5 lines, `configTime()` | any board with a station link -- Rev 1 is on Wi-Fi today, so this works immediately |
| **Authenticated client** | small | **the outage case**: no infrastructure, but a peered phone with a good clock |
| **GNSS** | a UART module on free GPIOs | an isolated field node with no phone and no Wi-Fi |
| **RTC (Rev 3)** | board revision | only across a cold boot |

The second row is the one that matters for this product. In a real outage there
is no NTP, but every responder carries a phone whose clock is good, and Columba
is already peered over BLE. The node asks its identified peer.

**An RTC cannot originate time.** It only remembers what something else told it.
It is a cold-start convenience, never the fix, and it is the only part of this
that would wait for Rev 3.

## Trust rules

A wrong clock is worse than no clock: it silently changes message expiry, stamp
validation and the age of every position on a map. So:

1. **Never let time run backwards.** Ordering depends on it.
2. **Bound the step.** A correction beyond a configured jump is refused and
   logged, not applied.
3. **Gate on the existing remote-management allow list.** Do not invent a new
   trust mechanism; only an allow-listed identity may set the clock, exactly as
   for any other privileged operation.
4. **Record provenance.** Which source, when, and what the correction was. When
   a timestamp is later disputed, this is the only way to answer it.
5. **Keep "unknown" expressible** all the way to the wire.

## Where the work lands

The offset belongs in `OS::time()`, which is **microReticulum, not this
firmware**. That means a fork change and a library PR, as `edab7c3` was, with the
pin bump that follows. Worth knowing before scheduling: the firmware-side work is
small, and the library-side change gates it.

---

# Part 2 -- Duty-cycle enforcement

## Current state

The accounting and the enforcement both exist and work. Enforcement is off by a
single default:

```cpp
// Config.h
#define RADIO_DUTY_CYCLE_LONGTERM 0.0f    // disabled -- set 0.01f to ship
```

Confirmed live on Rev 1: `longterm=0.0000 limit=0.0000 locked=0`.

## Why flipping the constant is not the feature

`tx_queue_handler()` gates the entire queue on `!airtime_lock`, and **the
transmit queue has no notion of priority**. Under lock, a distress message is
silenced exactly as a position beacon is.

That matters most for the workload TAK introduces. From
[`TAKCapability.md`](TAKCapability.md) §2, at SF7 / BW 250 kHz with a 1% duty
cycle -- 36 s of transmit per hour per node:

| Encoding | Airtime per report | Channel-wide capacity |
| --- | ---: | ---: |
| Raw CoT XML (~700 B) | 538 ms | 67 reports/hour |
| Compact packed position (~25 B) | 44 ms | 820 reports/hour |

A single node beaconing once a minute consumes a large share of its own budget.
Enable the limit as it stands and beacons will routinely be what holds the lock
when messaging needs it -- the failure the whole product exists to avoid.

## Design

Give the queue **priority classes**, then enable the limit:

- **Yields to the lock:** position beacons, telemetry, routine metrics.
- **Never yields:** direct messages, LXMF delivery, path and announce traffic
  needed to re-join the mesh.

Re-meshing a node quickly can be the difference that matters, so the budget
should be spent on getting a node back and on carrying what a person actually
sent -- not on a position that will be resent in sixty seconds anyway.

The limit itself then becomes honest to enable, and should be, because a node
that quietly exceeds its regional duty cycle is not shippable in the EU.

---

## Effort, in order

1. **`wall_offset` in `OS::time()`** plus `/time_offset` persistence, and a
   "time known" flag. Library change, then a pin bump. Small.
2. **NTP when a station link exists.** Near-free, and makes every lab board
   correct immediately.
3. **Time adoption from an allow-listed client.** The disaster path, and the one
   worth designing carefully rather than quickly.
4. **Priority classes on the transmit queue**, then set the long-term duty cycle
   to 0.01f.
5. **GNSS** only when a node must be correct with no phone and no Wi-Fi. A module
   on free GPIOs; still not a board respin.

Steps 1 and 2 are worth doing on their own merits and retire three deferred items
between them. Step 4 is the one not to rush: getting it wrong silences the
traffic that matters most.

## Acceptance

- A node with no time source reports "unknown" and continues to send 0 where it
  sends 0 today; nothing regresses into pretending.
- After adoption, LXMF announces a real timebase, RRC stamps real timestamps, and
  message expiry can be implemented against a clock.
- Time never moves backwards across an adoption, and a rejected correction is
  visible in the log with its provenance.
- With the duty cycle enabled and a beacon load applied, a message sent while the
  lock is held is still transmitted.

## What is deliberately not decided here

The **position rate policy** for TAK. `TAKCapability.md` §2 shows 10 nodes at one
report per minute fits and 25 does not; that is a product constraint to agree
before writing TAK code, not a number to discover in an exercise.
