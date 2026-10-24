# Charge Station — for M5Stack Cardputer / Cardputer-Adv

**You plug the Cardputer in and… nothing tells you whether it's actually charging. This firmware answers that, in one color.**

It turns the Cardputer into a charging dock display: the screen goes dark, the CPU drops to 80 MHz, the radio is never brought up — and when you press a key it shows a live verdict: **amber = charging**, green = full, red = plugged but no current, grey = not charging.

> No charger IC status register on this board, no current sensor — so the app reads the battery voltage trend and decides from that.

---

## The problem

Cardputer has no charge LED and no charge status anywhere in software (`M5.Power.isCharging()` returns *unknown*). The screen sleeps, the case is closed, and three very different situations look identical:

- it's charging fine,
- it's plugged into a port that can't supply enough (USB hub, Mac port, weak cable),
- the power switch is OFF — in which case **the battery is physically disconnected and it is not charging at all**, no matter how good the charger is.

Charge Station separates those three cases and shows the evidence: a voltage sparkline with real numbers.

## What you see

```
CHARGE STATION                     39m
4.02V          NO CHARGE
~87%           -1.74mV/m
-----------------------------------
No charge: switch/cable/port
[───── voltage sparkline, 40 min ─────]
```

| Color | State | Meaning |
|---|---|---|
| **Amber** | `CHARGING` / `TRICKLE` | Voltage climbing — power is really going in |
| **Green** | `FULL` | Reached 4.15 V, safe to unplug |
| **Red** | `WEAK` | Plugged, but the voltage isn't rising — the source can't keep up |
| **Grey** | `NO CHARGE` | Voltage falling — not charging |
| **Red** | `NO BATT` | Switch is OFF (battery disconnected) or no battery |

Plus: an ETA to full while charging, and an estimated net charge current (`~63mA`) derived from the slope.

## ⚠️ Read this before you complain it doesn't charge

1. **The power switch must be ON.** OFF disconnects the battery — charging is physically impossible, and the ADC reads a floating pin. This is the #1 reason "it doesn't charge".
2. **Give it a few minutes.** Current is estimated from a voltage slope; the first verdict needs ~3 samples (90 s), a trustworthy slope needs ~10 min.
3. **Not every port works.** A Mac USB port or an unpowered hub may supply less than the device draws — you'll see `WEAK` or `NO CHARGE` and a falling curve. That's the app doing its job.

## Install

**Recommended — as an app through M5Launcher** (keeps your NVS, your other apps, your settings):

- Build/flash `firmware.bin` at offset `0x10000`, or install the `.bin` with M5Launcher.
- Overnight logs and your language choice live in this app's own NVS namespace and survive switching between apps.

**Not recommended — full merged image at `0x0`:** it overwrites the NVS partition (language choice, M5Launcher settings, Wi-Fi credentials) with `0xFF`.

## Keys

| Key | Action |
|---|---|
| any key | wake the screen for 3 s |
| `R` | stay on for 5 min (auto-off after) |
| `+` / `-` | calibration trim, ±10 mV steps, ±200 mV (saved) |
| `L` | toggle 中文 / English |
| `1` / `2` | force 中文 / English |
| `9` | clear the overnight log |
| `0` | clear the language choice and reboot into the picker |

First boot asks for a language: `[1] 中文` / `[2] English`, 60 s timeout **defaults to English**.

## Overnight log

Every 10 minutes one record (voltage + state) goes to flash, 144 records = 24 h, ring-overwritten. Read it back over USB:

```bash
python3 tools/read_log.py            # finds the Cardputer by MAC
python3 tools/read_log.py /dev/cu.usbmodem5
```

It prints the whole night's curve, the plug/taper/full timestamps and the average slope. Useful when you want to know *how* it charged, not just *whether*.

## Honest limits

- **It does not make charging faster.** The charger IC and its programming resistor are fixed hardware; this app only removes load (dark screen, 80 MHz, no radio) so the charger gets more of what it can give.
- **Current is an estimate**, derived from the voltage slope and a typical Li-ion voltage→SoC curve — good to a rough order of magnitude, not a coulomb counter.
- **Verdicts need a slope**, so a freshly plugged device says `SAMPLING` for the first minute or two. That's honest, not broken.
- Battery voltage is read on GPIO10 through a 1:2 divider; absolute accuracy is a few tens of mV, hence the `+`/`-` trim.

## Build

```bash
pio run -e cardputer
# app-only image:   .pio/build/cardputer/firmware.bin      (flash at 0x10000)
# merged image:     .pio/build/cardputer/firmware.factory.bin (flash at 0x0, WIPES NVS)
```

PlatformIO + Arduino (M5Unified / M5Cardputer). Target: `esp32-s3`, 8 MB flash, no PSRAM. Flash ~24 %, RAM ~9 %.

## Chinese documentation

→ [README_CN.md](README_CN.md)

## License

MIT — see [LICENSE](LICENSE).
