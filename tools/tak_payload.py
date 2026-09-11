"""What the first byte of a mesh payload means.

Three codecs now share one destination: tier 2 CoT, position reports, and chat.
A receiver tells them apart by byte zero, which each codec happens to use as its
own format version -- and that works only because those version numbers differ.

That is a coincidence held in place by nothing, and it is the kind of
coincidence that survives right up until somebody bumps a version. So byte zero
is declared here as a **single namespace** rather than three independent version
counters: a codec's version *is* its kind, and a new version of one codec takes
a new number from this table rather than incrementing its own.

The cost of getting this wrong is not a parse error. A position report read as
tier 2 fails cleanly; a tier 2 frame read as a position decodes into
coordinates, and a marker lands somewhere nobody put it.
"""

COT_TIER2 = 1
POSITION_V2 = 2
CHAT_V1 = 3

# Every kind that may appear as byte zero, and what produced it. A reader that
# does not recognise a kind should drop the frame rather than guess: a peer
# running a newer build is ordinary, and so is a frame that is not ours at all.
KINDS = {
    COT_TIER2: "cot-tier2",
    POSITION_V2: "position-v2",
    CHAT_V1: "chat-v1",
}


def kind_of(frame):
    """The kind byte of a frame, or None if it is too short or unknown."""
    if not isinstance(frame, (bytes, bytearray)) or not frame:
        return None
    kind = frame[0]
    return kind if kind in KINDS else None


def name_of(frame):
    """What produced this frame, for a log line. 'unknown' rather than a guess."""
    kind = kind_of(frame)
    return KINDS.get(kind, "unknown")
