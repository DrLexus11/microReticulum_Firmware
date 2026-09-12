# The mesh and the server — one picture, two networks

Asked 2026-09-12: can the deck merge meshed and non-meshed ATAK events? Team
members arrive over Tailscale and talk to OpenTAKServer; do they have to join
the mesh first?

**Short answer: no, they must not have to — and today the deck does not merge
anything.**

## What is actually running on the deck

Two separate things that have never been introduced to each other:

| | What it is | Who reaches it |
| --- | --- | --- |
| **OpenTAKServer** | TLS on 8089 and 8443 | iTAK, WinTAK and ATAK over Tailscale |
| **`cot_bridge.py`** | a local CoT endpoint on 127.0.0.1:8087, backed by Reticulum | mesh nodes, as a team member |

Nothing crosses between them. A Tailscale operator's marker reaches everyone on
the server and no one on the mesh; a responder's marker reaches everyone on the
mesh and no one at the command post. Two correct, complete, separate pictures —
which is the failure this document exists to remove.

## Requiring them to join the mesh is the wrong answer

They are on iOS and Windows, arriving over the internet, with no radio. Making
mesh membership a precondition would mean a command post that cannot see the
field unless it buys hardware it does not need — and the command post is
exactly the participant most likely to have a working internet connection and
least likely to be carrying a RAD.

The deck already *is* the meeting point. It has a Reticulum identity, a team
membership and an OTS instance on the same machine.

## The deck is a member of both, and a policy point between them

Not a tunnel. **A gateway between a fat pipe and a thin one is a policy
decision, not a wire**, and the traffic asymmetry is not a detail:

- Mesh → server: send everything. The Tailscale side has bandwidth to spare,
  and a command post wants every report it can get.
- Server → mesh: send almost nothing, deliberately. Position reports from
  server-side clients at ATAK's native cadence would saturate a LoRa channel by
  themselves — ten mesh nodes at one report a minute already spend 32% of it
  with the *typed* codec, and a single Tailscale client reports far faster than
  that.

So the rule is: **the mesh subscribes to what it asks for, rather than
receiving what the server happens to carry.** Markers and directed chat cross
inward because an operator created them deliberately and they are rare.
Positions cross inward only for a named few — the incident commander, say —
and through the same gate everything else goes through. Drawings wait on tier
3 fragmentation, as they do already.

That is a filter with an explicit allow-list, and it belongs in the bridge
where the airtime model already lives.

## Three things that are harder than the plumbing

### Identity: who is a server-side operator, on the air?

Pivot 1 removed device identifiers from everything the mesh carries. A
Tailscale client's events are full of them, so they cannot simply be forwarded.

Nor can they all appear as the deck: that would collapse every command-post
operator into one track, and a responder asking "who told me to go there" would
get "the deck".

The shape that fits what is already built: the deck **vouches** for each
server-side participant, deriving a stable `urtn-` UID for them from its own
identity and their server identity. Mesh nodes then see distinct, addressable
tracks that resolve to real people, with no device identifier on the air, and
the derivation is reproducible so the same operator is the same UID tomorrow.

### Authority: a command from off-mesh is still a command

"Tailscale users issuing commands" is the part that needs care. Creating a
mission or tasking a team is not the same as putting a marker on a map, and
the two networks authenticate completely differently:

| | How a participant is known |
| --- | --- |
| Server side | an OTS client certificate, issued by an enrolment we control |
| Mesh side | a Reticulum identity plus the fleet secret |

**The deck is where those two meet, which makes it a trust boundary.**
Translating one into the other has to be a deliberate, narrow mapping rather
than a side effect of forwarding. A tasking that arrives over Tailscale should
carry, on the mesh, an attribution a responder can act on and a signature the
mesh can check — which is what `tak_tasking.py` already does for mesh-side
tasking, and is the natural place to extend rather than a second scheme.

Until that is built, the honest position is: **markers and reports cross,
commands do not.** A command post that can place a marker and send a direct
message can run an exercise; one that can silently task a team across a trust
boundary nobody has designed is a different proposition.

### Echo: the same event arriving twice

A responder's marker goes mesh → deck → server. Every Tailscale client sees it.
If the gateway is naive, the server's copy comes back deck → mesh and the
marker exists twice with two UIDs, or loops.

The endpoint already solves exactly this for the local ATAK — `CotOutbound`
refuses our own events before they can teach the pipeline anything — and the
same discipline applies here, keyed on the vouched UID rather than the local
one.

## Where this goes

Not in PR C. It is its own piece of work, and it depends on two things already
scheduled: tier 3 fragmentation (PR D) before drawings can cross inward at all,
and the relaying/boundary work (PR E) before the deck's role as a transit point
is settled.

It should be written down as its own PR before Outdoor Test 1 if command-post
participation is wanted during that test — because the answer to "can the CP
see the field" is currently *no*, and that is worth knowing in advance rather
than discovering on the day.
