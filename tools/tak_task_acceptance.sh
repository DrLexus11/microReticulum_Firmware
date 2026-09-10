#!/usr/bin/env bash
# Drive the TAK tasking round trip over the real phone, backend and transport.
#
#   tools/tak_task_acceptance.sh              # accept
#   DECISION=decline tools/tak_task_acceptance.sh
#   ADB_DEV=192.168.1.37:46067 tools/... # when adb lists the phone more than once
#
# Issues a signed go-to task from the dev command post, waits for the phone to
# verify and persist it, responds through TaskManager.decide() -- the same call
# the inbox card makes -- and then waits for the command post to verify the
# signed response. Everything runs over IPC and the radio path; nothing here
# reaches into the store directly, which is the point.
#
# Requires the debug build (the harness actions ship only there) and the dev
# command post's database and key under ~/.impr-tak/tasking-dev/.
# Drive the TAK tasking acceptance loop over the real phone/backend/IPC path.
set -uo pipefail
DEV="$HOME/.impr-tak/tasking-dev"
PY="$HOME/.local/share/rnode-rns-venv/bin/python"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PKG="network.columba.app.debug"
# Select by transport id, not serial: this handset's mDNS serial contains a
# space, it is listed several times (mDNS and IP), and its address changes
# between sessions. Waydroid is excluded explicitly -- it is an x86 ATAK host
# on this same machine and adb will happily offer it as a target.
if [ -n "${ADB_DEV:-}" ]; then
    TARGET="-s $ADB_DEV"
else
    TID=$(adb devices -l 2>/dev/null | grep -w device | grep -vi waydroid \
        | grep -o 'transport_id:[0-9]*' | head -1 | cut -d: -f2)
    [ -n "$TID" ] || { echo "no phone on adb; set ADB_DEV"; exit 1; }
    TARGET="-t $TID"
fi
TAG=COLUMBA_TEST
DECISION="${DECISION:-accept}"

adbx() { adb $TARGET "$@"; }
cast() { local a="$1"; shift; adbx shell am broadcast -a "network.columba.test.$a" -n "$PKG/network.columba.app.test.TestReceiver" "$@" >/dev/null 2>&1; }
task() { $PY "$REPO/tools/tak_tasking.py" --db "$DEV/tasks.sqlite3" --identity "$DEV/authority" "$@"; }
await() {
  local pattern="$1" secs="${2:-30}" t0=$SECONDS line
  while (( SECONDS - t0 < secs )); do
    line=$(adbx logcat -d -s "$TAG" 2>/dev/null | grep -E "$pattern" | tail -1)
    [ -n "$line" ] && { echo "$line"; return 0; }
    sleep 1
  done
  return 1
}
step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

AUTHORITY=$($PY -c "import RNS;print(RNS.hexrep(RNS.Identity.from_file('$DEV/authority').get_public_key(),delimit=False))")

step "phone task destination and key"
adbx logcat -c; cast GET_TASK_KEY
KEYLINE=$(await 'task_key |task_key_err' 40) || { echo "FAIL: no task_key reply"; exit 1; }
echo "$KEYLINE"
case "$KEYLINE" in *task_key_err*) echo "FAIL: receiver not ready"; exit 1;; esac
PHONE_PUB=$(sed -n 's/.*public=\([0-9a-f]*\).*/\1/p' <<<"$KEYLINE")

step "trust the command post, and pin the phone"
adbx logcat -c; cast SET_TASK_AUTHORITY --es hex "$AUTHORITY"
await 'task_authority_set |task_authority_err' 30 || { echo "FAIL: authority not set"; exit 1; }
task peer a54 "$PHONE_PUB" 2>&1 | tail -1

step "issue a signed go-to task"
TASK_ID=$(task goto 40.954836 29.093450 --peer a54 --instruction 'Acceptance: verify and respond' --lifetime 900 2>&1 | tail -1)
echo "task $TASK_ID"

step "phone verifies and persists THIS task"
LINE=""
# LoRa across several hops is slower than a LAN path, and a run that gives up
# at sixty seconds reports a failure that is really a short timeout.
for i in $(seq 1 "${ARRIVE_TRIES:-15}"); do
  adbx logcat -c; cast LIST_TASKS; sleep 4
  LINE=$(adbx logcat -d -s $TAG 2>/dev/null | grep "task id=$TASK_ID" | tail -1)
  [ -n "$LINE" ] && break
done
[ -z "$LINE" ] && { echo "FAIL: task never arrived"; task list | grep "$TASK_ID"; exit 1; }
echo "$LINE"

step "respond: $DECISION"
adbx logcat -c; cast RESPOND_TASK --es id "$TASK_ID" --es decision "$DECISION"
await 'task_decided |task_respond_err' 30 || { echo "FAIL: no decision"; exit 1; }

step "command post sees the signed response"
for i in $(seq 1 20); do
  OUT=$(task list 2>/dev/null | grep "$TASK_ID")
  if grep -qE '"state": "(accepted|declined)"' <<<"$OUT"; then
    echo "$OUT"; echo; echo "ACCEPTANCE COMPLETE"; exit 0
  fi
  sleep 6
done
echo "last state: $(task list | grep "$TASK_ID")"
echo "FAIL: response never verified at the command post"; exit 1
