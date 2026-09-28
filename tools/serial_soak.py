#!/usr/bin/env python3
"""Watch a board over its serial console for hours: restarts, heap, ESP-NOW.

CarriedIssues #1 has two open questions a short look cannot answer. Does an
ESP-NOW peer bring on the TASK_WDT resets? And what are the roughly daily
software restarts -- Rev 2-2's bootlog showed three, 89 063 s, 102 034 s and
90 147 s apart, which is RNS_LOW_MEMORY_REBOOT's signature (ESP.restart at
<=2 % free heap), a leak? Both are answered by the same record: every boot and
its reason, a crash's backtrace, the heap curve, and ESP-NOW's peer count, over
windows long enough to hold several restarts.

    tools/serial_soak.py --port /dev/ttyACM2 --hours 16 --out ~/.impr-tak/soak/rev2-2.log
    tools/serial_soak.py --summary ~/.impr-tak/soak/rev2-2.log

Attaching must not reset the board, or the reset under study is the one we
caused: DTR and RTS are set before the port opens (as ozd_serial_log.py does).
A board that reboots drops its USB port; the logger waits and reattaches, and
the boot banner that follows is the record of why.

Only the lines that answer the question are kept -- these boards log at trace
level, and a full log would be gigabytes by morning.
"""

import argparse
import re
import statistics
import sys
import time
from datetime import datetime
from pathlib import Path

KEEP = re.compile(
    r"boot reason=|\[boot\]|\[mem\]|\[tables\]|\[diag\]|\[espnow\]|\[blepeer\]|\[init\] !!!|NOT RELAYING|"
    r"Guru Meditation|panic|abort\(\)|Backtrace|Rebooting|TASK_WDT|task_wdt|brownout|"
    r"Low memory|LOW_MEMORY|heap_caps_malloc failed|out of memory")
MEM = re.compile(r"\[mem\] internal=(\d+) largest=(\d+) psram=(\d+)")
BLOCKS = re.compile(r"alloc_blocks=(\d+) free_blocks=(\d+)")
ESPNOW = re.compile(r"\[espnow\] state=(\S+) ch=(\d+) peers=(\d+)")
TABLES = re.compile(r"\[(?:tables|diag)\] ((?:\w+=\d+ ?)+)")
BOOT = re.compile(r"boot reason=([^\r\n]*?)(?: prev=(\S+))?$|\[boot\] reset reason: ([^,(]+)")


def open_quietly(port, baud):
    import serial
    link = serial.Serial(baudrate=baud, timeout=0.5, dsrdtr=False, rtscts=False)
    link.port = port
    link.dtr = True
    link.rts = True
    link.open()
    return link


def record(port, baud, hours, out):
    out.parent.mkdir(parents=True, exist_ok=True)
    end = time.time() + hours * 3600
    pending = b""
    with open(out, "a", buffering=1) as log:
        log.write("%s ### soak started on %s for %.1f h\n" % (stamp(), port, hours))
        while time.time() < end:
            try:
                link = open_quietly(port, baud)
            except Exception:
                time.sleep(2)
                continue
            log.write("%s ### attached\n" % stamp())
            try:
                while time.time() < end:
                    pending += link.read(8192)
                    *lines, pending = re.split(rb"[\r\n]+", pending)
                    for raw in lines:
                        line = raw.decode("utf-8", "replace")
                        if KEEP.search(line):
                            log.write("%s %s\n" % (stamp(), line.strip()[-300:]))
            except Exception as error:          # the board rebooted, or the cable moved
                log.write("%s ### port lost: %s\n" % (stamp(), error))
                try:
                    link.close()
                except Exception:
                    pass
                time.sleep(2)
        log.write("%s ### soak ended\n" % stamp())


def stamp():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def summarise(path, out=sys.stdout):
    boots, mem, espnow, crashes, tables, blocks = [], [], [], [], [], []
    first = last = None
    for line in open(path, errors="replace"):
        when = line[:19]
        try:
            moment = datetime.strptime(when, "%Y-%m-%d %H:%M:%S")
        except ValueError:
            continue
        first = first or moment
        last = moment
        found = BOOT.search(line)
        if found:
            boots.append((when, (found.group(1) or found.group(3) or "").strip(), found.group(2) or ""))
        found = MEM.search(line)
        if found:
            mem.append((moment, int(found.group(1)), int(found.group(2))))
        found = BLOCKS.search(line)
        if found:
            blocks.append((int(found.group(1)), int(found.group(2))))
        found = TABLES.search(line)
        if found:
            tables.append(dict((k, int(v)) for k, v in re.findall(r"(\w+)=(\d+)", found.group(1))))
        found = ESPNOW.search(line)
        if found:
            espnow.append((found.group(1), int(found.group(3))))
        if re.search(r"Guru Meditation|Backtrace|TASK_WDT|task_wdt|abort\(\)", line):
            crashes.append(line.strip()[:200])
    if first is None:
        print("nothing recorded", file=out)
        return
    hours = (last - first).total_seconds() / 3600
    print("window: %s -> %s (%.1f h)" % (first, last, hours), file=out)
    print("boots: %d" % len(boots), file=out)
    for when, reason, prev in boots:
        print("  %s  %s%s" % (when, reason, "  prev=" + prev if prev else ""), file=out)
    if crashes:
        print("crash lines: %d, first: %s" % (len(crashes), crashes[0]), file=out)
    if mem:
        internal = [m[1] for m in mem]
        largest = [m[2] for m in mem]
        span = (mem[-1][0] - mem[0][0]).total_seconds() / 3600
        slope = (internal[-1] - internal[0]) / span if span > 0 else 0.0
        print("internal heap: first %d, last %d, min %d; %+.0f B/h over %.1f h"
              % (internal[0], internal[-1], min(internal), slope, span), file=out)
        print("largest block: first %d, last %d, min %d, median %d"
              % (largest[0], largest[-1], min(largest), statistics.median(largest)), file=out)
    if blocks:
        print("blocks: allocated %d -> %d (max %d), free %d -> %d (max %d)"
              % (blocks[0][0], blocks[-1][0], max(b[0] for b in blocks),
                 blocks[0][1], blocks[-1][1], max(b[1] for b in blocks)), file=out)
    if tables:
        print("tables (first -> last, max):", file=out)
        for key in tables[0]:
            values = [t.get(key, 0) for t in tables]
            print("  %-10s %6d -> %6d, max %d" % (key, values[0], values[-1], max(values)), file=out)
    if espnow:
        peers = [p for _, p in espnow]
        states = sorted({s for s, _ in espnow})
        print("esp-now: states %s; peers min %d max %d" % (", ".join(states), min(peers), max(peers)), file=out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--hours", type=float, default=12.0)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--summary", type=Path, help="summarise a recorded log")
    args = parser.parse_args()
    if args.summary:
        summarise(args.summary)
    elif args.port and args.out:
        record(args.port, args.baud, args.hours, args.out)
    else:
        parser.error("give --port and --out to record, or --summary to read")


if __name__ == "__main__":
    main()
