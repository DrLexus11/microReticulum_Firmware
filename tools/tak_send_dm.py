#!/usr/bin/env python3
"""Send one direct chat message into a running bridge, as ATAK would.

For the hardware check that PR C's whole point rests on: a line typed on one
node reaching the *person* on another, over LXMF, and showing up in both their
ATAK and their Columba conversation.

It writes a GeoChat event to the bridge's local CoT port exactly as ATAK does,
so nothing about the path under test is simulated -- the bridge cannot tell
this from the real client, and everything after the socket is production code.

    tools/tak_send_dm.py --to urtn-<peer uid> --text "meet me at the north gate"

The peer's UID is what the bridge prints for itself at startup ("this node is
urtn-..."), and what ATAK shows for that peer in its contact list. Addressing
by UID rather than callsign is the point: it is what lets the far end resolve
one member instead of fanning the line out to the team.
"""

import argparse
import socket
import sys
import time
import uuid
from datetime import datetime, timezone


def iso(when):
    return when.strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def build(sender_uid, recipient_uid, callsign, room, text, message_id, now):
    """The GeoChat event ATAK writes for a direct message.

    The recipient's uid goes in the `id` attribute and in chatgrp's uid1, while
    `chatroom` carries their *callsign* -- which is exactly why the room alone
    cannot tell "everyone" from "one person", and why the codec carries the
    recipient separately.
    """
    stamp = iso(now)
    return (
        '<event version="2.0" uid="GeoChat.%s.%s.%s" type="b-t-f" how="h-g-i-g-o" '
        'time="%s" start="%s" stale="%s">'
        '<point lat="0.0" lon="0.0" hae="9999999.0" ce="9999999.0" le="9999999.0"/>'
        '<detail>'
        '<__chat chatroom="%s" groupOwner="false" id="%s" messageId="%s" '
        'parent="RootContactGroup" senderCallsign="%s">'
        '<chatgrp id="%s" uid0="%s" uid1="%s"/></__chat>'
        '<link relation="p-p" type="a-f-G-U-C" uid="%s"/>'
        '<remarks source="BAO.F.ATAK.%s" time="%s" to="%s">%s</remarks>'
        '<marti><dest callsign="%s"/></marti>'
        '</detail></event>'
        % (sender_uid, recipient_uid, message_id, stamp, stamp, stamp,
           room, recipient_uid, message_id, callsign,
           recipient_uid, sender_uid, recipient_uid,
           sender_uid, sender_uid, stamp, recipient_uid, escape(text), room)
    )


def escape(value):
    return (value.replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--to", required=True,
                        help="the recipient's urtn- UID, as the bridge prints it")
    parser.add_argument("--text", default="radio check over LXMF")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8087,
                        help="the bridge's local CoT port")
    parser.add_argument("--from-uid", default="ANDROID-DMTEST",
                        help="what this pretend ATAK calls itself")
    parser.add_argument("--callsign", default="DECK")
    parser.add_argument("--room", default="PEER",
                        help="the recipient's callsign, which is what ATAK "
                             "puts in the chatroom field for a direct message")
    parser.add_argument("--listen", type=float, default=20.0,
                        help="seconds to read back events the bridge sends us")
    args = parser.parse_args()

    if not args.to.startswith("urtn-"):
        sys.exit("--to should be a urtn- UID; %r will not resolve to a member "
                 "and the line will be dropped rather than broadcast" % args.to)

    message_id = str(uuid.uuid4())
    event = build(args.from_uid, args.to, args.callsign, args.room,
                  args.text, message_id, datetime.now(timezone.utc))

    with socket.create_connection((args.host, args.port), timeout=5) as sock:
        # A self-report first. The bridge learns what this ATAK calls itself
        # from one of these, and without it the chat line is still handled --
        # but the session looks nothing like a real one.
        sock.sendall(
            ('<event version="2.0" uid="%s" type="a-f-G-U-C" how="m-g" '
             'time="%s" start="%s" stale="%s">'
             '<point lat="41.0" lon="29.0" hae="30.0" ce="10.0" le="9999999.0"/>'
             '<detail><takv device="T" os="36" platform="ATAK-CIV" version="5.6"/>'
             '<contact callsign="%s"/></detail></event>'
             % (args.from_uid, iso(datetime.now(timezone.utc)),
                iso(datetime.now(timezone.utc)), iso(datetime.now(timezone.utc)),
                args.callsign)).encode("utf-8"))
        time.sleep(1.0)

        print("sending message %s to %s" % (message_id, args.to), flush=True)
        sock.sendall(event.encode("utf-8"))

        print("watching the bridge for %.0fs; expect the log to say "
              "'direct message sent over LXMF'" % args.listen, flush=True)
        sock.settimeout(0.5)
        deadline = time.time() + args.listen
        while time.time() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            print("  <- %s" % chunk[:200].decode("utf-8", errors="replace"), flush=True)


if __name__ == "__main__":
    main()
