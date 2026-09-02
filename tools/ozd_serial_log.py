#!/usr/bin/env python3
"""Serial log reader for the OZD fixture.

Holds DTR and RTS high so attaching does not reset the board, and prints both
plain firmware output and KISS-framed log frames (0x80) with timestamps. An
explicit --reset performs an EN-only reset after opening the port so boot output
can be captured without pulling GPIO0 into the ROM downloader.
"""

import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=0.0, help="0 = run forever")
    ap.add_argument("--reset", action="store_true", help="reset once and capture boot")
    args = ap.parse_args()

    p = serial.Serial(args.port, args.baud, timeout=0.2,
                      dsrdtr=False, rtscts=False)
    if args.reset:
        p.dtr = False  # GPIO0 high
        p.rts = True   # EN low
        time.sleep(0.1)
        p.rts = False  # EN high
        p.dtr = False
    else:
        p.dtr = True
        p.rts = True

    t0 = time.time()
    inframe = False
    esc = False
    frame = bytearray()
    line = bytearray()

    def emit(tag, text):
        print("[%7.2f] %s %s" % (time.time() - t0, tag, text), flush=True)

    emit("--", "attached to %s" % args.port)
    while True:
        if args.seconds and (time.time() - t0) > args.seconds:
            return 0
        for x in p.read(4096):
            if x == 0xC0:
                if inframe and frame and frame[0] == 0x80:
                    emit("KISS", bytes(frame[1:]).decode("utf-8", "replace").rstrip())
                inframe = True
                esc = False
                frame.clear()
            elif inframe:
                if esc:
                    frame.append(0xC0 if x == 0xDC else 0xDB if x == 0xDD else x)
                    esc = False
                elif x == 0xDB:
                    esc = True
                else:
                    frame.append(x)
            if x in (10, 13):
                if line:
                    emit("TXT ", line.decode("utf-8", "ignore").rstrip())
                    line.clear()
            elif 32 <= x < 127:
                line.append(x)


if __name__ == "__main__":
    sys.exit(main())
