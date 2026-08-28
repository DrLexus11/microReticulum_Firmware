# Stitching bridged rooms in a client

How a client presents one continuous room conversation when the messages arrive
by two different routes: live over an RRC Link, and after the fact over LXMF
from the bridge.

§3 is **implemented and emitted**. §2 describes the human-readable body, which
is unchanged and is what stock clients still see; §3 is the structured
metadata a merging client should read instead. §5 is a product decision that
remains open.

## 1. Why there are two routes at all

RRC is ephemeral by design and the hub holds no history. Store-and-forward is
LXMF's job, and the bridge exists so a member who was out of range receives what
was said while they were gone. See [`RRCRequirements.md`](RRCRequirements.md)
§12c.

The consequence for a client is that the same logical room is fed by two
transports with different delivery semantics:

| | Live (RRC) | Bridged (LXMF) |
| --- | --- | --- |
| Arrives | while joined | on propagation sync |
| Attribution | Link identity, proven | asserted by the hub |
| Ordering | as received | must be reconstructed |
| Completeness | everything while joined | bounded by the hub's ring |

A client that treats these as two separate inboxes gives the user two partial
histories. A client that merges them naively gives duplicates. Doing it properly
needs identifiers, and that is what §3 adds.

## 2. What the bridge emits today

One LXMF message per absent member, plus one catch-up message when a member
joins a bridged room for the first time.

- **source** -- the node's `lxmf`/`delivery` address, announced so recipients
  can recall the identity and validate the signature. One address serves every
  bridged room on that node.
- **title** -- the room name.
- **content** -- `<nick / room> text`, or `<room> text` when no nickname is
  known. A catch-up message carries several such lines separated by newlines.
- **fields** -- empty.

This is deliberately readable in stock NomadNet, Sideband and Columba, and it is
enough for a human. It is **not** enough for a client that wants one merged
conversation, for four reasons:

1. **The sender is a display string.** RRC's own definition of done requires
   that Link identity, not a nickname, controls attribution
   ([`RRCRequirements.md`](RRCRequirements.md) §14) -- and this discards exactly
   that. Two members may choose one nickname. A client cannot correlate a
   bridged line with the same person's live messages, cannot apply a block, and
   cannot show a verified identity.
2. **Nothing marks it as room traffic.** Detection means matching a pattern in
   the body, which any sender can reproduce in an ordinary direct message.
3. **There is no message id**, so a bridged copy cannot be recognised as one the
   client already received live on another device.
4. **One timestamp covers a whole catch-up.** The digest collapses many messages
   under the moment it was composed, so time-ordered merging is impossible.

## 3. The encoding: `rrc.bridge/1`

LXMF reserves fields for exactly this and stock clients ignore what they do not
recognise, so adding these changes nothing for existing software.

```
fields = {
  0xFB FIELD_CUSTOM_TYPE: "rrc.bridge/1",

  0xFC FIELD_CUSTOM_DATA: {
    0: room name,
    1: hub RRC destination hash (16 bytes),
    2: [                            # one entry per message
         [ message_id,              # the RRC envelope's K_ID
           sender_identity_hash,    # 16 bytes, the Link identity
           nickname,                # display only, never for identity
           timestamp_ms,            # when it was said, not when composed
           text ],
         ...
       ]
  },

  0xFD FIELD_CUSTOM_META: {
    0: ring depth configured on the hub,
    1: timestamp of the oldest line the hub still holds
  }
}
```

A relayed single message is one entry; a catch-up is several. The client
handles one shape.

The human-readable body stays exactly as it is. A client that understands the
fields should render from them and ignore the body.

## 4. How a client stitches

1. **Key the conversation on (hub destination hash, room)** -- never on the LXMF
   source. One node bridges every room behind a single delivery address, so
   keying on the source merges unrelated rooms.
2. **Deduplicate on `message_id`.** This is what makes the experience seamless
   rather than merely functional: without it, anyone connected on two devices
   sees every message twice, once live and once bridged.
3. **Order by `timestamp_ms`**, not by arrival and not by the LXMF timestamp.
4. **Detect gaps and say so.** The hub's ring is bounded, so a catch-up can be
   incomplete. If its oldest entry is newer than the client's last-seen
   timestamp for that room, messages were lost to overflow; `CUSTOM_META` gives
   the client what it needs to mark the discontinuity rather than present a
   false-continuous log. For a command record, silently omitting the gap is
   worse than showing less.

## 5. Trust: this is hub-attested, not end-to-end

The hub signs the LXMF message, so a recipient learns *node X asserts that this
identity said this*. It is not a signature from the original sender.

That is inherent to bridging rather than a shortcoming of the implementation:
RRC v1 carries no per-sender message signatures, so there is nothing for the
bridge to forward even if it wanted to. A client must not present a bridged
message as carrying the same assurance as a live one it received over a Link
whose identity Reticulum proved.

Whether that is acceptable for a command backbone is a product decision, and it
is much cheaper to answer before a client ships assuming otherwise. If it is
not, the fix belongs in RRC -- per-sender signatures in the envelope -- not in
the bridge.

## 6. On unifying the clients

Merging Eridanus's RRC support into Columba, so one application covers direct
messages, propagation and rooms, is the natural home for this. It is also the
only place the stitching can live: it needs the live RRC path and the LXMF path
in one process, sharing one identity and one message store.

Two things to settle before that work starts, because both are cheaper to
decide than to retrofit:

- **§3 is done**, so a client can be built against the fields from the start
  rather than growing body-parsing that has to be torn out.
- **Decide §5.** Whether rooms need end-to-end sender attribution determines
  whether RRC v1 is the final protocol for the backbone or an interim one, and
  that shapes the client's data model rather than just its rendering. The
  identifier is versioned so that adding per-sender signatures later is a
  `rrc.bridge/2`, not a break.
