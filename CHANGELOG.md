# Changelog

All notable user-visible changes. Version numbers stay in the same patch line
by preference — no cosmetic 1.2.0 jumps.

## v1.0.11

- Adds an estimated net charge current (`~63mA`) under the slope, so "is it
  really going in, and how fast" has a number instead of just an arrow.
- Adds `9` to clear the overnight log without reflashing.

## v1.0.10

- Fixes the language picker missing its `[2]` label (only `[1] 中文` was drawn).

## v1.0.9

- Fixes `1`/`2` doing nothing on the language picker: the picker now uses the
  same key path as the running app instead of its own polling loop.
- Adds `0` to clear the language choice and reboot back into the picker.
- Adds `1`/`2` as language shortcuts at any time.

## v1.0.8

- Adds a key echo on the picker screen (`key seen: x`) for troubleshooting.

## v1.0.7

- Adds bilingual UI (Chinese / English) with a boot-time language picker:
  60 s timeout, defaults to English. `L` toggles at any time.

## v1.0.6

- Adds an overnight log: one record per 10 minutes, 144 records = 24 h, kept in
  flash so you can read the whole night back over USB.
- Adds plug / taper / full event timestamps.
- Fixes the slope being thrown off by the plug-in voltage step (IR drop) — the
  slope window now starts after the step.

## v1.0.5

- Fixes the sparkline reading empty slots after a restart (ring buffer index).
- Lowers verdict thresholds to the noise floor of a 20-minute window.

## v1.0.4

- Fixes bogus readings with the switch OFF: a sample is only accepted when it
  is inside 3.0–4.3 V and the 8-sample spread is ≤ 60 mV.

## v1.0.3

- Adds a voltage calibration trim (`+`/`-`, ±10 mV), saved to flash.

## v1.0.2

- Adds the 40-minute voltage sparkline.
- `R` stay-on now auto-clears after 5 minutes; any key returns to 3 s.
- Retunes slope thresholds for a 1750 mAh pack (1.0 / 0.2 / −0.2 mV per minute).

## v1.0.1

- Persists the sample ring to flash so the slope survives reboots.

## v1.0.0

- First release: dark screen, 80 MHz, no radio, instant verdict from the
  battery voltage trend.
