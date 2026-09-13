#!/usr/bin/env bash
# Bring up the deck's Waydroid node, and launch what runs on it.
#
#   tools/waydroid_node.sh status
#   tools/waydroid_node.sh atak        # ATAK on the deck
#   tools/waydroid_node.sh columba     # the mesh endpoint it talks to
#   tools/waydroid_node.sh restart     # clear a stale session
#
# Why this exists rather than the taskbar icon: Waydroid tracks a session
# separately from the container, and the two can disagree. When they do, every
# launch path -- `show-full-ui`, `app launch`, and the desktop entries built on
# them -- fails with:
#
#   RuntimeError: Already tracking a session
#
# and nothing opens. `waydroid status` still says "Session: RUNNING", so it
# reads as healthy; the tell is "IP address: UNKNOWN" on a session that claims
# to be up. Seen on the deck 2026-09-13, where the taskbar icon had been dying
# silently. Starting a session is not the fix and makes it worse, because the
# stale one has to be stopped first.
#
# So: check for that exact disagreement, clear it, then launch.
set -uo pipefail

ATAK_PACKAGE="com.atakmap.app.civ"
COLUMBA_PACKAGE="network.columba.app.debug"

export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

say() { printf '[waydroid] %s\n' "$*"; }

status_field() {
    waydroid status 2>/dev/null | awk -F'\t' -v key="$1" '$1 == key":" {print $2}'
}

healthy() {
    local session ip
    session="$(status_field Session)"
    ip="$(status_field 'IP address')"
    # A session with no address is the stale case: it claims to be running and
    # refuses every launch.
    [ "$session" = "RUNNING" ] && [ -n "$ip" ] && [ "$ip" != "UNKNOWN" ]
}

ensure_session() {
    if healthy; then
        say "session up at $(status_field 'IP address')"
        return 0
    fi
    if [ "$(status_field Session)" = "RUNNING" ]; then
        say "session claims RUNNING with no address -- stale, clearing it"
        waydroid session stop >/dev/null 2>&1
        sleep 3
    fi
    say "starting a session"
    nohup waydroid session start >/dev/null 2>&1 &
    for _ in $(seq 40); do
        sleep 1
        healthy && { say "up at $(status_field 'IP address')"; return 0; }
    done
    say "session did not come up; see /var/lib/waydroid/waydroid.log"
    return 1
}

launch() {
    ensure_session || return 1
    say "launching $1"
    waydroid app launch "$1" 2>&1 | sed 's/^/[waydroid] /'
}

case "${1:-status}" in
    status)
        waydroid status
        healthy && say "healthy" || say "NOT healthy -- run: $0 restart"
        ;;
    restart)
        waydroid session stop >/dev/null 2>&1
        sleep 3
        ensure_session
        ;;
    atak)     launch "$ATAK_PACKAGE" ;;
    columba)  launch "$COLUMBA_PACKAGE" ;;
    *)        say "unknown command: $1"; exit 2 ;;
esac
