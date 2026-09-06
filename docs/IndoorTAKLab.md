# Indoor TAK lab: full capability minus voice

The deck is a command post whenever there is a LAN, which indoors there always
is. That makes the whole TAK path testable now, on real hardware, months before
Vox exists — and indoors is where it should be debugged anyway.

Scoped 2026-09-06, correcting an earlier over-correction: dropping the central
command post from the *outdoor* exercise does not defer the TAK work, it defers
it **outdoors**. Indoors it is the next thing to test.

---

## 1. Two tracks, not one sequence

These answer different questions and share one codec.

| | Indoor TAK lab | Outdoor field mesh |
| --- | --- | --- |
| Question | Does the TAK path work at all? | Does the mesh degrade gracefully? |
| Command post | The deck, over the LAN | None |
| Client | ATAK-CIV, already installed | Columba's own map |
| Transport | LoRa and ESP-NOW, with Wi-Fi present | LoRa and ESP-NOW only |
| Validates | codec, gateway, ingest, chat, markers | failover, range, recovery |
| Blocked on | nothing | PR A |

Running the indoor lab first is not a detour. Every compact encoding it
exercises — position now, markers and chat next — is the same encoding the
outdoor exercise needs, and finding a wire-format bug across a room is
considerably cheaper than finding it across a field.

## 2. What can run today, with no new code

Everything for the position half already exists and has never been run
end to end:

- `tools/cot_gateway.py` on the deck. It prints its destination hash and
  announces so nodes learn a path to it.
- Columba on the phone, **Settings → Position Reporting**: toggle on, paste
  that hash, grant precise location.
- **ATAK-CIV** (`com.atakmap.app.civ`) is already installed on the A54. Point
  it at the deck on TCP 8087, or let it pick up multicast 239.2.3.1:6969 on
  the same LAN.

That is the first real test of the compact codec, the gateway, and the
announce-based destination discovery. So far the codec has only been proven
against synthetic fixes and its own two twins; nothing has carried a byte of it
over a radio.

Expect this to find something. It usually does.

## 3. What "full capability minus voice" still needs

Position is one direction. The rest of it:

### PR 2 — CoT ingestion, the command downlink

*Next.* The gateway listens for CoT from ATAK and carries it back into the
mesh, so the deck can task a node rather than only watch one. Compact on the
wire and unicast to the addressed node, the same discipline as position.

Everything arriving is untrusted until verified: a command that moves people is
exactly the payload worth forging, and the signing path built for the time
authority is there to be reused.

### PR 3 — Markers, both directions

A compact marker encoding — point, type, short label, timestamp — expanded to
CoT at the gateway and rendered natively in Columba. One encoding serving both
tracks: ATAK drops a marker indoors, Columba drops one in a field, the bytes on
the radio are the same.

### PR 4 — Chat

ATAK's GeoChat bridged to LXMF at the gateway. LXMF is the carrier because it is
store-and-forward and tolerates a node being unreachable, which is the whole
problem; raw CoT relay is not, because §2's table gives sixty-seven messages an
hour for the entire channel.

### Not in this track: voice

Nothing exists — RRC carries no audio and there is no codec in the firmware —
and over LoRa it would take the channel and starve the position reports the map
depends on. ESP-NOW could plausibly carry Codec2 at 3.2 kbps, which is three
percent of its measured 100 kbps, but that is a capability that appears and
disappears as people move and it needs its own analysis before it is designed.

## 4. What the indoor lab cannot tell you

Worth stating so the outdoor exercise does not get skipped on the strength of a
green indoor run:

- **Wi-Fi is present indoors**, so ESP-NOW is channel-locked to the access point
  rather than sweeping for a peer. The infrastructure-free discovery path that
  the field depends on is *not* exercised here.
- **The deck reaches the RADs over `UDPInterface` on the LAN**, not over LoRa.
  A working indoor test says nothing about the radio path to a command post.
- **Nobody moves far.** Failover, re-convergence and what is lost across a
  transition are the outdoor questions, and they are the ones that decide
  whether any of this survives a real deployment.

The indoor lab proves the application layer. The outdoor exercise proves the
network underneath it. Both are needed, and this one is available now.
