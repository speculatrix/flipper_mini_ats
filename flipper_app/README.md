# ATS-Mini Remote

Control an [ATS-Mini](https://esp32-si4732.github.io/ats-mini/) SI4732 receiver from your Flipper Zero. The app uses the receiver's [ad hoc remote protocol](https://esp32-si4732.github.io/ats-mini/remote.html) over Wi-Fi.

## Hardware needed

- An ATS-Mini receiver
- The Flipper Zero **Wi-Fi devboard**, flashed with the UART-to-TCP bridge firmware from the [project repository](https://github.com/speculatrix/flipper_mini_ats) (the wifi_bridge folder)

The Flipper and the ATS-Mini both have USB device ports only, so they can't talk over a cable. The devboard joins the receiver's Wi-Fi network and passes data between the Flipper's GPIO UART (pins 13/14, 115200 baud) and the receiver's TCP port 60000. The app powers the devboard from the 5 V pin while it runs.

## Setup

- On the ATS-Mini, set **Settings > Wi-Fi > AP Only** and **Settings > TCP Port > Ad hoc**
- Plug the flashed Wi-Fi devboard onto the Flipper's GPIO header
- Open **Apps > GPIO > ATS-Mini Remote**

## Remote screen

The screen shows the band, mode, frequency, step, bandwidth, AGC, volume, RSSI, SNR, battery voltage and an RSSI bar.

- **Up / Down:** volume up / down. In FM, hold to repeat.
- **Hold Up / Down:** next / previous mode (AM, LSB, USB only)
- **Right / Left:** tune up / down one step. Also scrolls the receiver's menus after an OK click.
- **Hold Right / Left:** next / previous band
- **OK:** encoder click
- **Hold OK:** encoder press
- **Back:** main menu

## Other screens

- **Set Frequency:** enter a frequency in kHz, for example 107900 for 107.9 MHz. It has to be inside the receiver's current band.
- **Commands:** band, mode, step, bandwidth, AGC/attenuator, backlight, calibration and sleep, each up and down
- **Help:** setup and control summary
