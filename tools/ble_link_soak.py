#!/usr/bin/env python3
"""How usable a BLE link really was, from the phones' own logs.

A link that came up proves nothing about the ten minutes after. BLE phone to
phone has failed in ways no single check shows: a peer interface kept but left
offline while packets still arrived (seven minutes, 2026-09-26), links torn
down every ~60 s by a second connection to a rotated address, a connection
interval stuck at Android's default. Each looked fine at the moment it was
looked at. Bench RF drifts, too, so no BLE change is judged on one run or
without a baseline from this.

    tools/ble_link_soak.py --device LEXUS=adb-R5CW31XHFRA-yxRf77._adb-tls-connect._tcp \\
                           --device NEXUS=192.168.1.55:5555 --minutes 10

Records each phone's logcat for the window (kept under --out, so a run can be
re-read with --from), then reports per phone:

  * each BLE peer's share of Columba's 30 s heartbeats in which it was online
    -- the number that says whether traffic could cross;
  * link events: peer interfaces created, revived, detached; tie-breaks;
    connections declined by the identity tag; writes refused before identity;
  * GATT failures by status (8 supervision timeout, 133 generic, ...);
  * the connection intervals actually negotiated, in ms;
  * bytes carried per peer, and any timed file-part rates.

Needs Columba's peer heartbeat (android_ble_interface.py, 2026-09-27).
"""

import argparse
import collections
import json
import re
import subprocess
import sys
import time
from pathlib import Path

HEARTBEAT = re.compile(r"BLEInterface\[[^\]]*\] peers: (.*)$")
PEER_STATE = re.compile(r"([0-9a-f]{8})\[([^\]]*)\]=(online|offline)")
EVENTS = {
    "peer interface created": re.compile(r"created peer interface for"),
    "revived within grace": re.compile(r"is back within the grace; online again"),
    "detach scheduled": re.compile(r"scheduled detach for [0-9a-f]{8}"),
    "detached after grace": re.compile(r"detached interface for [0-9a-f]{8} after grace"),
    "tie-break closed a link": re.compile(r"keeping the link from"),
    "declined: peer initiates": re.compile(r"Not connecting to .*PEER_INITIATES"),
    "declined: already linked": re.compile(r"Not connecting to .*ALREADY_LINKED"),
    "write refused before identity": re.compile(r"Rejecting pre-identity write"),
}
GATT_FAIL = re.compile(r"onClientConnectionState\(\) - status=(\d+) .*connected=false")
DISCONNECT = re.compile(r"Disconnected from [0-9A-F:]+(?:: Error \(status: (\d+)\)| (\(manual\))|: (Normal disconnect))")
INTERVAL = re.compile(r"onConnectionUpdated\(\) - Device=\S+ interval=(\d+) latency=\d+ timeout=(\d+) status=(\d+)")
BYTES = re.compile(r"BLEPeerInterface\[([^\]]*)\] (RX|TX): (\d+) bytes")
PART_RATE = re.compile(r"last part in \d+ms \((\d+) bit/s\)")


def summarise(lines):
    """Everything this reports, from one phone's log lines."""
    ticks = 0
    online = collections.Counter()
    seen = collections.Counter()
    names = {}
    events = collections.Counter()
    gatt = collections.Counter()
    disconnects = collections.Counter()
    intervals = collections.Counter()
    carried = collections.defaultdict(lambda: {"RX": 0, "TX": 0})
    rates = []
    for line in lines:
        beat = HEARTBEAT.search(line)
        if beat:
            ticks += 1
            for peer, name, state in PEER_STATE.findall(beat.group(1)):
                names[peer] = name
                seen[peer] += 1
                online[peer] += state == "online"
            continue
        for label, pattern in EVENTS.items():
            if pattern.search(line):
                events[label] += 1
        found = GATT_FAIL.search(line)
        if found and found.group(1) != "0":         # 0 is an orderly disconnect
            gatt[int(found.group(1))] += 1
        found = DISCONNECT.search(line)
        if found:
            disconnects["status %s" % found.group(1) if found.group(1) else
                        "manual" if found.group(2) else "normal"] += 1
        found = INTERVAL.search(line)
        if found and int(found.group(1)) > 0:
            intervals["%.2f ms, timeout %d ms%s" % (int(found.group(1)) * 1.25, int(found.group(2)) * 10,
                                                   "" if found.group(3) == "0" else ", status " + found.group(3))] += 1
        found = BYTES.search(line)
        if found:
            carried[found.group(1)][found.group(2)] += int(found.group(3))
        found = PART_RATE.search(line)
        if found:
            rates.append(int(found.group(1)))
    return {
        "heartbeats": ticks,
        "peers": {peer: {"name": names[peer], "heartbeats": seen[peer], "online": online[peer]}
                  for peer in seen},
        "events": dict(events),
        "gatt_failures": {str(k): v for k, v in sorted(gatt.items())},
        "disconnects": dict(disconnects),
        "intervals": dict(intervals),
        "carried": {name: dict(v) for name, v in carried.items()},
        "part_rates_bps": rates,
    }


def report(label, summary, out=sys.stdout):
    print("== %s: %d heartbeat(s)" % (label, summary["heartbeats"]), file=out)
    if not summary["heartbeats"]:
        print("   no heartbeats -- is Columba running a build with the peer heartbeat?", file=out)
    for peer, row in sorted(summary["peers"].items(), key=lambda kv: -kv[1]["heartbeats"]):
        share = 100.0 * row["online"] / row["heartbeats"]
        print("   peer %s %-18s online %5.1f%% of %d heartbeat(s)"
              % (peer, row["name"], share, row["heartbeats"]), file=out)
    for key in ("events", "gatt_failures", "disconnects", "intervals"):
        if summary[key]:
            print("   %s: %s" % (key.replace("_", " "),
                                 ", ".join("%s=%s" % kv for kv in summary[key].items())), file=out)
    for name, moved in summary["carried"].items():
        print("   carried %-18s rx %d B, tx %d B" % (name, moved["RX"], moved["TX"]), file=out)
    if summary["part_rates_bps"]:
        print("   timed file parts: %s bit/s" % ", ".join(map(str, summary["part_rates_bps"])), file=out)


def record(devices, minutes, out_dir):
    """Stream each phone's new log lines to a file for the window."""
    out_dir.mkdir(parents=True, exist_ok=True)
    running = []
    for label, serial in devices:
        handle = open(out_dir / ("%s.log" % label), "w")
        running.append((handle, subprocess.Popen(
            ["adb", "-s", serial, "logcat", "-v", "time", "-T", "1"], stdout=handle,
            stderr=subprocess.DEVNULL)))
    try:
        time.sleep(minutes * 60)
    finally:
        for handle, process in running:
            process.terminate()
            process.wait(timeout=10)
            handle.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--device", action="append", default=[], metavar="LABEL=SERIAL",
                        help="a phone to record; repeat for each")
    parser.add_argument("--minutes", type=float, default=10.0)
    parser.add_argument("--out", type=Path, default=Path("ble-soak-%s" % time.strftime("%Y%m%d-%H%M%S")),
                        help="where the raw logs are kept")
    parser.add_argument("--from", dest="source", type=Path,
                        help="report on logs recorded earlier instead of recording")
    parser.add_argument("--json", action="store_true", help="print the summary as JSON")
    args = parser.parse_args()

    if args.source:
        logs = sorted(args.source.glob("*.log"))
    else:
        devices = [tuple(d.split("=", 1)) if "=" in d else (d, d) for d in args.device]
        if not devices:
            parser.error("give at least one --device, or --from")
        print("recording %d phone(s) for %.0f min into %s" % (len(devices), args.minutes, args.out),
              flush=True)
        record(devices, args.minutes, args.out)
        logs = [args.out / ("%s.log" % label) for label, _ in devices]

    results = {}
    for path in logs:
        with open(path, errors="replace") as handle:
            results[path.stem] = summarise(handle)
    if args.json:
        print(json.dumps(results, indent=2))
    else:
        for label, summary in results.items():
            report(label, summary)


if __name__ == "__main__":
    main()
