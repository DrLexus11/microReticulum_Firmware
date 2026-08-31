# A MANET Link Layer Bridged to Reticulum

Whether to add self-forming peer networking beneath Reticulum for the command
backbone, and what shape it should take.

Status: **ESP-NOW one-hop interface implemented; hardware acceptance pending.**
Written 2026-08-28 and implemented 2026-08-31. The wire and runtime contract is
in [ESPNowPeerProtocol.md](ESPNowPeerProtocol.md).

---

## 1. The instinct is right, and it is idiomatic rather than a workaround

The concern behind the question -- that Reticulum resists encapsulation and
tunnelling, so we should not fight it -- is correct, and the proposed answer is
the one Reticulum is designed for.

Reticulum's model is that **interfaces are dumb pipes and Reticulum does the
routing and cryptography**. An interface's only job is to move frames between
adjacent nodes. Adding a new interface whose transport happens to be a
self-forming radio link is not a violation of that model; it is the model. It is
the same category of change as `TCPServerInterface`, which we already added.

So: a MANET layer presented as an interface is **not** encapsulation. Nothing is
wrapped in anything Reticulum objects to. Reticulum keeps its addressing, its
end-to-end encryption, its transport nodes and its path discovery, entirely
unchanged.

## 2. The discipline that decides whether this succeeds

**Form links. Do not route.**

If the MANET layer performs multi-hop routing *and* Reticulum performs multi-hop
routing, there are two routing systems stacked on one topology. That produces
route stretch, duplicate delivery, loops that neither layer can see, and failures
that are close to undebuggable -- each layer behaving correctly while the system
does not.

Reticulum's multi-hop routing is its core competence and we have proven it works
across mixed media: a client reaching Rev 1's hub through Rev 2 over LoRa at two
hops, verified repeatedly. There is no reason to duplicate it and every reason
not to.

So the layer should do exactly two things:

1. **Neighbour discovery** -- who else is in radio range, right now.
2. **One-hop link formation** -- a frame path to each neighbour.

Then hand every frame to Reticulum and let it decide where things go. "MANET"
here should mean **auto-peering, not auto-routing.**

## 3. The gap this actually fills

Today two RADs in the same room, with no router, **cannot use WiFi to talk to
each other at all.**

The SoftAP fallback makes a node an access point *for clients* -- phones. It does
not peer nodes. Two nodes without infrastructure fall back to LoRa alone: 10.9
kbps shared, with a 1% duty cycle capping each of them at 36 seconds of transmit
per hour.

Over WiFi the same hop is megabits, with no duty cycle. For a building-scale
command backbone -- which is the stated use -- that difference is not an
optimisation, it is the difference between a chat room and a command net. LoRa
then becomes what it is good at: the long link between buildings, carrying what
must cross it rather than everything.

## 4. Options, and what each costs

| Approach | Fit | Cost |
| --- | --- | --- |
| **ESP-NOW** | **Best.** Connectionless, no association, broadcast discovery is native, one-hop by nature so it cannot fight Reticulum's routing. | 250-byte payload against Reticulum's larger MTU, so fragmentation is required. Peers must share a WiFi channel. |
| **AP+STA concurrent** | Workable. Gives real IP, so `UDPInterface` works unchanged. | Role and channel juggling; on ESP32 the STA and AP share a channel, so joining a router constrains the peer link. |
| **ESP-WIFI-MESH** | **Avoid.** | It builds and maintains its own routing tree, which is precisely the stacked-routing failure in §2. Already linked into our image at roughly 219 KB, and unused. |

ESP-NOW is the recommendation: smallest, no IP stack, discovery for free, and
structurally incapable of the routing conflict.

Two honest constraints:

- **Channel coupling.** ESP-NOW operates on the current WiFi channel. A node
  associated to a building router on channel 6 can only peer with nodes on
  channel 6. In the disaster case -- no router, nodes free to pick a common
  channel -- this is fine, and that is the case the feature exists for. When the
  building network is healthy the nodes are already on it and do not need this.
- **No second transport-security system is needed.** Reticulum encrypts end to
  end regardless of interface. Admission control is IFAC's job; the implemented
  interface reuses the LoRa/backbone IFAC credentials and secure-node policy so
  it cannot remain an open ingress when the radio backbone is locked down.

## 5. What the flash work bought

This is now affordable. The application partition sits at roughly 44% of 4 MB
after the repartition, where it was 90% of 2 MB. An ESP-NOW interface plus
fragmentation is small, but a year ago it would not have fitted alongside the
Bluetooth stack. Worth noting that the headroom work paid for something concrete
rather than being tidiness.

## 6. On improving the protocol itself for command use

Separately from the link layer, the question was whether the *protocol* should
change to suit group command. The most valuable change is one already in the
backlog rather than anything new.

**RRC is ephemeral by design.** Membership and messages exist only while clients
are connected; the hub holds nothing. That is correct for incident chat and wrong
for a command net, where the question "what did I miss?" is the whole point --
and where mobility means clients *will* drop, since long-lived Links are the part
of that stack most sensitive to path change.

The fix is `docs/RRCRequirements.md` deferred item 6: **an explicit RRC-to-LXMF
bridge.** Room traffic is also delivered as LXMF, so it is stored by a
propagation node and reaches members who were absent. That gives persistence,
store-and-forward and offline delivery without inventing a protocol, and without
quietly changing RRC's ephemeral contract for the clients that expect it.

Resist adding history to RRC itself. The ecosystem already has a
store-and-forward layer that stock clients speak, we have implemented it, and it
is the piece that survives a node going down.

## 7. Suggested order

1. **RRC-to-LXMF bridge.** Highest value for the command use case, no new
   protocol, uses what is already built and accepted.
2. **ESP-NOW peer interface**, strictly one-hop, presented to Reticulum as an
   ordinary interface. Test with two RADs and no router at all.
3. Revisit only if measurements show the peer link is the bottleneck rather than
   the LoRa backhaul.
