#!/usr/bin/env bash
# Drive a CoT round trip between the deck bridge and the phone's endpoint.
#
#   tools/tak_cot_acceptance.sh
#
# Both ends run a local CoT endpoint -- that is the whole design -- so neither
# is a server and there is no address to type. This harness stands in for ATAK
# on both sides at once:
#
#   deck  : connects to 127.0.0.1:18087, the bridge's own endpoint
#   phone : connects through `adb forward` to the phone's 127.0.0.1:18087
#
# so the entire loop is observable from here without ATAK running at all. When
# ATAK *is* running it simply becomes a second client on the phone's endpoint;
# it does not change the path being tested.
#
# Requires: the bridge running on the deck (tools/cot_bridge.py --team Cyan),
# and the phone configured with the same team and fleet secret with its
# endpoint switched on.
set -uo pipefail

# The endpoint moved off 8087 on 2026-09-13, which is ATAK's own default CoT
# input -- the two were competing for the same port and the loser retried
# forever, which reads as connection flapping. The forward port moves with it
# so the two do not collide on this machine.
FORWARD_PORT="${FORWARD_PORT:-18088}"
ENDPOINT_PORT=18087
TEAM="${TEAM:-Cyan}"

# Select by transport id, not serial: this handset is listed several times
# (mDNS and IP) and its address changes between sessions. Waydroid is excluded
# explicitly -- it is an x86 ATAK host on this same machine and adb will
# happily offer it as a target.
if [ -n "${ADB_DEV:-}" ]; then
    TARGET="-s $ADB_DEV"
else
    TID=$(adb devices -l 2>/dev/null | grep -w device | grep -vi waydroid \
        | grep -o 'transport_id:[0-9]*' | head -1 | cut -d: -f2)
    [ -n "$TID" ] || { echo "no phone on adb; set ADB_DEV"; exit 1; }
    TARGET="-t $TID"
fi

step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
fail() { printf '\033[31mFAIL: %s\033[0m\n' "$*"; exit 1; }
pass() { printf '\033[32mPASS: %s\033[0m\n' "$*"; }

step "deck bridge is listening"
ss -ltn 2>/dev/null | grep -q "127.0.0.1:$ENDPOINT_PORT" \
    || fail "no bridge on 127.0.0.1:$ENDPOINT_PORT -- start tools/cot_bridge.py --team $TEAM"
pass "bridge up"

step "phone endpoint is reachable"
adb $TARGET forward "tcp:$FORWARD_PORT" "tcp:$ENDPOINT_PORT" >/dev/null \
    || fail "adb forward failed"
# `adb forward` accepts the local connection whether or not anything is
# listening on the device, then closes it when the device side refuses. So a
# successful connect proves nothing; only a connection that stays open does.
python3 - "$FORWARD_PORT" <<'READY' \
    || fail "phone is not listening on $ENDPOINT_PORT -- turn the endpoint on from the TAK settings page"
import socket, sys, time
try:
    sock = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
except OSError:
    sys.exit(1)
sock.settimeout(1.5)
time.sleep(0.5)
try:
    # An immediate clean EOF is adb telling us the device end refused.
    if sock.recv(1) == b"":
        sys.exit(1)
except socket.timeout:
    pass          # Open and quiet is exactly right: nothing to say yet.
except OSError:
    sys.exit(1)
sys.exit(0)
READY
pass "phone endpoint up"

step "round trip"
python3 - "$FORWARD_PORT" "$ENDPOINT_PORT" <<'PYEOF'
import socket, sys, time

phone_port, deck_port = int(sys.argv[1]), int(sys.argv[2])


def read_events(sock, seconds):
    """Collect whole CoT events for a while. Same framing rule as the endpoint:
    there is no length prefix, so the closing tag is all there is to go on."""
    sock.settimeout(0.5)
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
            end = buffer.index(b"</event>") + len(b"</event>")
            events.append(buffer[:end].decode("utf-8", "replace"))
            buffer = buffer[end:]
    return events


def marker(uid, callsign, lat, lon):
    return (
        '<event uid="%s" type="a-h-G" how="h-g-i-g-o" version="2.0">'
        '<point lat="%s" lon="%s" hae="48.0" ce="9999999.0" le="9999999.0"/>'
        '<detail><contact callsign="%s"/></detail></event>' % (uid, lat, lon, callsign)
    )


deck = socket.create_connection(("127.0.0.1", deck_port), timeout=5)
phone = socket.create_connection(("127.0.0.1", phone_port), timeout=5)
# Drain whatever either end has queued before the measurement starts.
read_events(deck, 1)
read_events(phone, 1)

failures = []

stamp = str(int(time.time()))
deck_marker = marker("deck-" + stamp, "DECK.MARK", "40.9549", "29.0934")
phone_marker = marker("phone-" + stamp, "PHONE.MARK", "40.9601", "29.1002")

print("  deck -> phone")
deck.sendall(deck_marker.encode())
seen = read_events(phone, 12)
if any("deck-" + stamp in event for event in seen):
    print("    arrived")
else:
    failures.append("deck marker did not reach the phone (saw %d events)" % len(seen))

print("  phone -> deck")
phone.sendall(phone_marker.encode())
seen = read_events(deck, 12)
if any("phone-" + stamp in event for event in seen):
    print("    arrived")
else:
    failures.append("phone marker did not reach the deck (saw %d events)" % len(seen))

deck.close()
phone.close()
for problem in failures:
    print("  " + problem)
sys.exit(1 if failures else 0)
PYEOF
STATUS=$?

if [ $STATUS -eq 0 ]; then
    pass "CoT crossed the mesh in both directions"
else
    fail "round trip incomplete -- see above"
fi
