# Radio roadmap: Rev 3 antenna diversity, Rev 4 dual radio, HaLow

Written 2026-09-21 for whoever picks up the Rev 3 and Rev 4 RAD boards. It
answers one question: how to get more link out of more antennas without
breaking a single node already in the field.

The short version:

- **Rev 3 -- switched antenna diversity.** Two antennas per radio, chosen per
  neighbour. Invisible to every other node. No protocol work.
- **Rev 4 -- two LoRa transceivers, receive diversity.** Both radios decode on
  the same channel and the node keeps whichever copy decodes. Also invisible,
  and the second radio can later listen on baseline while the first tries an
  enhanced mode, or serve a second band.
- **True 2x2 MIMO with STBC and spatial multiplexing on LoRa is not
  recommended.** No LoRa silicon does it, spatial multiplexing is the wrong tool
  in LoRa's SNR regime, and the only route is an SDR-class board.
- **HaLow already has what LoRa would need built**: MIMO, STBC and per-peer
  capability negotiation are in 802.11ah. Today's modules, as far as we know,
  are single-stream.
- **In Reticulum, capability-based delivery lives in path selection**:
  microReticulum's Transport should prefer the faster interface on a node that
  has more than one. That is our library and in scope.

The constraint that shapes all of it: **no node that is already deployed may
stop hearing anything it hears today.**

## Rev 3: switched antenna diversity

Two antennas per LoRa radio behind an SPDT RF switch, the switch driven from a
GPIO, the choice made per packet by firmware.

**Receive.** A LoRa transceiver cannot switch antennas partway through a
preamble and compare them, so the choice is per neighbour, learned over time:
record RSSI and SNR for every received packet against the antenna it arrived
on and the neighbour it came from, and listen on the antenna that has been
hearing the expected neighbour best. With nothing expected, listen on the
antenna with the best recent average.

**Transmit.** Send on the antenna that last received that neighbour best. The
path is reciprocal -- same frequency, same geometry -- so the antenna that hears
a peer well is the one that reaches it.

**Retries alternate antennas.** LXMF already retries every message, and now
every tier-3 fragment. A retry on the other antenna rolls independent fading
dice at no cost: in an urban canyon, where the first attempt fails because of a
fade specific to one antenna's position, the second is the one most likely to
succeed.

**Why it is the easy win.** The modulation does not change, so no other node
can tell. No capability exchange, no protocol version, no fallback path. It is
firmware in the LoRa interface layer and a map from next hop to antenna.

**Regulatory.** The gain limit (21) applies per antenna; switching between two
antennas does not add their gains.

## Rev 4: two transceivers

The request was 2x2 MIMO with STBC and spatial multiplexing on LoRa. That is not
recommended, for three reasons:

1. **No LoRa chip does MIMO.** SX126x, SX127x and LR11xx each have a single RF
   path and a single-stream chirp spread spectrum modem. STBC for LoRa exists in
   research on software-defined radios with custom modems, at a power budget far
   outside a pocket node, and it interoperates with nothing.
2. **Spatial multiplexing is the wrong tool for LoRa.** It trades SNR for
   capacity and pays off on high-SNR links. LoRa is designed to operate near and
   below the noise floor, where extra antennas help through **diversity** --
   range and reliability -- not through multiplexing.
3. **It would break compatibility.** A non-standard modulation cannot be heard
   by any existing node.

**What delivers most of the benefit with real chips: two transceivers on the
same channel, each on its own antenna, both decoding.** The node keeps whichever
copy decodes -- packet-level selection combining. In multipath fading that is a
substantial reliability gain, and like Rev 3 it is invisible to every other node.
Combine it with Rev 3's per-neighbour antenna choice on transmit, and alternate
on retries.

The second radio is also what makes anything beyond that possible:

- **A baseline listener.** A single-radio node can only listen with one set of
  radio settings, so it cannot hear an enhanced mode while it listens on the
  baseline everyone uses. With two radios, one always listens on baseline.
- **A second band.** 2.4 GHz LoRa, or HaLow, as a second Reticulum interface.

**Regulatory.** If true array or beamforming gain is ever pursued, combined
array gain generally counts toward EIRP.

## Capability-based delivery, backward compatible

For anything beyond Rev 3 and Rev 4's transparent diversity:

- **Baseline for everything everyone must hear.** Announces, path requests and
  broadcasts stay on the baseline LoRa settings permanently. This is what keeps
  every deployed node on the mesh.
- **Enhanced modes are unicast only**, to a neighbour that has advertised
  support, with fallback to baseline after a failed attempt.
- **Where capability is advertised.** A board already announces its own
  destination; its radio capabilities ride in that announce's app data, and the
  firmware keeps a per-neighbour capability table. A node that advertises
  nothing is treated as baseline.
- **Where capability-based delivery lives in Reticulum.** RNS chooses paths by
  hop count, not by bitrate. On a node with LoRa and HaLow, preferring the faster
  interface when both reach a destination is a change to microReticulum's
  Transport -- our library, so in scope. It also connects to D2's measured fetch
  gate and PR F's cost display (see `docs/TAKDeliveryPlan.md`), because the path
  table's first hop can hide a slow leg further on: from the deck, the first hop
  is 10 Mbps UDP even when LoRa is two hops away.

## HaLow

**The standard already has what LoRa would need to be built.** 802.11ah
defines MIMO up to four spatial streams, STBC and beamforming -- and, crucially,
capability negotiation and backward compatibility in the MAC: capabilities are
exchanged when a station associates, rates adapt per peer, and a MIMO node talks
single-stream to single-stream peers automatically. Reticulum does not need to
know any of it; it rides IP on top.

**The silicon, as far as we know, is single-stream.** The Morse Micro MM8108 on
Vox-mini, the MM6108, and Newracom's NRC7394 are believed to be one spatial
stream. Verify against the MM8108 datasheet (linked in the vox-mini README), and
check whether the vendor firmware supports antenna diversity; if it does, Rev
3's approach carries straight over.

**What HaLow gives today, with no work from us:** per-link rate adaptation from
BPSK to 256-QAM across 1, 2, 4 and 8 MHz channels -- the capability-based rate
selection this document is otherwise asking for, handled by the standard.

## Recommended order

1. **Rev 3:** antenna diversity, per-neighbour selection, alternate on retry.
2. **microReticulum:** prefer the faster interface on multi-interface nodes.
3. **Rev 4:** dual-radio receive diversity; the second radio as baseline
   listener or second band.
4. **HaLow:** let the standard negotiate; confirm the module's stream count and
   antenna support.
5. **LoRa STBC or spatial multiplexing:** leave to research unless an SDR-class
   board is wanted.
