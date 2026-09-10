"""The local CoT endpoint: ATAK connects to 127.0.0.1, never to anyone's IP.

This is decision 1 of TAKNative.md. Each participant runs one of these, so a
partition never looks to ATAK like a disconnected server -- the app keeps
running and the data thins out. Pointing ATAK at the deck instead would end an
exercise for everyone the moment the deck went away, including two people
standing next to each other.

Only the stream handling lives here, deliberately. Framing CoT out of a TCP
byte stream is the fiddly part and it is worth testing without a radio, a
socket or a Reticulum instance in the way.
"""

# ATAK opens a TCP connection and writes CoT documents back to back with no
# enclosing root element and no length prefix, so the reader has to find the
# boundaries itself. There is no framing to rely on other than the closing tag.
EVENT_OPEN = b"<event"
EVENT_CLOSE = b"</event>"
SELF_CLOSE = b"/>"

# An unterminated event must not grow without bound. A peer that opens "<event"
# and never closes it would otherwise consume memory until the process dies,
# and the largest legitimate CoT seen from ATAK is a 6 KB drawing.
MAX_EVENT_BYTES = 256 * 1024


class CotStream:
    """Reassembles whole CoT events from arbitrary TCP fragments.

    A segment can carry half an event, several events, or an event split
    mid-attribute; all three happen in practice, and treating a read() as a
    message is the classic way to lose CoT under load.
    """

    def __init__(self, max_event_bytes=MAX_EVENT_BYTES):
        self._buffer = bytearray()
        self._max = max_event_bytes

    def feed(self, chunk):
        """Add received bytes; return the complete events they finished."""
        if not chunk:
            return []
        self._buffer.extend(chunk)
        events = []
        while True:
            start = self._buffer.find(EVENT_OPEN)
            if start < 0:
                # Nothing resembling an event yet. Keep only a fragment that
                # could still become one, so noise cannot accumulate.
                if len(self._buffer) > len(EVENT_OPEN):
                    del self._buffer[:-len(EVENT_OPEN)]
                break
            if start:
                # Discard whatever preceded the event: XML declarations,
                # whitespace between documents, or a partial event we already
                # gave up on.
                del self._buffer[:start]
            end = self._buffer.find(EVENT_CLOSE)
            if end < 0:
                if len(self._buffer) > self._max:
                    # Abandon the oversized event rather than the connection:
                    # a peer sending one runaway document should not cost us
                    # the ones that follow it.
                    self._buffer.clear()
                break
            end += len(EVENT_CLOSE)
            events.append(bytes(self._buffer[:end]))
            del self._buffer[:end]
        return events

    @property
    def pending(self):
        """Bytes held awaiting completion. Exposed for tests and diagnostics."""
        return len(self._buffer)


def is_self_addressed(cot_xml, own_uid):
    """True when this event is the endpoint's own, echoed back.

    ATAK and the mesh both rebroadcast, so an endpoint that forwarded
    everything it received would feed its own position back into the mesh and
    grow a loop that looks exactly like a busy network.
    """
    if not own_uid:
        return False
    needle = ('uid="%s"' % own_uid).encode("utf-8")
    if isinstance(cot_xml, str):
        cot_xml = cot_xml.encode("utf-8")
    head = cot_xml[:cot_xml.find(b">") + 1] if b">" in cot_xml else cot_xml
    return needle in head
