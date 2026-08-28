# Open items from the RRC-to-LXMF bridge branch

Known and deliberately not fixed on this branch. Each is recorded with what is
actually known about it, so the next person does not have to rediscover the
diagnosis before deciding whether to act.

## 1. Leaked LittleFS descriptor on the hashlist store

    E esp_littlefs: Failed to unlink path "./hashlist_store/seg1.dat". Has open FD.

Seen on Rev 1, twice within a 200-second window, and again after a reflash. The
store segment cannot be replaced because a descriptor on it was never closed.

**Why it matters beyond the log line.** Every open descriptor on LittleFS holds
a cache buffer in internal RAM -- the one resource this board actually runs out
of. If the leak is per-rotation rather than one-off, it is a slow drain with
exactly the signature that took Rev 1 off the air.

**Not yet established:** whether the count grows with uptime, and whether the
descriptor belongs to the hashlist rotation or to something reading it. Both are
answerable by counting the message against uptime, which the current
instrumentation already makes easy.

Unrelated to the bridge. It predates this branch.

## 2. Composed messages carry a zero propagation stamp

`lxmf_compose_propagated()` appends 32 zero bytes where a real proof-of-work
stamp would go. This is inert today and deliberately so: the stamp gates ingest
at a propagation node and is stripped again before a message is served
(`LXMRouter.py` does this, and so does `lxmf_message_get_request()` here), so a
message inserted into this node's own store never crosses the gate the stamp
exists for.

**It stops being inert under roadmap item 4a.** A peer validates the stamp
against its own advertised cost before storing, and would reject every bridged
message we offer it. Generate a real stamp before enabling peering, rather than
afterwards while wondering why a peer holds none of our room traffic.

Cost estimate: our advertised cost is 16, so roughly 65k hashes per message.
That is seconds of CPU on this hardware and must not run on a callback path --
the same constraint that already puts composition on the main loop.

## 3. No LXMF-to-RRC direction

A reply sent to the bridge's delivery address does not appear in the room. The
bridge is one-way.

This is the change that makes loop prevention stop being free. Today nothing
injects into RRC and RRC ingress is only ever an envelope arriving on a Link, so
a loop is structurally impossible. Once LXMF can inject, the hub must not
re-broadcast what it originated, and LXMF's transient-id tracking is the
mechanism -- see [`RRCRequirements.md`](RRCRequirements.md) §12c.

## 4. Trust: bridged messages are hub-attested

A recipient learns *this node asserts that this identity said this*, not a
signature from the original sender. RRC v1 carries no per-sender message
signatures, so there is nothing for the bridge to forward even if it wanted to.

This is a product decision rather than a defect, and it is the one open question
that shapes a client's data model rather than its rendering:
[`BridgeClientContract.md`](BridgeClientContract.md) §5. The `rrc.bridge/1`
identifier is versioned so that adding per-sender signatures later is a
`rrc.bridge/2` rather than a break.

Decide it before a unified client ships assuming otherwise.

## 5. Rev 1's UNKNOWN resets are reduced, not explained

Rev 1 restarted repeatedly with `reset reason: UNKNOWN (0)`. Two things are now
known:

- A large share of them were **self-inflicted during diagnosis**. Closing and
  reopening the USB CDC port resets the board, and each console capture was an
  open followed by a close. Uptime read 7 seconds immediately after one
  reattach. Reset counts gathered that way are not evidence.
- The reconnect storm was real and is fixed, and with it the memory slide: 90
  minutes of continuous uptime with internal free flat at ~57.5 KB, against a
  previous rate of roughly 15 KB/hour ending in a self-restart.

**Not established:** whether any UNKNOWN resets remain once the observer is
removed. The way to answer it is the metrics poller -- one long-lived port
connection, uptime as ground truth -- rather than repeated console attachments.

## 6. Roster and history bounds are untested at their limits

`RRC_BRIDGE_MAX_MEMBERS` is 16 per room and a full roster drops the newcomer
rather than evicting an established member. `RRC_BRIDGE_QUEUE_DEPTH` is 8
messages and a full queue drops the newest. Neither has been exercised at its
bound on hardware; both are counted in the dropped metric, so the failure is at
least visible rather than silent.

The digest budget is exercised only up to the history actually accumulated in
testing, which is far short of the 20-line default.
