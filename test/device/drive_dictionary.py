#!/usr/bin/env python3
"""
Host-side driver for the on-device dictionary soak, over the USB serial link.

Sends CMD:PRESS/RELEASE/TAP button-injection commands (firmware built with
-DINPUT_INJECTION=1, i.e. the `default` env) to drive the REAL reader UI, and
parses the `[DICT] lookup ... largestBlock=` log lines to watch the heap trend
until the dictionary "stops finding words". Requires: pip install pyserial.

DEVICE PREREQUISITES (set once, by hand, before running):
  - A book is open in the reader.
  - A dictionary is selected (Settings > Dictionary).
  - Long-press action = Dictionary (so holding Confirm opens word-select).

Usage:
  python3 test/device/drive_dictionary.py --port /dev/tty.usbmodemXXXX \
      --iters 500 --csv heap.csv
"""
import argparse
import csv
import re
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial not installed. Run: pip install pyserial")

LOOKUP_RE = re.compile(rb"lookup '([^']*)': ok=(\d) found=(\d) heapFree=(\d+) largestBlock=(\d+)")


def send(ser, cmd):
    ser.write(("CMD:" + cmd + "\n").encode())
    ser.flush()


def drain(ser, seconds, rows, state):
    """Read serial for `seconds`, capturing lookup log lines into rows."""
    end = time.time() + seconds
    while time.time() < end:
        line = ser.readline()
        if not line:
            continue
        m = LOOKUP_RE.search(line)
        if m:
            word = m.group(1).decode(errors="replace")
            ok, found = int(m.group(2)), int(m.group(3))
            free, largest = int(m.group(4)), int(m.group(5))
            state["n"] += 1
            rows.append((state["n"], word, ok, found, free, largest))
            print(f"#{state['n']:-5d} '{word}' ok={ok} found={found} free={free} largest={largest}")
            if found == 0 and ok == 1:
                state["misses"] += 1
            elif found == 1:
                state["misses"] = 0  # reset run of failures on any hit


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="serial device, e.g. /dev/tty.usbmodem1101")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--iters", type=int, default=500, help="lookup cycles to drive")
    ap.add_argument("--hold-ms", type=int, default=550, help="Confirm hold to open word-select")
    ap.add_argument("--settle", type=float, default=1.2, help="seconds to wait per e-ink refresh")
    ap.add_argument("--page-every", type=int, default=6, help="turn page every N lookups")
    ap.add_argument("--csv", default="dict_heap.csv")
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    time.sleep(2.0)  # let the port settle
    rows, state = [], {"n": 0, "misses": 0}
    print("Driving dictionary lookups. Ctrl-C to stop.\n")

    try:
        for i in range(args.iters):
            # 1) Hold Confirm to open word-select on the current page.
            send(ser, "PRESS:CONFIRM")
            drain(ser, args.hold_ms / 1000.0, rows, state)
            send(ser, "RELEASE:CONFIRM")
            drain(ser, args.settle, rows, state)
            # 2) Confirm the highlighted word -> performLookup() logs the heap.
            send(ser, "TAP:CONFIRM")
            drain(ser, args.settle, rows, state)
            # 3) Dismiss the definition / not-found popup.
            send(ser, "TAP:BACK")
            drain(ser, args.settle, rows, state)
            # 4) Move to a different word, and turn the page periodically.
            send(ser, "TAP:RIGHT")
            drain(ser, 0.4, rows, state)
            if (i + 1) % args.page_every == 0:
                send(ser, "TAP:DOWN")  # page forward (side button)
                drain(ser, args.settle, rows, state)

            if state["misses"] >= 5:
                print(f"\n*** 5 consecutive lookups stopped finding words at cycle {i} — "
                      f"reproduced 'stopped working'. Watch largestBlock above. ***")
                break
    except KeyboardInterrupt:
        print("\nInterrupted.")
    finally:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["n", "word", "ok", "found", "heapFree", "largestBlock"])
            w.writerows(rows)
        print(f"\nWrote {len(rows)} samples to {args.csv}")
        ser.close()


if __name__ == "__main__":
    main()
