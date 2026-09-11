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
                    # Abandon the oversized event, not the connection and not
                    # whatever followed it. Clearing the whole buffer threw
                    # away any complete event that arrived in the same read as
                    # a runaway one, so a peer sending a single unterminated
                    # document cost us the good events behind it.
                    #
                    # `continue`, not `break`: resynchronising once per read
                    # leaves the rest of an oversized read sitting in the
                    # buffer, draining a few bytes per call. Looping here
                    # discards until the buffer is either within the bound or
                    # holds no further opening, so `pending` is bounded when
                    # feed() returns. Each pass removes at least one byte, so
                    # it terminates.
                    self._resynchronise()
                    continue
                break
            end += len(EVENT_CLOSE)
            if end > self._max:
                # A *complete* event can be oversized too, and the bound was
                # only ever applied to unterminated ones. There are two ways
                # to get here: a genuinely enormous document, or -- more
                # often -- an unterminated event followed by a real one, where
                # this close tag belongs to the second and the span covers
                # both. Either way the span is not one event, and forwarding
                # it would put a megabyte of whatever it is on the air.
                #
                # Resynchronised from *inside* the span rather than past it:
                # the close tag at the end may well belong to a good event, and
                # dropping the whole span would discard that event too. Moving
                # to the next '<event' drops only the runaway's opening, and
                # the buffer shrinks by at least one byte, so this terminates.
                self._resynchronise()
                continue
            events.append(bytes(self._buffer[:end]))
            del self._buffer[:end]
        return events

    def _resynchronise(self):
        """Drop the current event and pick up at the next one that starts.

        Not a clear(): the bytes after a runaway document are as likely to be
        the start of a good event as anything else, and discarding them means a
        single malformed document costs every event that shared a read with it.
        """
        nxt = self._buffer.find(EVENT_OPEN, 1)
        if nxt < 0:
            self._buffer.clear()
        else:
            del self._buffer[:nxt]

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
    if isinstance(cot_xml, (bytes, bytearray)):
        try:
            cot_xml = bytes(cot_xml).decode("utf-8", errors="strict")
        except UnicodeDecodeError:
            # Not our event if it is not even text. Decoding with replacement
            # here turned malformed bytes into U+FFFD *inside* an otherwise
            # valid event, which then passed every later check and went to the
            # mesh carrying content nobody sent.
            return False
    end = _start_tag_end(cot_xml)
    head = cot_xml if end < 0 else cot_xml[:end]
    span = _attribute_value_span(head, "uid")
    if span is None:
        return False
    return head[span[0]:span[1]] == own_uid


def _start_tag_end(xml):
    """Index of the character closing the root start tag, or -1.

    Quote-aware, because a closing angle bracket is legal unescaped inside an
    XML attribute value. Stopping at the first one would cut the tag short and
    lose the uid attribute -- and losing it in is_self_addressed() means
    forwarding our own event, which is the loop that check exists to prevent.
    """
    quote = ""
    for index, character in enumerate(xml):
        if quote:
            if character == quote:
                quote = ""
        elif character in "\"'":
            quote = character
        elif character == ">":
            return index
    return -1


def _attribute_value_span(start_tag, name):
    """Half-open span of the named attribute's *value* within a start tag.

    The name is matched as a whole token, so a parent_uid attribute is not
    mistaken for uid -- a substring check would read somebody else's event as
    our own and stop us forwarding it.
    """
    index = start_tag.find(name)
    while index >= 0:
        before = " " if index == 0 else start_tag[index - 1]
        after = index + len(name)
        while after < len(start_tag) and start_tag[after].isspace():
            after += 1
        if before.isspace() and after < len(start_tag) and start_tag[after] == "=":
            value = after + 1
            while value < len(start_tag) and start_tag[value].isspace():
                value += 1
            if value < len(start_tag) and start_tag[value] in "\"'":
                close = start_tag.find(start_tag[value], value + 1)
                if close > 0:
                    return value + 1, close
        index = start_tag.find(name, index + 1)
    return None


def _parse(cot_xml):
    """Parse a CoT event, refusing the XML features CoT never needs.

    This input arrives from a socket and from the mesh. Declarations and
    entities are what turn an XML parser into a denial of service, and no
    legitimate CoT event contains either.

    Every failure leaves as a ValueError. ElementTree's ParseError descends
    from SyntaxError, not from ValueError, so a caller guarding with the
    obvious `except ValueError` would let malformed XML through untouched --
    and malformed XML is exactly what a socket delivers first.
    """
    import xml.etree.ElementTree as ET
    if isinstance(cot_xml, (bytes, bytearray)):
        cot_xml = bytes(cot_xml).decode("utf-8", errors="strict")
    if not isinstance(cot_xml, str):
        raise ValueError("CoT must be text or bytes")
    if "<!" in cot_xml:
        raise ValueError("XML declarations and entities are not accepted")
    try:
        event = ET.fromstring(cot_xml)
    except ET.ParseError as error:
        raise ValueError("malformed CoT: %s" % error) from error
    if event.tag != "event":
        raise ValueError("not a CoT event")
    return event


def rewrite_self_uid(cot_xml, atak_uid, our_uid):
    """Give our own self-reports a Reticulum-rooted UID before they leave.

    ATAK reports itself as ANDROID-xxxx, which is a device identifier: it
    cannot be verified, cannot be reversed to address the peer, and changes if
    the app's data is cleared. Peers should see the UID derived from this
    node's destination instead, which is the whole point of pivot 1 in
    TAKIntegrationPivots.md -- otherwise the derived identity exists in the
    codebase and never reaches a track anybody looks at.

    Only *self-reports* are rewritten. An object placed on the map -- a marker,
    a drawing -- carries its own UID and is a distinct thing that happens to
    have been created here; rewriting those would collapse every marker this
    node ever dropped into one track. The discriminator is the same one the
    firehose stager uses and that held across all 42 captured events: a
    self-report's event UID is the reporting device's own.
    """
    import xml.etree.ElementTree as ET
    if not atak_uid or not our_uid:
        return cot_xml if isinstance(cot_xml, str) else bytes(cot_xml).decode("utf-8")
    event = _parse(cot_xml)
    if event.get("uid") != atak_uid:
        return cot_xml if isinstance(cot_xml, str) else bytes(cot_xml).decode("utf-8")
    event.set("uid", our_uid)
    return ET.tostring(event, encoding="unicode")


def learn_atak_uid(cot_xml):
    """The UID this ATAK calls itself, learned from a self-report.

    Nothing configures it: ATAK announces its own identifier in every position
    report, and asking an operator to type it would be one more setting that
    can be wrong. Returns None for anything that is not a self-report, which
    includes every marker and every event relayed from a peer.
    """
    try:
        event = _parse(cot_xml)
    except ValueError:
        return None
    uid = event.get("uid") or ""
    # A self-report describes a unit; ATAK's carries <takv>, which identifies
    # the software reporting. A marker never does, which keeps a marker created
    # here from being mistaken for the device that created it.
    if uid and event.find("detail/takv") is not None:
        return uid
    return None


class CotOutbound:
    """Everything that happens to an event between ATAK and the mesh.

    Separated from the socket so the order of the three steps can be tested,
    because the order is the whole of it: refuse anything already ours, learn
    the device's UID, then rewrite our self-reports.

    One per endpoint rather than one per process. The learned UID belongs to
    the run: changing team tears the endpoint down, and a UID carried across
    that would be a claim about an ATAK nobody has heard from since.
    """

    def __init__(self, our_uid):
        self.our_uid = our_uid
        self.atak_uid = None
        self.dropped = 0

    def frame(self, cot_xml, encode):
        """The frame to put on the mesh, or None if this event should not go.

        None is ordinary and covers three cases: our own event coming back,
        something that is not CoT at all, and an event we could not encode.
        """
        if isinstance(cot_xml, (bytes, bytearray)):
            try:
                cot_xml = bytes(cot_xml).decode("utf-8", errors="strict")
            except UnicodeDecodeError:
                # Counted and dropped, never repaired. Replacement decoding
                # silently rewrites the operator's content -- a callsign with
                # one bad byte becomes a callsign with a U+FFFD in it, and
                # that is what the rest of the team sees.
                self.dropped += 1
                return None
        # Validated here rather than relied on downstream. rewrite_self_uid
        # returns early -- without parsing -- until an ATAK UID has been
        # learned, so before the first self-report of a session nothing else in
        # this path would look at the event at all.
        try:
            _parse(cot_xml)
        except ValueError:
            self.dropped += 1
            return None
        # The echo guard comes first, before anything is learned from the
        # event. Our own self-report echoed back carries <takv> and is a
        # perfectly well-formed self-report, so learning from it would set the
        # ATAK UID to our own -- after which no genuine self-report matches it
        # and none is ever rewritten again. The device would report itself as
        # ANDROID-xxxx to the whole team for the rest of the session, which is
        # the one outcome this pipeline exists to prevent.
        if is_self_addressed(cot_xml, self.our_uid):
            return None
        if self.atak_uid is None:
            learned = learn_atak_uid(cot_xml)
            if learned:
                self.atak_uid = learned
        try:
            return encode(rewrite_self_uid(cot_xml, self.atak_uid, self.our_uid))
        except ValueError:
            self.dropped += 1
            return None
