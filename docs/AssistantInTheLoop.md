# An assistant in the loop — direction, not yet a plan

Raised 2026-09-12, from the field side rather than the software side. **Notes
only.** Nothing here is scheduled; it is written down so the shape is not
re-derived later, and so decisions taken before it (the query API, the chat
path, the plugin's scope) do not quietly close doors it needs.

## What was asked for

From inside chat, an operator should be able to ask and be answered:

- *What is around me?*
- *Tell me about the markers near here.*
- *I am looking at bearing 120 — what am I looking at?*
- *What can you tell me about my surroundings?*

And it should be able to **act**, not only describe: drop markers, describe a
mission, create a mission — "all a command personnel can do".

**Model choice must be configurable.** A frontier model where the question
deserves one; a smaller, cheaper model where the volume does. That is a
requirement, not a deployment detail, and it shapes where the boundary sits.

## Why this is written down now rather than later

Three decisions already taken would be expensive to revisit if this arrives as
a surprise.

**Chat is the interface.** The assistant is reached "inside from chat", which
means it is a *participant* in the chat path PR C just built, not a separate
screen. A participant needs an identity, an address, and delivery semantics —
all of which it gets for free by being an LXMF destination like any other
member. That is a good fit and worth not breaking.

**The query API's shape.** `TAKDeliveryPlan.md` settled pull-for-one-building,
push-for-the-neighbourhood. An assistant answering "what is around me" is the
heaviest possible consumer of exactly that API, and it asks for *areas* rather
than single items. The push half matters more than it looked.

**Where the model runs.** Not decided, and it is the first real question:

| | Where | Cost | Works when |
| --- | --- | --- | --- |
| At the command post | deck, with internet or a local model | one link to the internet | the mesh reaches the CP |
| On the responder's phone | small local model | none per query | always, including fully partitioned |
| Split | small model local, frontier via the CP | mixed | degrades rather than fails |

Disaster-first says the split: a responder cut off from the CP must still get
*an* answer about what is around them, because that is precisely when they are
cut off and need it. That argues for the on-device half being the one that can
answer from local map and marker data alone, with the frontier model reached
through the mesh when it is reachable.

## The part that is not a model problem

"What is around me" is answerable only if the node *knows* what is around it.
That is the `urban-tak` data — roads, buildings, addresses — plus the live
marker and position picture. **The assistant is an interface onto that data, and
it cannot be better than the data underneath.** Building it before the map data
exists would produce something that sounds confident and knows nothing, which
in a rescue is worse than no answer.

So the honest ordering is: map and building data first (`urban-tak` phases 0–4),
the query API second, the assistant third. It is a payoff, not a foundation.

## The part that needs care

An assistant with **command capabilities** — creating markers and missions —
is acting on a shared operational picture that other people are navigating by.
Two things follow, and neither is a model choice:

- **Attribution.** A marker it creates must be attributable to the operator who
  asked for it, not to an anonymous agent. Peers are already announced under a
  Reticulum-rooted UID, so the mechanism exists.
- **Confirmation before it acts.** Describing is free; creating a mission is
  not. Where the line sits is an operational decision for the people running
  the callout, not a default to be picked here.

## Open questions

1. Which model, where, and what is the fallback when the frontier one is
   unreachable — silence, or the small one?
2. What can it do without confirmation, and what always needs a human?
3. How does an answer reach an operator who asked while partitioned and has
   since moved?
4. Does the assistant read the map data directly, or only through the same
   query API a human client uses? The second is slower and much easier to
   reason about.
