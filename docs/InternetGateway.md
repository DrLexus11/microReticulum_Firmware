# Internet Gatewaying over the Mesh

Whether a RAD with internet access can share it with nodes that have none, how
Reticulum approaches the question, and what is actually worth building.

Status: **analysis, nothing implemented.** Written 2026-08-28.

---

## 1. The short answer

**Not in the way the question usually means, and that is a deliberate property of
Reticulum rather than a gap.** A mesh node cannot be given "internet access"
through a neighbour, because Reticulum does not carry IP.

**But the useful version is very achievable**: a node with connectivity can
expose internet-backed *services* as Reticulum destinations, and every node on
the mesh can reach them. That is gatewaying at the application layer, and it is
the only version that survives the bandwidth arithmetic in §3.

## 2. What Reticulum actually does

Reticulum is not a tunnelling or NAT layer. It has its own addressing --
destination hashes derived from public keys -- its own routing through transport
nodes, and end-to-end encryption throughout. There is no IP header anywhere in
it, no DNS, no default route, no concept of a gateway address.

The interfaces confirm the direction of travel. Every one of them --
`TCPClientInterface`, `TCPServerInterface`, `I2PInterface`, `UDPInterface`,
`RNodeInterface`, `AutoInterface` -- carries *Reticulum over some medium*. None
routes IP over Reticulum. `TCPInterface` connects two Reticulum instances across
the internet; it does not hand internet access to the far side.

So "share my internet with the mesh" has no native expression. What Reticulum
offers instead is **reachability**: any node can reach any destination anywhere
on the Reticulum network, including destinations hosted on machines that do have
internet. Our own lab already relies on this -- Rev 2 reaches the Deck's
services through Rev 1 over LoRa without Rev 2 having any network of its own.

Tunnelling IP over a Reticulum Link is *possible* in principle -- a Link is a
reliable encrypted byte channel, and nothing stops someone bridging a TUN device
across it. It is a bad idea here for the reason in the next section, not for a
protocol reason.

## 3. Why general internet access is not on the table

Measured at our working point, SF7 / BW 250 kHz / CR 4:5:

| | |
| --- | --- |
| Raw channel rate | **10.9 kbps** |
| Legal transmit budget at 1% duty cycle | **36 seconds per hour, per node** |
| Payload that buys, channel-wide | **~48 KiB per hour** |

Against that budget:

| Content | Airtime | Share of the channel's *hourly* budget |
| --- | ---: | ---: |
| One LXMF text message (300 B) | 0.2 s | 0.6% |
| A NomadNet page (2 KB) | 1.5 s | 4% |
| A modest web page (500 KB) | 366 s | **10 hours** |
| One phone photo (3 MB) | 2194 s | **61 hours** |

A single ordinary web page costs ten hours of the entire channel's lawful
transmit budget, shared by every node in earshot. Web browsing over this mesh is
not slow; it is arithmetically impossible. Any design that implies otherwise
will fail in the field and take the product's credibility with it.

Note the duty cycle is currently unenforced in the firmware
(`RADIO_DUTY_CYCLE_LONGTERM = 0.0f`). That makes the numbers above look
pessimistic on a bench and exactly right the moment anything ships in the EU.

## 4. What is worth building

Gateway *services*, not connectivity, sized to text:

1. **LXMF bridge to email or SMS.** An internet-connected node runs an LXMF
   propagation node and a bridge: messages addressed to a bridge destination are
   relayed outward, replies come back as LXMF. A few hundred bytes each way,
   entirely within budget, and it is the single highest-value gateway for a
   disaster deployment -- it reaches people who are not on the mesh at all.
2. **Curated inbound feeds.** Emergency bulletins, utility status, shelter
   locations, pulled by the gateway and published as NomadNet pages or pushed as
   LXMF. Pull once at the gateway, serve many times over the mesh, instead of
   every node fetching.
3. **A narrow fetch service.** A request handler that retrieves an allowlisted
   URL, strips it to text, truncates hard, and returns it. Useful for a
   responder who needs one specific page. Must be allowlisted and size-capped --
   an open fetch proxy on a shared 10 kbps channel is a denial-of-service
   primitive with extra steps.

All three are ordinary Reticulum destinations with request handlers. Nothing new
is needed in the protocol, and nothing changes about how nodes reach them.

## 5. Where it belongs: the blackbox, not the RAD

This is the blackbox's job as described in `docs/Messaging.md`: Linux,
mains-powered, always on, and the node that actually has connectivity. It already
has to run `lxmd`; a bridge and a feed fetcher sit naturally beside it.

The RADs stay what they are -- transport and local attachment. They do not need
to know a gateway exists, because reaching it is just reaching another
destination.

A RAD *could* host a small gateway if it had a WiFi uplink to a working router,
and that is worth remembering for a building whose network survives while the
wider area is cut off. But it should not be the design centre: the RAD's value
is that it keeps working when infrastructure does not.

## 6. What this does not solve

Be plain with anyone evaluating this. A mesh gateway does not give a phone on a
node's SoftAP a working internet connection. Browsers will not work, apps will
not sync, and video calls are not in the same universe as the numbers in §3.
What it gives is **messages in and out of a disconnected area, and small
amounts of curated information flowing in** -- which in an actual incident is
most of what matters, and is worth saying in those terms rather than as
"internet over the mesh".
