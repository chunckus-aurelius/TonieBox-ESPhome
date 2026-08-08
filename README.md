# TonieBox ESPHome

ESPHome components and a reference configuration for running a **Toniebox
ESP32-S3 board (rev v1.6.C)** as a Home Assistant media player, with its NFC
reader, audio codec, accelerometer, buttons and RGB LED all exposed as
entities.

This replaces the stock firmware entirely. It does not talk to the Boxine
cloud and does not read or write Tonie content — it reports the tag UID to
Home Assistant and lets you decide what that means.

> **Status:** working, but board-revision specific. The pin map comes from
> rev v1.6.C. The `board:` key in the example config is a placeholder you
> should confirm against your own module, and the two ADC sensors scale by
> the rev v1.6.C resistor dividers — check yours if readings look wrong.

## Components

| Component  | Bus | Purpose |
|------------|-----|---------|
| `trf7962a` | SPI | TI TRF7962A ISO15693 / SLIX NFC reader — fires `on_tag_present` / `on_tag_removed` and reports the UID |
| `dac3100`  | I2C | TI TLV320DAC3100 I2S audio codec — clocking, output routing, volume, mute |
| `lis3dh`   | I2C | ST LIS3DH 3-axis accelerometer — X/Y/Z as sensors |

Each reads its configuration back at the end of `setup()` and prints it in
`dump_config()`, so a codec that ACKs on I2C but is silent, or an init table
that failed to apply, is visible in the boot log rather than presenting as a
mystery.

## Requirements

- ESPHome with the **`esp-idf`** framework (the config sets this explicitly)
- The `mixer` and `resampler` speaker platforms, and the `speaker` media
  player platform
- Home Assistant, if you want the tag scans to go anywhere useful. A
  streaming source such as Music Assistant handles playback.

## Usage

Point `external_components` at this repository:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/chunckus-aurelius/TonieBox-ESPhome
      ref: main
    components: [trf7962a, dac3100, lis3dh]
```

Then take [`TonieESP.yaml`](TonieESP.yaml) as your starting point. It is a
complete working config, not a skeleton.

You will need a `secrets.yaml` alongside it defining:

```yaml
wifi_ssid: "..."
wifi_password: "..."
api_key: "..."       # base64 API encryption key
ota_password: "..."
```

## Things that will bite you

The example config is heavily commented, and most of those comments exist
because something failed in a way that took a while to explain. The
load-bearing ones:

- **There is no PSRAM.** Every audio buffer has to fit in internal SRAM, and
  44.1 kHz playback only just does. Raising any `buffer_duration` or
  `buffer_size` tends to fail at runtime, not at compile time.
- **The `on_boot` sequence must stay a single lambda.** Splitting it into
  separate actions with `- delay:` between them does not work: `DelayAction`
  is asynchronous and returns immediately, so the I2C bus gets created
  part-way through power-up and every device on it fails to probe.
- **Never add `output:` entries for GPIO45 or GPIO47.** `GPIOBinaryOutput::setup()`
  calls `turn_off()` at `HARDWARE(800)` — after the bus is already up — which
  cuts the peripheral rail out from under everything. Those pins belong to the
  `on_boot` lambda and nowhere else.
- **The media player sample rates must match the I2S speaker's.** All three
  are 44100 in the example for a reason.

## Attribution

These are fresh ESPHome-native implementations, but the hardware knowledge
behind them is not original work. Pin assignments, register maps and init
sequences were derived from the
[toniebox-reverse-engineering](https://github.com/toniebox-reverse-engineering)
project — chiefly `teddybox` (`board_def.h` and its `trf7962a`, `dac3100` and
`lis3dh` components), which in turn credits `RvX_TLV320DAC3100` for the
PLL/NDAC/MDAC clock values and an earlier `electricimp/trf7962a` for the
TRF7962A register map.

TRF7962A quirk handling is cited inline to TI application note `sloa140b.pdf`
by section. Per-file attribution is in the header comment of each component.

## License

[GPLv3](LICENSE).

ESPHome is dual-licensed — its C++/runtime code (`.c`, `.cpp`, `.h`, `.hpp`,
`.tcc`, `.ino`) is GPLv3 and its Python code is MIT. The C++ in this
repository derives from and links against that runtime, so GPLv3 applies here
too.

**One caveat, stated plainly:** at the time of writing, the upstream
`toniebox-reverse-engineering` repositories referenced above ship **no license
file**, which by default means all rights reserved. The register addresses and
values used here are datasheet-derived facts about TI and ST silicon rather
than upstream's creative expression, and the implementations are independently
written — but if you intend to build on this commercially, that gap is worth
resolving with the upstream authors rather than assuming.
