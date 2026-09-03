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

    # Configure the control lines BEFORE opening.
    #
    # serial.Serial(port, ...) opens immediately, and pyserial asserts DTR and
    # RTS by default while doing so. On this board's CP2102 auto-reset circuit
    # that is a reset pulse on EN: attaching to watch the log rebooted the very
    # thing being watched. It is visible on the OLED, which blanks for a second
    # each time, and it is why captures sometimes showed a board that had just
    # restarted, or nothing at all. Constructing without a port lets the line
    # states be set first; pyserial then applies them as it opens.
    p = serial.Serial(baudrate=args.baud, timeout=0.2,
                      dsrdtr=False, rtscts=False)
    p.port = args.port
    p.dtr = True
    p.rts = True
    p.open()

    # Clear HUPCL, so the kernel does not drop the modem lines when we close.
    # Lowering DTR/RTS on close is a reset pulse on EN for this board's CP2102
    # auto-reset circuit -- the board reboots when the log viewer EXITS, which is
    # why consecutive captures kept showing a freshly booted node and why the
    # OLED blanks for a second around every attach/detach.
    try:
        import termios
        attrs = termios.tcgetattr(p.fileno())
        attrs[2] &= ~termios.HUPCL          # c_cflag
        termios.tcsetattr(p.fileno(), termios.TCSANOW, attrs)
    except Exception as exc:                # non-POSIX, or an exotic driver
        print(f"-- could not clear HUPCL ({exc}); attaching may reset the board")

    if args.reset:
        p.dtr = False  # GPIO0 high
        p.rts = True   # EN low
        time.sleep(0.1)
        p.rts = False  # EN high
        p.dtr = False

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
