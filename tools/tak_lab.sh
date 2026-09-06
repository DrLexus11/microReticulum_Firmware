#!/usr/bin/env bash
# Bring the indoor TAK lab up or down.
#
#   impr-tak start          gateway + OpenTAKServer, the full picture
#   impr-tak start --simple gateway only, ATAK connects straight to it
#   impr-tak status         what is listening, and the destination hash
#   impr-tak stop           everything this script started
#
# Two shapes, and which one you want depends on whether you need a map on the
# deck. The gateway is a CoT *producer*; OpenTAKServer owns distribution and
# draws the web map. Two things both trying to be the server is the arrangement
# to avoid, so in full mode the gateway does not listen at all.
#
#   full:    mesh -> gateway -> UDP 8087 -> OTS -> ATAK on 8088, web UI on 8081
#   simple:  mesh -> gateway -> TCP 8087 -> ATAK
#
# Nothing here needs root. RabbitMQ runs as a container because SteamOS has a
# read-only /usr, and OTS refuses to start without it.

set -uo pipefail

REPO="${REPO:-/home/deck/projects/microReticulum_Firmware}"
OTS_DIR="${OTS_DIR:-/home/deck/projects/OpenTAKServer}"
RNS_PY="${RNS_PY:-/home/deck/.local/share/rnode-rns-venv/bin/python3}"
RUN_DIR="${RUN_DIR:-$HOME/.impr-tak}"
GATEWAY_IDENTITY="${GATEWAY_IDENTITY:-$HOME/.rns_cot_gateway_identity}"

# OTS defaults, from opentakserver/defaultconfig.py. Its CoT input is UDP,
# which is what the gateway forwards to; EUDs get the TCP port.
OTS_UDP_PORT="${OTS_UDP_PORT:-8087}"
OTS_TCP_PORT="${OTS_TCP_STREAMING_PORT:-8088}"
OTS_WEB_PORT="${OTS_LISTENER_PORT:-8081}"
RABBIT_CONTAINER="${RABBIT_CONTAINER:-impr-rabbitmq}"

mkdir -p "$RUN_DIR"

say()  { printf '\033[1m%s\033[0m\n' "$*"; }
warn() { printf '\033[33m%s\033[0m\n' "$*"; }
fail() { printf '\033[31m%s\033[0m\n' "$*" >&2; }

# Never pkill -f on a pattern that appears in this script's own command line.
# It matches the shell running it and takes the whole session down with it,
# which has happened twice on this bench.
pid_of() { pgrep -f "$1" 2>/dev/null | head -1; }

port_busy() { ss -ltnu 2>/dev/null | grep -q ":$1 "; }

start_rabbit() {
    if port_busy 5672; then
        say "rabbitmq: already listening on 5672"
        return 0
    fi
    if ! command -v docker >/dev/null; then
        fail "rabbitmq needs docker, which is not on PATH"
        return 1
    fi
    if docker ps -a --format '{{.Names}}' | grep -qx "$RABBIT_CONTAINER"; then
        docker start "$RABBIT_CONTAINER" >/dev/null && say "rabbitmq: container restarted"
    else
        docker run -d --name "$RABBIT_CONTAINER" \
            -p 5672:5672 -p 15672:15672 rabbitmq:3-management >/dev/null \
            && say "rabbitmq: container created"
    fi
    # It takes a few seconds to accept connections, and OTS exits rather than
    # waits if it cannot reach the broker.
    for _ in $(seq 1 30); do
        port_busy 5672 && { say "rabbitmq: ready"; return 0; }
        sleep 1
    done
    fail "rabbitmq did not come up within 30s"
    return 1
}

start_ots() {
    if [ -n "$(pid_of 'opentakserver')" ]; then
        say "opentakserver: already running"
        return 0
    fi
    if port_busy "$OTS_WEB_PORT"; then
        warn "port $OTS_WEB_PORT is already taken by something else."
        warn "Set OTS_LISTENER_PORT to move the web UI, or stop the other process."
        return 1
    fi
    # MediaMTX is the video stack. Nothing here streams video, and it is one
    # more daemon to install on a read-only filesystem.
    OTS_MEDIAMTX_ENABLE=False \
    nohup "$OTS_DIR/.venv/bin/opentakserver" > "$RUN_DIR/ots.log" 2>&1 &
    disown
    for _ in $(seq 1 40); do
        port_busy "$OTS_WEB_PORT" && { say "opentakserver: up on :$OTS_WEB_PORT"; return 0; }
        sleep 1
    done
    fail "opentakserver did not come up; see $RUN_DIR/ots.log"
    return 1
}

start_gateway() {
    local mode="$1"
    if [ -n "$(pid_of 'cot_gateway.py')" ]; then
        say "gateway: already running"
        return 0
    fi
    local args=(--identity "$GATEWAY_IDENTITY")
    if [ "$mode" = full ]; then
        # Producer only: OTS serves the clients.
        args+=(--forward "127.0.0.1:$OTS_UDP_PORT" --no-tcp)
    fi
    setsid nohup "$RNS_PY" "$REPO/tools/cot_gateway.py" "${args[@]}" \
        > "$RUN_DIR/gateway.log" 2>&1 < /dev/null &
    disown
    for _ in $(seq 1 30); do
        grep -q "destination <" "$RUN_DIR/gateway.log" 2>/dev/null && break
        sleep 1
    done
    grep -E "^\[gateway\] destination" "$RUN_DIR/gateway.log" 2>/dev/null \
        || { fail "gateway did not announce; see $RUN_DIR/gateway.log"; return 1; }
}

cmd_start() {
    local mode=full
    [ "${1:-}" = "--simple" ] && mode=simple

    if [ "$mode" = full ]; then
        start_rabbit || return 1
        start_ots || warn "continuing without OTS; the gateway still runs"
    fi
    start_gateway "$mode" || return 1

    echo
    say "Ready."
    local ip
    ip=$(ip route get 1.1.1.1 2>/dev/null | awk '{print $7; exit}')
    if [ "$mode" = full ]; then
        echo "  web UI       http://${ip:-localhost}:$OTS_WEB_PORT"
        echo "  ATAK server  ${ip:-localhost}  port $OTS_TCP_PORT  TCP (not SSL)"
    else
        echo "  ATAK server  ${ip:-localhost}  port 8087  TCP (not SSL)"
        echo "  or multicast 239.2.3.1:6969 with no configuration at all"
    fi
    echo "  logs         $RUN_DIR/"
    echo
    echo "Paste the destination hash above into Columba:"
    echo "  Settings -> Position Reporting -> gateway, then switch it on."
}

cmd_status() {
    printf '%-14s %s\n' "rabbitmq" "$(port_busy 5672 && echo 'up' || echo 'down')"
    printf '%-14s %s\n' "opentakserver" "$([ -n "$(pid_of opentakserver)" ] && echo 'up' || echo 'down')"
    printf '%-14s %s\n' "gateway" "$([ -n "$(pid_of cot_gateway.py)" ] && echo 'up' || echo 'down')"
    echo
    ss -ltnu 2>/dev/null | grep -E ":(5672|8081|8087|8088|8089) " || echo "(no lab ports listening)"
    echo
    grep -E "^\[gateway\] destination" "$RUN_DIR/gateway.log" 2>/dev/null
    tail -4 "$RUN_DIR/gateway.log" 2>/dev/null | grep -E "^\[gateway\] (new|move)" || true
}

cmd_stop() {
    local pid
    pid=$(pid_of 'cot_gateway.py') && [ -n "$pid" ] && kill "$pid" && say "gateway stopped"
    pid=$(pid_of 'opentakserver')  && [ -n "$pid" ] && kill "$pid" && say "opentakserver stopped"
    # RabbitMQ is left running: it is a container, it costs little, and OTS is
    # slow to start without a warm broker.
    say "rabbitmq left running (docker stop $RABBIT_CONTAINER to remove it)"
}

case "${1:-start}" in
    start)  shift || true; cmd_start "${1:-}" ;;
    status) cmd_status ;;
    stop)   cmd_stop ;;
    *)      echo "usage: impr-tak {start [--simple]|status|stop}" >&2; exit 2 ;;
esac
