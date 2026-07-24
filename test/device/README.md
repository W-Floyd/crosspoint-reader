# On-device UI driver (serial button injection)

Drive the **real** device UI from the host over USB serial, to reproduce the
dictionary "stops finding words" failure unattended and capture the heap trend.

## How it works

A dev build (`-DINPUT_INJECTION=1`, set in the `default` env) extends the
existing `CMD:` serial protocol with synthetic button injection:

| Command | Effect |
|---|---|
| `CMD:PRESS:<NAME>` | hold a button down (host times the hold) |
| `CMD:RELEASE:<NAME>` | release it |
| `CMD:TAP:<NAME>` | press + auto-release after a few frames |

`<NAME>` is a logical button — `CONFIRM`, `BACK`, `LEFT`, `RIGHT` (remap-aware,
resolved through `MappedInputManager::physicalIndex`) — or a fixed one: `UP`,
`DOWN`, `POWER`. Injection is layered on `HalGPIO` over the real `InputManager`
and is **excluded from release builds** by the `INPUT_INJECTION` gate.

Combined with the heap logging in `performLookup`
(`[DICT] lookup '<w>': ok=.. found=.. heapFree=.. largestBlock=..`), the host
script drives repeated lookups and records the heap until words stop resolving.

## Flash a dev build

```bash
pio run -e default -t upload      # default env defines INPUT_INJECTION
```

## Device prerequisites (set once, by hand)

- A book is open in the reader.
- A dictionary is selected (Settings → Dictionary).
- Long-press action = **Dictionary** (holding Confirm opens word-select —
  see `LP_MENU_DICTIONARY`).

## Run

```bash
python3 -m venv test/device/.venv && test/device/.venv/bin/pip install pyserial
cd test/device

# See ports / verify the link before a long run (auto-detects the port if omitted):
./.venv/bin/python drive_dictionary.py --list-ports
./.venv/bin/python drive_dictionary.py --selftest        # expects CMD_OK round-trip

# Drive the soak:
./.venv/bin/python drive_dictionary.py --iters 500 --csv heap.csv
```

`--port` is optional — if omitted it auto-detects the single USB-serial device
(macOS `cu.usbmodem*`, Linux `ttyACM*`/`ttyUSB*`), and errors if there are zero
or several. `--selftest` fires one `CMD:TAP:CONFIRM` and confirms the firmware
replies `CMD_OK`, so you know the dev build + link are good before committing to
a 500-cycle run.

It loops: hold Confirm (open word-select) → tap Confirm (look up) → tap Back →
tap Right (next word), turning the page every few cycles. Each lookup prints
`free`/`largest`; it stops and flags when 5 consecutive lookups fail — the
"stopped finding words" repro. Plot `largestBlock` from the CSV: if it decays
while `heapFree` stays high, that's fragmentation.

## Manual poking

```bash
# 115200 8N1; send one command per line
printf 'CMD:TAP:CONFIRM\n' > /dev/tty.usbmodemXXXX
```
