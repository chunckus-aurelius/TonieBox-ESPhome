# Toniebox → ESPHome: teardown, UART wiring, and flashing

A full walkthrough of getting [`TonieESP.yaml`](../TonieESP.yaml) onto a Toniebox:
opening the shell, soldering a UART bridge, and flashing over ESPHome Web.

- **Board:** `TONIEBOX-ESP32` rev 1.6.C
- **Target profile:** `esp32-s3-devkitc-1`

![Assembled Toniebox](images/00-assembled.jpg)

> **Check your board first.** Three different processors have shipped in the
> Toniebox. The two TI-based boards are the older revisions — this firmware is
> only for the ESP32 main board.

## Tools and parts

- Phillips screwdriver
- Soldering iron, flux paste, and 4 Dupont wires — three for the UART bridge,
  one to bridge J100
- USB-to-serial adapter, 3.3 V logic — FTDI FT232R or CH340
- Chrome or Edge — ESPHome Web runs on Web Serial

## Teardown

### 1. Remove the base disk

The round plate on the bottom comes off by rotating the Tonie to the left. Place
it on a stable surface (the floor, as pictured) and rotate while pressing down —
it is held by clips, not screws. Underneath is the only Phillips screw holding
the internals and exterior shell together.

![Bottom plate removed](images/01-foot-removed.jpg)
*Bottom plate removed*

![Screw exposed underneath](images/02-bottom-screw.jpg)
*First screw, exposed*

### 2. Lift the top panel, remove the battery tray

Unclip the NiMH battery wires from the black battery tray and unplug its JST
connector from the main board. The battery tray is held to the white internal
frame by two screws either side of the barrel jack. With those out, tip the tray
away from the frame and it slides free.

![Top panel out](images/03-top-open.jpg)
*Top panel out*

![Shell splits open](images/04-shell-open.jpg)
*Shell splits: battery side / board side*

### 3. Free the main board

With the battery tray gone, tip the main board away from the internal frame.
Then work the six-pin connector out of the antenna socket, rocking the board
gently side to side while pulling.

![Battery unclipped, screws out](images/05-battery-out.jpg)
*Battery unclipped, screws out*

![NFC antenna connector disconnected](images/06-antenna-connector.jpg)
*NFC antenna connector disconnected*

### 4. Board out

As the board lifts free it is still tethered to the speaker and the ear buttons.
Unplug both, and the board is out.

## The main board

Silkscreen reads `TONIEBOX-ESP32 1.6.C, 2022-04-27`.

![Mainboard free of the shell](images/07-board-out.jpg)
*Mainboard free of the shell*

## Wiring the UART bridge

Solder three cut Dupont wires to the GND / TX / RX pads near the barrel jack —
blue = GND, green = RX, yellow = TX. Bridge **J100** to put the ESP32-S3 into
download mode.

![UART bridge wires soldered](images/08-uart-wired.jpg)
*Adapter socket + GND/TX/RX bridge wires*

Power the board through its barrel jack. In download mode the LED should *not*
light up as it normally would. Plug the Dupont wires into the adapter as
follows:

| Board pad | Wire   | Adapter pin |
|-----------|--------|-------------|
| GND       | blue   | GND         |
| RX        | green  | TX          |
| TX        | yellow | RX          |

UART is cross-connected, so TX and RX are swapped relative to the board's
pinout. Then plug the adapter into a USB port on your computer or Home Assistant
host.

![Adapter powered, ready to flash](images/09-adapter-powered.jpg)
*Adapter powered, ready to flash*

![Main board after desoldering](images/10-board-clean.jpg)
*Main board with a bit of flux left from desoldering the Dupont wires after
flashing ESPHome*

## Flashing with ESPHome Web

### Connect the board

Open [web.esphome.io](https://web.esphome.io) in Chrome or Edge and grant it
access to the adapter's serial port.

![Grant serial port access](images/20-serial-connect.png)
*Grant serial port access*

Once connected, the device card appears. Use the upload button next to **Prepare
for first use**.

![ESP Device connected](images/21-prepare-first-use.png)
*Device connected — upload button circled*

### Create a configuration

Choose **Empty Configuration** and paste in [`TonieESP.yaml`](../TonieESP.yaml).

![Empty Configuration](images/22-create-config.png)
*Choose Empty Configuration*

The config reads four values with `!secret`, so define them in `secrets.yaml`
before compiling or the build fails on the first one: `wifi_ssid`,
`wifi_password`, `api_key` and `ota_password`. See [the README](../README.md#usage).

When prompted to match a board, pick **ESP32-S3 DevKitC-1**.

![Select matching board](images/23-select-board.png)
*Pick `esp32-s3-devkitc-1`*

### Install the firmware

The first install has to go over UART — pick whichever option matches where the
adapter is plugged in. Once OTA is configured, later flashes go over Wi-Fi.

![Pick an install method](images/24-install-method.png)
*Pick an install method*

**Plug into this computer** or **Plug into your Home Assistant server** compiles
and flashes directly:

![Firmware flashing in progress](images/25-installing.png)
*Firmware flashing in progress*

If you chose **Download firmware binary** instead, pick **Factory image** after
compiling, then flash it with your own tool:

![Choose what to download](images/26-download-factory.png)
*After compiling, choose Factory image*

## Adding to Home Assistant

With ESPHome installed, the Toniebox is discovered automatically under
**Settings → Devices**.

![Discovered in Home Assistant](images/27-ha-discovered.png)
*Discovered in Home Assistant — press Add*

## Device dashboard

**Controls** has an LED toggle, a test tone, and speaker volume. **Sensors**
covers the accelerometer, battery and charge voltage, both ear buttons,
headphone detect, and the NFC tag reader.

![Controls and sensors](images/28-dashboard.png)
*Controls and Sensors*

Tap a Tonie figurine on top of the box and the **NFC Tag UID** sensor updates in
real time in the Activity feed — antenna, reader, and firmware all confirmed
working end to end.

![Tag read](images/29-dashboard-tag.png)
*A Tonie figurine, read successfully*

![Tag removed](images/30-dashboard-tag-removed.png)
*NFC Tag UID returns to `none` when the figurine is lifted off*
