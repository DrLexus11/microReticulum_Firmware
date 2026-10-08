# Outdoor Test 1: the procedure

Range, disconnection and reconnection, with a mission executable at the far
end (`TAKDeliveryPlan.md`, Outdoor Test 1). First run: vehicles, 2026-10-10.

This file is the procedure. **The site, the route, the people and their
callsigns go in the local site sheet, `~/.impr-tak/ot1/site-sheet.md`, never
here**: this repository is public until the private pivot, and exercise data
stays out of public repositories.

---

## 1. What is being tested, and what is not

Being tested, on the soaked build (firmware `b775d4d` stack, library
`9e83d72`):

1. **Range:** how far the LoRa link holds between a moving vehicle and the
   command post, and at what RSSI and SNR it fails.
2. **Disconnection:** what the operator sees when a node leaves range, and how
   long until the command post notices.
3. **Reconnection:** how long after re-entering range until positions, paths
   and telemetry resume, unattended.
4. **A mission at the far end:** a message and a marker sent from the far node
   reach the command post, and an order from the command post reaches the far
   node -- including one sent while it was out of range (store and forward).

**Not being tested**, and expected to show as gaps rather than failures:

- **Telemetry store-and-forward (T2) is not built.** A board with no path to a
  gateway loses its reports; Grafana shows a gap until it is back.
- **Detail reports come every 30 minutes.** RSSI, neighbours and the topology
  map update at that pace; health reports (uptime, hops, heap) every 5 minutes.
- **No field configuration, no channel escape, no rendezvous.** Every node
  runs the fleet's default LoRa settings and IFAC network.
- **BLE telemetry over a phone** is unproven on the bench; a node that reaches
  the mesh only by BLE may report nothing.

## 2. Topology

```
  command post (vehicle or fixed)              far end (vehicle)
  ┌───────────────────────────────┐            ┌──────────────────────┐
  │ deck: rnsd, bridge, gateway,  │            │ RAD (casing or bag)  │
  │   dev telemetry stack offline │            │   ↕ BLE / Wi-Fi      │
  │ RAD ─── LoRa ─────────────────┼────────────┼── phone: Columba,    │
  │ phone: ATAK + Columba         │            │   ATAK               │
  └───────────────────────────────┘            └──────────────────────┘
```

- The deck runs its **dev telemetry stack** (Mosquitto, backend, Prometheus,
  Grafana on 127.0.0.1:3000). It works with no internet: the gateway, broker,
  backend and Grafana are all local. If the deck has internet, the MQTT bridge
  also forwards to production; if not, the bridge queues and catches up later.
- **The deck must not sleep** during the test (it does overnight by choice).
  Disable suspend for the day.
- A **propagation node** runs at the command post (lxmd on the deck, and the
  boards' own), so a message to an out-of-range node is held and delivered on
  return (`tools/lxmd_propagation.example.conf`).

## 3. Preflight, the day before

| # | Check | How | Pass |
| --- | --- | --- | --- |
| P1 | Every field board on the soaked build | Grafana boards table, Firmware column | the same hash on every board |
| P2 | Soak verdict | spare's uptime and restarts in Grafana; core dump read if it restarted | no restart in 48 h |
| P3 | Every board reports | Grafana: each board green within 15 min | all green |
| P4 | Clocks synced | boards table, Clock column | "synced" everywhere |
| P5 | Carriers up | "Carriers up" timeline; topology map | LoRa up on every board |
| P6 | Phone to board | Columba on each phone sees its RAD (BLE or Wi-Fi) | a message round trip |
| P7 | Store and forward | send to a phone with Bluetooth off; turn it on; it arrives | delivered unattended |
| P8 | Power | each board and phone on its field supply for an hour | no brownout restart |
| P9 | Weather protection | casing or sealed bag per board; antenna outside | -- |
| P10 | Deck offline | Wi-Fi off: Grafana still loads and updates | yes |
| P11 | Airtime and gain | gain 21 on every board; the operator confirms the lab approval covers the site | confirmed |

## 4. Runs

Each run starts from a **baseline**: both vehicles at the command post, every
board green on Grafana, a message round trip done. Note the clock time at each
step on the site sheet; Grafana and the logs are matched to it afterwards.

### R1 -- range, outbound

The far vehicle drives the route outward at a steady pace, stopping at marked
points. At each stop: a Columba message from the far phone to the command
post, and its delivery time.

**Record:** the last stop where a message arrives directly; the first where
it does not. The RSSI and SNR the boards report for that link (detail report,
topology map) as it degrades.

**Expect:** delivery in seconds while in range; at the edge, delivery
escalates to the propagation node after 140-180 s (measured over LoRa,
2026-09-13), not instantly.

### R2 -- disconnection

At the first stop out of range, the far vehicle waits **10 minutes**.

**Record:** when the command post's ATAK shows the far node's position as
stale; when Grafana shows the far board silent (counted silent after 15 minutes; the alert fires after 30);
when the far node's links turn amber on the topology map.

### R3 -- store and forward, out of range

While the far node is out of range, the command post sends it an order
(a Columba message and an ATAK chat).

**Record:** that the sender sees "held" rather than "delivered"; the message
is waiting at the propagation node.

### R4 -- reconnection, unattended

The far vehicle drives back toward the command post. **Nobody touches a
device.**

**Record:** the time from re-entering range (the R1 edge point) until, in
order: the far node's position updates at the command post; the order from R3
arrives at the far phone without anyone pressing sync; Grafana shows the far
board reporting again.

**Expect:** paths re-form within one announce interval; the held message
arrives when the far node next syncs. Whether that happens unattended is
exactly what the bench could not prove (TAKDeliveryPlan, *A node that comes
back does not ask what it missed*) -- record what happens, either way.

### R5 -- a mission at the far end

From a stop well inside range: the far phone drops an ATAK marker and sends a
chat; the command post answers with an order and a marker.

**Record:** each arrives, with times, on both sides.

### R6 (if time allows) -- relay

A third board placed between the two, so the far vehicle is out of direct
range but in range of the relay.

**Record:** the hop count Grafana shows for the far board (2 through the
relay); message delivery through it.

## 5. What to bring back

- The site sheet, with times.
- Grafana covers itself: Prometheus on the deck keeps every report; the topology
  map and the boards table can be replayed for any minute of the day.
- Deck logs: `~/.impr-tak/telemetry-gateway.log`, the bridge log, lxmd's log.
- If a board restarted: do not reflash it. Its core dump holds the panic
  (`coredump` partition); read it first (`CarriedIssues.md`, *a panic when a
  cached packet was replayed*).

## 6. Stop conditions

- Rain reaching a board without a casing.
- A board restarting more than once: stop, keep it powered, read its core dump
  at the end of the day.
- Any doubt about the airtime or gain approval for the site.
