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
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial not installed. Run: pip install pyserial")

LOOKUP_RE = re.compile(rb"lookup '([^']*)': ok=(\d) found=(\d) heapFree=(\d+) largestBlock=(\d+)")

# USB-serial device-name hints for auto-detection (macOS cu.*, Linux ttyACM/USB).
PORT_HINTS = ("usbmodem", "usbserial", "wchusbserial", "ttyACM", "ttyUSB")


def list_serial_ports():
    return list(list_ports.comports())


def print_ports():
    ports = list_serial_ports()
    if not ports:
        print("No serial ports found.")
        return
    print("Available serial ports:")
    for p in ports:
        print(f"  {p.device:<28} {p.description or ''}")


def autodetect_port():
    """Return the single likely device port, or None if 0 / ambiguous."""
    candidates = [p.device for p in list_serial_ports()
                  if any(h in p.device for h in PORT_HINTS) and "Bluetooth" not in p.device]
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        print("Auto-detect: no likely USB-serial device found.")
    else:
        print(f"Auto-detect: ambiguous ({', '.join(candidates)}); pass --port to choose.")
    return None


def selftest(ser):
    """Fire one CMD:TAP and confirm the firmware's CMD_OK round-trip."""
    print("Self-test: sending CMD:TAP:CONFIRM ...")
    ser.reset_input_buffer()
    send(ser, "TAP:CONFIRM")
    end = time.time() + 3.0
    while time.time() < end:
        line = ser.readline()
        if not line:
            continue
        text = line.decode(errors="replace").strip()
        if text:
            print(f"  <- {text}")
        if b"CMD_OK" in line:
            print("Self-test PASSED: injection is live (dev build with INPUT_INJECTION).")
            return True
        if b"CMD_ERR" in line:
            print("Self-test FAILED: firmware rejected the button name.")
            return False
    print("Self-test FAILED: no CMD_OK within 3s.\n"
          "  - Is the device flashed with the `default` env (-DINPUT_INJECTION=1)?\n"
          "  - Right --port? (try --list-ports)")
    return False


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
    ap.add_argument("--port", help="serial device (auto-detected if omitted)")
    ap.add_argument("--list-ports", action="store_true", help="list serial ports and exit")
    ap.add_argument("--selftest", action="store_true", help="verify the CMD_OK round-trip and exit")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--iters", type=int, default=500, help="lookup cycles to drive")
    ap.add_argument("--hold-ms", type=int, default=550, help="Confirm hold to open word-select")
    ap.add_argument("--settle", type=float, default=1.2, help="seconds to wait per e-ink refresh")
    ap.add_argument("--page-every", type=int, default=6, help="turn page every N lookups")
    ap.add_argument("--csv", default="dict_heap.csv")
    args = ap.parse_args()

    if args.list_ports:
        print_ports()
        return

    port = args.port or autodetect_port()
    if not port:
        print("No port. Use --list-ports to see options, then pass --port.")
        sys.exit(2)

    ser = serial.Serial(port, args.baud, timeout=0.2)
    time.sleep(2.0)  # let the port settle
    print(f"Connected: {port} @ {args.baud}")

    if args.selftest:
        ok = selftest(ser)
        ser.close()
        sys.exit(0 if ok else 1)

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
