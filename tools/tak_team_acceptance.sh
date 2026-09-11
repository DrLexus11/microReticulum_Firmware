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
ALPHA_PORT="$ALPHA_PORT" BRAVO_PORT="$BRAVO_PORT" ALPHA_UID="$ALPHA_UID" \
python3 - <<'PYEOF'
import os, re, socket, sys, time

alpha_port = int(os.environ["ALPHA_PORT"])
bravo_port = int(os.environ["BRAVO_PORT"])
alpha_uid = os.environ["ALPHA_UID"]
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

# The first self-report teaches the endpoint its ATAK uid; the typed paths all
# wait on that, so nothing before it is a fair test of them.
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

alpha.close()
bravo.close()
sys.exit(1 if failures else 0)
PYEOF
[ $? -eq 0 ] || FAILED=1

step "result"
if [ "$FAILED" = "0" ]; then
    printf '\033[32m  every codec crossed the mesh\033[0m\n'
else
    printf '\033[31m  something did not cross -- logs in %s\033[0m\n' "$WORK"
    exit 1
fi
