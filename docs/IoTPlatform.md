# Layered firmware -- the platform architecture

**Status:** agreed 2026-09-28 (operator). Adopted gradually, PR by PR -- see
*Migration* below and `TAKDeliveryPlan.md`. Once a layer exists, its rules are
binding on new code (`CLAUDE.md`, *Architecture*).

## Why

The firmware carries a tactical product (IMPR-RAD, then IMPR-Vox) and is to
become the base of IoT products: a product firmware should be *our Reticulum
core plus its own services*, not a fork that drifts the way ours drifted from
the original. Today everything runs from one 4 480-line `RNode_Firmware.ino`
with 175 `#if` blocks, configuration is 394 build flags plus EEPROM plus
provisioning, and modules share state through globals. That works for one
product and cannot be extended by another without editing the core.

**Linux compatibility is a constraint at every layer, always.** IMPR-Vox runs
Linux (Pi Zero 2 W); the repository already builds natively (Portduino,
`native/`). Nothing above the platform layer may depend on FreeRTOS, Arduino,
ESP-IDF or POSIX.

## The layers

Dependencies point **down** only. A layer may use any layer below it and none
above it.

```
  Application      order, config, fail-fast, wiring, restart policy
       |
  Services         Reticulum core, radio links, propagation, RRC, provisioning,
       |           telemetry, product services  -- one contract, one lifecycle
       |
  Drivers          radio (ILoRaRadio), BLE, display, sensors, storage media
       |
  Platform         time, storage, serial/log, net, threads, mailboxes, reboot
                   -- ESP32 (Arduino) and Linux (native) implementations

  Protocol         codecs and wire formats (position, TAK, telemetry, files):
  (beside all)     pure functions, no I/O, no allocation after construction;
                   usable from any layer, depends on nothing but the standard
                   library. Pinned by shared fixtures across repositories.
```

### Application

- Owns **start order** and **dependencies between services** (declared, not
  implied by call order in `setup()`).
- Loads **configuration** once, as typed structs with defaults and validation.
  Every value has a default, a valid range, and a source (build flag,
  provisioning, runtime). Invalid **required** configuration fails fast at
  boot and says so loudly (the NOT RELAYING warning is the model); invalid
  **optional** configuration disables that service and reports it degraded.
- Decides **fault handling**: a service reporting *failed* is restarted with
  backoff; one that keeps failing is disabled and reported, not allowed to take
  the node down.
- Wires cross-service communication: which mailbox connects which services.
  Services do not find each other.

### Services

One contract for everything that runs:

```cpp
class IService {
public:
  virtual const char* name() const = 0;
  virtual bool init(const AppContext&) = 0;   // allocate everything here
  virtual bool start() = 0;
  virtual void poll(uint32_t now_ms) = 0;     // bounded: see time budget
  virtual Health health() const = 0;          // Starting/Healthy/Degraded/Failed + reason
  virtual void telemetry(TelemetryWriter&) const = 0;
  virtual void stop() = 0;
};
```

- **Lifecycle** is the application's to drive: `init` -> `start` -> `poll`...
  -> `stop`. A service never starts itself.
- **Health** is reported, not inferred: a state and a short reason
  ("no peer 2 min", "radio not responding"). The application acts on it; the
  telemetry service ships it.
- **Telemetry** is each service's own, written into a shared writer in the
  telemetry codec -- the mesh-health view is the sum of the services' reports.
- **Communication** is through mailboxes and events the application wires, not
  globals. Shared read-only objects (identity, config) arrive in `AppContext`.
- **Threads** are the exception. Services are polled cooperatively from one
  loop; Reticulum has one owner (it is not thread-safe). A service that must
  block (a HaLow or MQTT client on Vox) asks the platform for a thread and
  talks to the rest through mailboxes.

### Drivers

- One interface per kind of device, implementations per chip. `ILoRaRadio`
  (sx126x / sx127x / sx128x) is the model already in the tree.
- Drivers know hardware, not Reticulum: a radio driver moves frames; the LoRa
  *service* is what makes it a Reticulum interface.
- The same driver runs on both platforms where the bus exists: an SX1262 on SPI
  is the same code on an ESP32 board and on a Pi HAT.

### Platform

- The only layer that touches the OS or framework. Two implementations: ESP32
  (Arduino core today) and Linux (native). `native/reboot.h` is an early
  example of what belongs here.
- Provides time, persistent storage, serial and logging, networking, threads
  and mailboxes, watchdog feed, reboot and reset reason.

### Protocol

- Every wire format -- position, TAK payloads, file parts, telemetry, status
  lines -- as pure encode/decode functions with a version byte, tested on the
  host and pinned by fixtures shared with the other repositories (Columba, the
  telemetry backend). This is where the Python/Kotlin byte-identical rule
  lives on the firmware side.

## Rules drawn from this week's failures

These are part of the service contract, not advice:

1. **Time budget per poll.** A `poll()` returns within its budget (default
   20 ms; a service states a larger one if it must). The loop measures every
   poll and reports overruns in telemetry. The TASK_WDT resets in
   CarriedIssues #1 are this failure class: something held the loop.
2. **Allocate at init, not in the steady state.** Buffers, queues and tables
   are sized and allocated in `init()`; `poll()` does not allocate on the heap
   in its normal path. Rev 2-2 fragmented until it restarted -- largest block
   23 KB to 1.3 KB in a night -- with every table flat: steady-state churn.
   A service that must allocate at runtime uses a pool it owns.
3. **Logging is a service with a budget.** Formatting on the heap and
   `Serial.flush()` per line are not free; the runtime level defaults to
   NOTICE in field builds, and nothing is formatted below the active level.
   Every persisted log has a bound and a rotation policy -- one policy, owned
   by the platform layer -- and the log is shippable: a bounded spool the
   telemetry service drains off the device when it has a connection, boot and
   crash records first, rate-limited to the carrier's budget.
4. **Health is measured.** No service reports healthy from a flag it set
   itself; health comes from traffic, peers, or a successful operation within
   a window. Declared bitrates never count (`CLAUDE.md`, interface
   completeness).
5. **Loud on misconfiguration.** A service disabled by configuration says so
   at boot and in telemetry, in one line.

## Extension -- a product firmware

A product depends on the core rather than forking it: the core becomes a
PlatformIO library and a native (Linux) library, and a product firmware is an
application that registers the core's services and then its own, e.g.

```cpp
app.add(reticulum_core(cfg.rns));
app.add(lora_link(radio, cfg.lora));
app.add(ble_peers(ble, cfg.ble));
app.add(telemetry(cfg.telemetry));
app.add(my_sensor_service(bme280, cfg.sensor));   // the product's own
app.run();
```

The product's service speaks Reticulum through the core's services -- its own
destination, telemetry through the telemetry service -- and gets multi-radio
meshing, store-and-forward and identities without touching them.

## Data formats

- **Over the mesh: binary, never text.** On LoRa text costs 3-10x the airtime.
  Small frequent reports (position, core telemetry): fixed layouts with a
  version byte, as the ~20-byte position report is. Richer and application
  telemetry: **CBOR** (RFC 8949) with integer keys -- `tinycbor` is already
  linked, Go reads it natively, it gains fields without breaking readers and
  needs no code generation. Protobuf (nanopb) is the alternative if strict
  schemas and generated code are wanted.
- **gRPC does not fit the mesh**: it is request/response over a live HTTP/2
  connection.
- **Past the gateway: JSON** on MQTT, following Sparkplug B's topic layout and
  birth/death messages for node liveness.

## Migration -- gradual, one step per PR

No refactor-only cycle. Each step ships inside a PR that needs it, changes no
behaviour on its own, and is covered by tests before the next builds on it.

| Step | What | In |
| --- | --- | --- |
| R0 | This document; the rules into `CLAUDE.md`; new code follows the layers where they exist | PR E |
| R1 | **Firmware unit tests on Linux** (PlatformIO native + Unity): the codecs first, then the pieces the next steps move | PR F, first |
| R2 | **Protocol layer**: move the firmware's codecs (position, telemetry) into pure, host-tested modules | PR F, with telemetry |
| R3 | **`IService` contract**, wrapped around today's modules and called from today's loop in declared order; poll timing and health measured. No behaviour change | PR F, with telemetry -- each service reports its own |
| R4 | **Platform layer** extracted from the ESP32 and native code already in the tree | after PR F, platform track |
| R5 | **Application layer**: typed config with defaults and validation, start order, restart policy; `RNode_Firmware.ino` reduced to wiring | platform track |
| R6 | **Core as a library**; the first product firmware built on it | platform track, with the Arduino-core / ESP-IDF 5 upgrade (after R1's tests exist) |

Until a layer exists, new code is written so it can move into it: pure codecs
in their own files, hardware behind an interface, no new globals for sharing
between modules, no new blocking in the loop.
