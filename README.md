# ATS-Mini Remote for Flipper Zero

Control an [ATS-Mini](https://esp32-si4732.github.io/ats-mini/) receiver from a Flipper Zero
using the receiver's [ad hoc remote protocol](https://esp32-si4732.github.io/ats-mini/remote.html).

## Why Wi-Fi rather than USB?

The Flipper's USB-C port and the ATS-Mini's USB-C port are both USB *devices*;
neither can be a USB host, so they can't talk over a cable. The ATS-Mini serves
the same protocol on TCP port 60000, so the Flipper Wi-Fi devboard bridges it:

```
Flipper app ──UART (GPIO 13/14, 115200)──▶ Wi-Fi devboard ──TCP :60000──▶ ATS-Mini
```

## Parts

| Directory      | What                                                              |
|----------------|-------------------------------------------------------------------|
| `flipper_app/` | Flipper Zero app (`.fap`), built with [ufbt](https://github.com/flipperdevices/flipperzero-ufbt) |
| `wifi_bridge/` | UART↔TCP bridge firmware for the Wi-Fi devboard (PlatformIO)      |

## 1. Set up the ATS-Mini

* `Settings → Wi-Fi → AP Only`. The receiver creates the open network `ATS-Mini` at 10.1.1.1.
* `Settings → TCP Port → Ad hoc`

To use your home network instead, put the receiver in `Connect` mode, then set
`BRIDGE_WIFI_SSID`, `BRIDGE_WIFI_PASSWORD` and `BRIDGE_ATS_HOST` (e.g. `atsmini.local`)
in `wifi_bridge/include/config.h`.

## 2. Flash the Wi-Fi devboard

Flashing replaces whatever is on the devboard (e.g. Marauder or the Black Magic
debugger). You can flash those back later.

1. Hold **BOOT**, plug the devboard into your PC by its own USB-C port, then release BOOT.
2. ```sh
   cd wifi_bridge
   pio run -t upload
   ```
3. Unplug it from the PC and plug it onto the Flipper's GPIO header.

## 3. Build and install the Flipper app

```sh
pip install ufbt
cd flipper_app
ufbt            # build
ufbt launch     # install and run on a USB-connected Flipper
```

The app is under **Apps → GPIO → ATS-Mini Remote**. The app powers the devboard
from the 5 V pin while it runs.

## Using it

**Remote** screen:

| Button        | Action                      | Protocol |
|---------------|-----------------------------|----------|
| Up / Down     | Volume up / down (hold to repeat in FM) | `V` / `v` |
| Hold Up / Down | Next / previous mode (AM/LSB/USB only) | `M` / `m` |
| Right / Left  | Tune up / down one step           | `R` / `r` |
| Hold Right / Left | Next / previous band          | `B` / `b` |
| OK            | Encoder click               | `e` |
| Hold OK       | Encoder short press         | `E` |
| Back          | Main menu                   |          |

Left/Right also scroll the receiver's menus after an OK click.

The screen shows the band, mode, frequency, step, bandwidth, AGC, volume, RSSI,
SNR, battery voltage and an RSSI bar. These come from the receiver's monitor log
(`t`). The app turns the log on and keeps it running.

**Set Frequency** sends `F<Hz>`. Enter the frequency in kHz (e.g. `107900` for 107.9 MHz).
It has to be inside the receiver's current band.

**Commands** has band, mode, step, bandwidth, AGC/attenuator, backlight,
calibration and sleep, each up/down.

## Bridge status messages

The bridge sends its own lines starting with `~` (`~WIFI:CONNECTING`,
`~TCP:CONNECTED`, `~TCP:LOST`, …). The app shows them on the Remote screen when
it is not connected.
