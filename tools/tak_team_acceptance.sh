#!/usr/bin/env bash
# Drive PR B's whole path between two bridges: membership, then every codec.
#
#   tools/tak_team_acceptance.sh
#   KEEP=1 tools/tak_team_acceptance.sh     # leave the bridges running
#
# Two bridges on separate Reticulum instances, one hop apart, because one hop
# is the only shape group traffic ever survived and the only shape this has
# been proven in. Each stands in for a node with ATAK attached: the harness
# connects to the local CoT endpoint each one serves, which is the same socket
# ATAK uses.
#
# What it asserts, in order, because each depends on the last:
#
#   1. the two discover each other from announces alone
#   2. a position crosses, typed, and lands under the sender's whole UID
#   3. a marker crosses and keeps its own uid
#   4. an SPI crosses without carrying the sender's device identifier
#   5. a chat line crosses and arrives with its text
#
# Run against a tree with no hardware attached: both ends are on loopback TCP,
# so this measures correctness and says nothing at all about airtime.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="${PY:-$HOME/.local/share/rnode-rns-venv/bin/python}"
WORK="${WORK:-$(mktemp -d -t tak-acceptance-XXXXXX)}"
ALPHA_PORT="${ALPHA_PORT:-8187}"
BRAVO_PORT="${BRAVO_PORT:-8188}"
LINK_PORT="${LINK_PORT:-4301}"
TEAM="${TEAM:-Cyan}"

step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
pass() { printf '\033[32m  PASS: %s\033[0m\n' "$*"; }
fail() { printf '\033[31m  FAIL: %s\033[0m\n' "$*"; FAILED=1; }
FAILED=0

cleanup() {
    if [ "${KEEP:-0}" = "1" ]; then
        echo "bridges left running; logs in $WORK"
        return
    fi
    for port in "$ALPHA_PORT" "$BRAVO_PORT"; do
        # By port, not by pattern. A pkill for the script name matches this
        # script too on this host, which has killed the shell running it.
        pid=$(ss -ltnp 2>/dev/null | grep ":$port " | sed 's/.*pid=\([0-9]*\).*/\1/' | head -1)
        [ -n "$pid" ] && kill "$pid" 2>/dev/null
    done
    sleep 1
}
trap cleanup EXIT

[ -x "$PY" ] || { echo "no RNS python at $PY; set PY"; exit 1; }

step "two Reticulum instances, one hop apart"
mkdir -p "$WORK/alpha" "$WORK/bravo"
cat > "$WORK/alpha/config" <<EOF
[reticulum]
  enable_transport = No
  share_instance = No
  panic_on_interface_error = No
[logging]
  loglevel = 4
[interfaces]
  [[Alpha listen]]
    type = TCPServerInterface
    enabled = Yes
    listen_ip = 127.0.0.1
    listen_port = $LINK_PORT
EOF
cat > "$WORK/bravo/config" <<EOF
[reticulum]
  enable_transport = No
  share_instance = No
  panic_on_interface_error = No
[logging]
  loglevel = 4
[interfaces]
  [[Bravo to alpha]]
    type = TCPClientInterface
    enabled = Yes
    target_host = 127.0.0.1
    target_port = $LINK_PORT
EOF

nohup "$PY" "$HERE/tools/cot_bridge.py" --team "$TEAM" --config "$WORK/alpha" \
    --identity "$WORK/alpha/node-identity" --callsign ALPHA --port "$ALPHA_PORT" \
    > "$WORK/alpha.log" 2>&1 &
sleep 12
nohup "$PY" "$HERE/tools/cot_bridge.py" --team "$TEAM" --config "$WORK/bravo" \
    --identity "$WORK/bravo/node-identity" --callsign BRAVO --port "$BRAVO_PORT" \
    > "$WORK/bravo.log" 2>&1 &
sleep 30

step "they find each other from announces alone"
if grep -q "team member BRAVO" "$WORK/alpha.log" && grep -q "team member ALPHA" "$WORK/bravo.log"; then
    pass "mutual discovery"
else
    fail "one or both did not learn the other"
    tail -5 "$WORK/alpha.log" "$WORK/bravo.log"
    exit 1
fi

step "every codec, across the mesh"
ALPHA_UID=$(sed -n 's/.*this node is \(urtn-[0-9a-f]*\).*/\1/p' "$WORK/alpha.log" | head -1)
BRAVO_UID=$(sed -n 's/.*this node is \(urtn-[0-9a-f]*\).*/\1/p' "$WORK/bravo.log" | head -1)
ALPHA_PORT="$ALPHA_PORT" BRAVO_PORT="$BRAVO_PORT" ALPHA_UID="$ALPHA_UID" \
BRAVO_UID="$BRAVO_UID" \
python3 - <<'PYEOF'
import os, re, socket, sys, time

alpha_port = int(os.environ["ALPHA_PORT"])
bravo_port = int(os.environ["BRAVO_PORT"])
alpha_uid = os.environ["ALPHA_UID"]
bravo_uid = os.environ["BRAVO_UID"]
failures = []


def drain(sock, seconds):
    sock.settimeout(0.4)
    buffer, events, deadline = b"", [], time.time() + seconds
    while time.time() < deadline:
        try:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buffer += chunk
        except socket.timeout:
            continue
        while b"</event>" in buffer:
            end = buffer.index(b"</event>") + 8
            events.append(buffer[:end].decode("utf-8", "replace"))
            buffer = buffer[end:]
    return events


def check(name, events, predicate, detail=""):
    if any(predicate(e) for e in events):
        print("\033[32m  PASS: %s\033[0m" % name)
    else:
        print("\033[31m  FAIL: %s %s\033[0m" % (name, detail))
        for event in events[:3]:
            print("        saw %s" % event[:100])
        failures.append(name)


alpha = socket.create_connection(("127.0.0.1", alpha_port), timeout=5)
bravo = socket.create_connection(("127.0.0.1", bravo_port), timeout=5)
drain(alpha, 2), drain(bravo, 2)

pli = ('<event uid="ANDROID-ALPHATEST" type="a-f-G-U-C" how="m-g" version="2.0">'
       '<point lat="%s" lon="%s" hae="33.5" ce="44.0" le="9999999.0"/>'
       '<detail><takv device="T" os="36" platform="ATAK-CIV" version="5.6"/>'
       '<contact callsign="ALPHA-EUD"/></detail></event>')

# Before anything else, and deliberately: an SPI as the very first event of
# the session, when nothing has been learned yet. The typed paths used to wait
# on a learned ATAK uid, which is only set by a self-report -- so a session
# that opened with a pointer sent it as tier 2 with ANDROID-<device>.SPI1
# intact, putting on the air the identifier pivot 1 removed from everything
# else. This check has to run first or it proves nothing.
first_spi = ('<event uid="ANDROID-ALPHATEST.SPI1" type="b-m-p-s-p-i" how="h-e" '
             'version="2.0" start="2026-09-11T20:00:00.000Z" '
             'stale="2026-09-11T20:00:20.000Z">'
             '<point lat="41.0100" lon="28.9000" hae="108.0" ce="9999999.0" '
             'le="9999999.0"/>'
             '<detail><contact callsign="ALPHA.DP0"/></detail></event>')
alpha.sendall(first_spi.encode())
seen = drain(bravo, 12)
check("a pointer sent before any position leaks no device id", seen or [""],
      lambda _: not any("ANDROID-ALPHATEST" in e for e in seen),
      "bravo saw the sender's device identifier")

# Now the self-report, which is what teaches the endpoint its ATAK uid. The
# typed paths no longer wait on it -- only the tier-2 self-report rewrite does.
alpha.sendall((pli % ("40.9549", "29.0934")).encode())
drain(bravo, 8)

alpha.sendall((pli % ("40.9750", "29.1150")).encode())
seen = drain(bravo, 15)
check("a position arrives under the sender's whole UID", seen,
      lambda e: alpha_uid in e and "40.975" in e,
      "expected %s" % alpha_uid)

marker = ('<event uid="7c9e6679-7425-40de-944b-e07fc1f90ae7" type="a-h-G" '
          'how="h-g-i-g-o" version="2.0" start="2026-09-11T20:00:00.000Z" '
          'stale="2026-09-11T20:05:00.000Z">'
          '<point lat="40.9601" lon="29.1002" hae="48.0" ce="9.0" le="9.0"/>'
          '<detail><contact callsign="HOSTILE.7"/><color argb="-1"/></detail></event>')
alpha.sendall(marker.encode())
seen = drain(bravo, 15)
check("a marker arrives and keeps its own uid", seen,
      lambda e: "7c9e6679-7425-40de-944b-e07fc1f90ae7" in e and "a-h-G" in e)

spi = ('<event uid="ANDROID-ALPHATEST.SPI1" type="b-m-p-s-p-i" how="h-e" '
       'version="2.0" start="2026-09-11T20:00:00.000Z" '
       'stale="2026-09-11T20:00:20.000Z">'
       '<point lat="41.0238" lon="28.9163" hae="108.0" ce="9999999.0" le="9999999.0"/>'
       '<detail><contact callsign="ALPHA.DP1"/></detail></event>')
alpha.sendall(spi.encode())
seen = drain(bravo, 15)
check("an SPI arrives rebuilt against the Reticulum identity", seen,
      lambda e: ".SPI1" in e and alpha_uid in e)
check("and carries no device identifier", seen or [""],
      lambda e: "ANDROID-ALPHATEST" not in e)

chat = ('<event uid="GeoChat.ANDROID-ALPHATEST.Cyan.9c1f2ab4-77de-4c11-b0a3-55e4d1c9f0aa" '
        'type="b-t-f" how="h-g-i-g-o" version="2.0">'
        '<point lat="40.95" lon="29.09" hae="1" ce="1" le="1"/>'
        '<detail><__chat chatroom="Cyan" groupOwner="false" id="Cyan" '
        'messageId="9c1f2ab4-77de-4c11-b0a3-55e4d1c9f0aa" parent="RootContactGroup" '
        'senderCallsign="ALPHA"><chatgrp id="Cyan" uid0="a" uid1="b"/></__chat>'
        '<remarks source="BAO.F.ATAK.x" to="Cyan">moving now</remarks></detail></event>')
alpha.sendall(chat.encode())
seen = drain(bravo, 15)
check("a chat line arrives with its text", seen,
      lambda e: "moving now" in e and "b-t-f" in e)


def direct(target, message_id, text):
    """A chat line addressed to one uid, the way ATAK writes a DM: the
    recipient's uid in the id field, their callsign as the chatroom."""
    return ('<event uid="GeoChat.ANDROID-ALPHATEST.%s.%s" type="b-t-f" '
            'how="h-g-i-g-o" version="2.0" time="2026-09-12T09:00:00.000Z">'
            '<point lat="40.95" lon="29.09" hae="1" ce="1" le="1"/>'
            '<detail><__chat chatroom="BRAVO" groupOwner="false" id="%s" '
            'messageId="%s" parent="RootContactGroup" senderCallsign="ALPHA">'
            '<chatgrp id="%s" uid0="%s" uid1="%s"/></__chat>'
            '<remarks source="BAO.F.ATAK.x" to="BRAVO">%s</remarks>'
            '</detail></event>'
            % (target, message_id, target, message_id, target, alpha_uid,
               target, text))


alpha.sendall(direct(bravo_uid, "1b0c3d4e-5f60-4a71-8b92-c3d4e5f60718",
                     "meet me at the north gate").encode())
seen = drain(bravo, 15)
check("a direct message reaches the member it names", seen,
      lambda e: "north gate" in e)
check("and threads on the uid rather than the callsign", seen or [""],
      lambda e: 'uid1="%s"' % bravo_uid in e)

# The recipient is real to ATAK and is not a member of this team: a server
# contact, or somebody who has never announced here. Broadcasting it would
# both leak a private line and fail to deliver it.
alpha.sendall(direct("S-1-5-21-999999999-888888888-777777777-1001",
                     "2c1d4e5f-6071-4b82-9ca3-d4e5f6071829",
                     "this must not be broadcast").encode())
seen = drain(bravo, 8)
check("a line for a stranger is not broadcast to the team", [""],
      lambda _: not any("must not be broadcast" in e for e in seen),
      "bravo saw a message addressed to somebody else")

# A receipt for a room line. Every member answering a broadcast with a delivery
# and a read receipt is two thirds of what group chat costs on the air -- 7.2 s
# of the 11.1 s a ten-person room spends -- for one bit of meaning each. These
# stop at the endpoint that produced them.
room_receipt = ('<event uid="3f8e1c2d-4a5b-4c6d-8e9f-0a1b2c3d4e5f" type="b-t-f-d" '
                'how="h-g-i-g-o" version="2.0" time="2026-09-12T09:10:00.000Z">'
                '<point lat="40.95" lon="29.09" hae="1" ce="1" le="1"/>'
                '<detail><__chatreceipt chatroom="Cyan" groupOwner="false" id="Cyan" '
                'messageId="3f8e1c2d-4a5b-4c6d-8e9f-0a1b2c3d4e5f" '
                'parent="RootContactGroup" senderCallsign="ALPHA">'
                '<chatgrp id="Cyan" uid0="a" uid1="Cyan"/></__chatreceipt>'
                '</detail></event>')
alpha.sendall(room_receipt.encode())
seen = drain(bravo, 8)
check("a receipt for a room line never reaches the team", [""],
      lambda _: not any("3f8e1c2d-4a5b-4c6d-8e9f-0a1b2c3d4e5f" in e for e in seen),
      "bravo received a room receipt")

# The direct receipt still goes, because there it is one peer and the operator
# is waiting on exactly that answer.
direct_receipt = ('<event uid="4a9f2d3e-5b6c-4d7e-9f0a-1b2c3d4e5f60" type="b-t-f-d" '
                  'how="h-g-i-g-o" version="2.0" time="2026-09-12T09:11:00.000Z">'
                  '<point lat="40.95" lon="29.09" hae="1" ce="1" le="1"/>'
                  '<detail><__chatreceipt chatroom="BRAVO" groupOwner="false" '
                  'id="%s" messageId="4a9f2d3e-5b6c-4d7e-9f0a-1b2c3d4e5f60" '
                  'parent="RootContactGroup" senderCallsign="ALPHA">'
                  '<chatgrp id="%s" uid0="%s" uid1="%s"/></__chatreceipt>'
                  '</detail></event>' % (bravo_uid, bravo_uid, alpha_uid, bravo_uid))
alpha.sendall(direct_receipt.encode())
seen = drain(bravo, 15)
check("a receipt for a direct message still arrives", seen,
      lambda e: "4a9f2d3e-5b6c-4d7e-9f0a-1b2c3d4e5f60" in e)

alpha.close()
bravo.close()
sys.exit(1 if failures else 0)
PYEOF
[ $? -eq 0 ] || FAILED=1

step "the reliable path was the one taken"
# A direct message crossing proves nothing on its own: the bare-packet
# fallback delivers it too, and the round trip looks identical. This is what
# separates "it arrived" from "it arrived by the path that can promise to".
if grep -q "direct message sent over LXMF" "$WORK/alpha.log"; then
    pass "a direct message went by LXMF, not as a bare packet"
else
    fail "the direct message fell back to a bare packet"
    grep -i "lxmf" "$WORK/alpha.log" | tail -5
fi

step "result"
if [ "$FAILED" = "0" ]; then
    printf '\033[32m  every codec crossed the mesh\033[0m\n'
else
    printf '\033[31m  something did not cross -- logs in %s\033[0m\n' "$WORK"
    exit 1
fi
