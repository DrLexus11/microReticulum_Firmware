# Fallback interfaces -- design (PR E, operator item 5)

**Status:** design only, 2026-09-28. Built in a later PR once PR E's
reliability items are closed. Columba's feature; the boards are infrastructure
and keep every interface on.

## The problem, in the operator's words

> GSM is down for the initial period and we are working with HaLow or LoRa.
> Responders get brief receptions of something better; they should take the
> weight off those radio systems -- airtime, channel utilisation. We need to
> switch interfaces quickly and get back into the mesh with updated routing,
> healing and reception. Main is BLE; if it fails to find peers, it should
> defer to a configurable interface.

Two needs:

1. **Fallback.** When the primary carrier has nobody to talk to, use another
   one -- without the responder noticing or doing anything.
2. **Relief.** When a fatter carrier is healthy, the thin one (LoRa above all)
   should carry as little as it can, because every handset's periodic traffic
   on it is shared airtime.

## What exists, and why it is not enough

- Columba interfaces have an `enabled` flag. Changing it runs
  `InterfaceConfigManager.applyInterfaceChanges()`, which **restarts the whole
  Reticulum stack**. A switch built on that restarts the mesh on every flap:
  links drop, announces repeat, and a flapping link flaps the whole node.
- `NetworkRestriction` already makes an IP interface conditional (Wi-Fi only,
  cellular only), but it is applied at that restart, not while running.
- Reticulum routes by hop count and recency, not by cost. **Interface gravity**
  (RNS 1.4) only moves a path when the *same* announce reappears with equal or
  fewer hops on a higher-gravity interface; it cannot make a node prefer a
  three-hop BLE path over a one-hop LoRa path. Read in full and set aside
  (`TAKDeliveryPlan.md`, operator item 5).
- Pieces built in PR E that this reuses: the BLE interface's 30 s peer
  heartbeat (who is online), announce-on-meeting (a fresh announce to a new
  peer on its link alone), and held announces.

## The model

Each interface gets a **role**:

| Role | Meaning |
| --- | --- |
| **Always** | Today's behaviour. The default for every existing interface. |
| **Primary** | The carrier this handset expects to use. There may be more than one. |
| **Fallback** | Stays up, but **quiet** while any primary is healthy; **active** while none is. |

**Quiet, not off.** A quiet fallback stays attached and keeps *receiving*, so
the mesh can still reach this handset through it, and nothing restarts. What it
stops is this handset's **broadcast load**: its own periodic announces and
path-request floods are not sent on it. Directed traffic whose path goes
through the fallback -- a reply, a link proof, a message to someone only
reachable there -- still goes. Announces are the steady cost on a shared LoRa
channel; directed traffic is what the responder actually needs.

Implemented where the held-announce filter already sits: the interface's
outgoing path, filtering announces by the same header read
`announce_destination()` uses, switched at runtime. No restart.

**Health is measured, never declared.** A primary is healthy when it has had a
live peer within a window:

| Carrier | Healthy when |
| --- | --- |
| BLE | at least one peer interface online (the 30 s heartbeat) |
| TCP / UDP / Auto (Wi-Fi, HaLow) | connected, and a packet heard within the window |
| RNode (LoRa) | radio online and a packet heard within the window |

Nothing keys on an interface's declared bitrate (`CLAUDE.md`, interface
completeness).

**Hysteresis.** A primary must be unhealthy for `T_down` (default 30 s) before
the fallback goes active, and healthy again for `T_up` (default 5 min) before
it goes quiet. A link that flaps for a minute must not flap the radio. Both are
configurable per handset.

**Re-joining at once.** When a fallback goes active it announces this handset's
destinations on it immediately -- fresh, as announce-on-meeting does -- so the
mesh learns the new route now, not at the next scheduled announce. When it goes
quiet nothing is sent: the routes through the primary are already fresher.

## The interfaces screen

- Each interface card gains a role selector: *Always* / *Primary* /
  *Fallback*. Existing configurations migrate as *Always*, so nothing changes
  for anyone until they choose.
- A fallback's card shows its state in one line: `QUIET - BLE healthy` or
  `ACTIVE - no primary peer 2 min`.
- The notification line follows the tactical rule: state first, one line.

## Acceptance, when it is built

1. BLE primary, LoRa (RNode) fallback: with a BLE peer present, the handset's
   own announces do not appear on LoRa (measured at a board's LoRa interface),
   and a message to it over LoRa still arrives.
2. Take the BLE peer away: within `T_down` + 10 s the handset is reachable over
   LoRa, with no manual announce and no Reticulum restart.
3. Bring the peer back: after `T_up` the fallback is quiet again; no restart.
4. Flap the BLE peer every 20 s for five minutes: the fallback does not change
   state more than once.
5. The same on HaLow/Wi-Fi primary with LoRa fallback -- the field case in the
   operator's words.

## Open questions

- **More than one fallback** -- ordered, or all at once? Start with all
  fallbacks switching together; order only if a real configuration needs it.
- **Propagation-node sync** over a quiet fallback: allowed, since it is
  directed, but it is a bulk transfer. Probably deferred to active only.
- **Boards** keep every interface on. Relief on the boards' LoRa is a firmware
  question -- announce rate caps -- not this feature.
