# TAK field exercise: two mobile, one command

The outdoor configuration we intend to test, what it needs that does not exist
yet, and which parts are scheduled into which PR.

Scoped 2026-09-06, from the intended exercise:

- **two field users**, each carrying one RAD paired over BLE to a phone running
  a TAK client;
- **one stationary command post** on desktop TAK (WinTAK or LinTAK);
- command can issue tasking, everyone can chat and drop markers;
- voice in a later PR.

Position reporting one-way -- phone to gateway to TAK -- is built
([`TAKCapability.md`](TAKCapability.md) §7). Everything else below is not, and
the point of this document is to be honest about which is which.

---

## 1. Three findings that change the plan

### iTAK on iOS cannot join the mesh

Columba is Android only. There is no iOS target, no Kotlin Multiplatform
sources, no shared module -- and no iOS Reticulum client we ship or intend to.
An iPhone running iTAK therefore has **no way onto the Reticulum mesh at all**.
Its BLE link to a RAD does not exist either; the BLE interface is Columba's.

So an iOS field user is reachable only over IP. That is a real configuration
and worth testing, but it is the arm of the experiment where the mesh is doing
nothing.

**Both field users should be on Android for the mesh-backed exercise.** Keep an
iOS handset if you want the IP-only comparison, and label it as such rather
than expecting it to degrade gracefully when the network goes.

### Tailscale is IP, and IP is the thing the mesh exists to replace

If the field phones can reach a tailnet, they have working internet. At that
point WinTAK and ATAK talk to each other natively over TAK's own protocols, and
neither Reticulum, nor the compact position codec, nor the CoT gateway carries
anything. The exercise would pass while testing none of the parts that matter.

That does not make Tailscale wrong -- it makes it a *second* transport, and the
interesting measurement is what happens when it stops. See §3 for the shape
that gets both.

### Chat and markers are not position

A position report is twenty bytes because it was designed to be. A GeoChat
message or a dropped marker is arbitrary CoT XML -- hundreds of bytes to a few
kilobytes -- and §2's airtime table applies to it unchanged: seven hundred bytes
is 538 ms on air and sixty-seven messages an hour channel-wide, shared between
everybody.

Chat over the mesh has to be **LXMF**, which is what LXMF is for:
store-and-forward, tolerant of a node being unreachable, and rated "best" under
mobility in §4. The gateway then bridges LXMF to GeoChat rather than relaying
raw CoT. Markers need the same treatment as position did -- a compact encoding,
expanded at the gateway.

Relaying raw CoT over LoRa is the one design that cannot work, and it is also
the most obvious thing to try.

---

## 2. What the outdoor setup needs that the indoor one does not

The deck's current interfaces are LAN-bound and will not survive leaving the
building:

```
[[RAD-01 Rev1]]  type = UDPInterface  forward_ip = 192.168.1.54
[[RAD-01 Rev2]]  type = UDPInterface  forward_ip = 192.168.1.88
```

Both reach the RADs over Wi-Fi. In a field there is no Wi-Fi, so the command
post needs **its own RAD on USB serial** and a matching interface:

```
[[Command RAD]]
  type = RNodeInterface
  enabled = Yes
  port = /dev/ttyUSB0
  frequency = <fleet frequency>
  bandwidth = 250000
  txpower = <within the 21 dB gain limit>
  spreadingfactor = 7
  codingrate = 5
  id_callsign = <as provisioned>
  id_interval = 600
```

And ESP-NOW is not an answer here: it is channel-locked to a station's AP, so
it cannot mesh across a field. LoRa is the backbone outdoors, and the only one.

---

## 3. The shape that tests both

Run the two transports side by side and make the mesh the fallback, so the
exercise can be degraded on purpose rather than only working when the network
does.

```
  field                                    command post
  ┌──────────────┐                         ┌──────────────────┐
  │ ATAK         │                         │ WinTAK / LinTAK  │
  │  ↕ CoT/IP    │                         │  ↕ CoT/IP        │
  │ Columba      │═══ Tailscale (when up) ══│ cot_gateway.py   │
  │  ↕ BLE       │                         │ rnsd             │
  │ RAD ─────────┼─── LoRa (always) ───────┼─ Command RAD     │
  └──────────────┘                         └──────────────────┘
```

**Tailscale arm.** The phone runs Columba as a transport node with a
TCPServerInterface bound to its tailnet address; the command post's `rnsd`
connects in as a TCPClientInterface. The phone then bridges tailnet to LoRa,
and the command post is on the mesh without needing radio range to it. Two
changes are required and neither is made:

- Columba writes `enable_transport = No`; this needs to be `Yes` on a phone
  acting as a bridge, and it costs battery.
- Columba has no UI for adding a TCPServerInterface on a chosen address.

**LoRa arm.** Unchanged and always on. When Tailscale drops, paths re-converge
through the radio and position keeps arriving -- slower, and only position
until chat and markers are built.

The measurement worth taking is the transition: how long after the tailnet goes
before the command post has a current picture again, and what it loses.

---

## 4. Scheduled work, in order

Each of these is its own PR. The first is the one that makes "command can issue
commands" true at all.

### PR 2 — CoT ingestion: the command downlink

The gateway is one-way today. This adds the other direction.

- The gateway listens for CoT from the command post's TAK client (TCP in, and
  the multicast group it already sends to).
- Tasking is encoded compactly, the same discipline as position, and unicast to
  the addressed node rather than broadcast.
- Columba gains a receiver and surfaces what arrives.
- Everything that arrives is untrusted until checked: a command that moves
  people is exactly the payload worth forging, and the signing path from the
  time authority already exists to be reused.

### PR 3 — Field transport

The gaps in §2 and §3: the command post's `RNodeInterface`, transport mode and
a TCP interface on Columba, and the failover measurement between the two arms.

### PR 4 — Chat and markers

LXMF as the carrier for chat, bridged to GeoChat at the gateway. A compact
marker encoding alongside the position one. Explicitly *not* raw CoT relay.

### PR 5 — Voice

Nothing exists today: RRC carries no audio, and there is no codec anywhere in
the firmware. Over the tailnet voice is ordinary. Over LoRa it is a research
question -- SF7/BW250 is 10.9 kbps raw before Reticulum's framing, so even
Codec2 at 3.2 kbps would take the channel and leave nothing for the position
reports the map depends on. That trade needs measuring before it is designed,
and it should not block the exercise.
