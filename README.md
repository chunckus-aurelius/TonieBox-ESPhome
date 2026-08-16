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
| `lis3dh`   | I2C | ST LIS3DH 3-axis accelerometer — X/Y/Z as sensors, plus optional hardware tap detection via `click:` |

Each reads its own configuration back off the chip at the end of `setup()` and
prints it in `dump_config()`, so a codec that ACKs on I2C but is silent, or an
init table that failed to apply, is visible in the boot log rather than
presenting as a mystery.

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

Getting it onto the hardware — opening the shell, soldering the UART bridge,
and flashing over ESPHome Web — is covered step by step in the
[flashing guide](docs/flashing-guide.md).

Once it is running, the box reports events and Home Assistant decides what they
mean. The [automations guide](docs/automations.md) has worked examples,
including a single automation that serves every box you own by resolving which
one a figure was placed on.

### Tap detection

`lis3dh` can use the LIS3DH's hardware click detector, which the example config
leaves off because the threshold depends on the box and how hard you tap:

```yaml
lis3dh:
  id: accelerometer
  click:
    threshold: 40       # 1-127, in units of full-scale/128
    time_limit: 10      # counts of the ODR period
    time_latency: 20
    time_window: 100
    on_click:
      - logger.log:
          format: "tap: CLICK_SRC=0x%02X"
          args: ['src']
```

`src` is the raw `CLICK_SRC` byte, so `on_click` can tell which axis was struck
and on which side — bit 0/1/2 for X/Y/Z, bit 3 for the sign. Two things worth
knowing before wiring it to anything:

- Enabling `click:` raises the accelerometer to **400 Hz**. At the 100 Hz used
  otherwise, a tap transient falls between samples and nothing fires. This costs
  a little more current, which matters on battery.
- A single physical tap usually trips **two or three axes** ~100–230 ms apart.
  Debounce inside `on_click` rather than in your automation — a `globals:`
  `uint32_t` holding the last `millis()` and a 400 ms gate is enough. Gating on
  a single axis instead does not work: which axis faces sideways depends on how
  the box is sitting.
- If you also use tilt gestures, note that a tap fires *during* a tilt, so the
  two trip each other unless `on_click` is gated on the box being upright. The
  [automations guide](docs/automations.md) has the measured orientation figures.

The click registers are read back off the chip after they are written, so a
misapplied setting shows up in the boot log rather than as silence.

## Stock Toniebox vs this firmware

Stock behaviour is taken from the official tonies manual. Status is what runs
on hardware today, not what is planned.

**Legend:** ✅ working · 🟡 partial · ⬜ not started · ⛔ deliberately out of scope

### Core playback

| Stock function | Stock behaviour | Here | Notes |
|---|---|---|---|
| Play a Tonie | Place figure on top, audio starts | ✅ | Tag UID goes to Home Assistant as `tag_scanned`; HA/Music Assistant decides what plays |
| Pause on removal | Lift the figure, playback pauses | 🟡 | The box reports removal; pausing is a Home Assistant automation — see the [automations guide](docs/automations.md). Removal is debounced 1.5 s in `trf7962a`, so a knocked figure does not trigger it |
| Resume position | Replacing the same figure resumes where it stopped, unless another was played in between | ⬜ | Stock keys this on last-played tag, not a timer |
| Skip chapter / change track | Tap either side of the box | 🟡 | `lis3dh` supports it — set `click:` and bind `on_click`. Proven end to end on hardware, driving next/previous track against a Music Assistant queue, but **not enabled in the example config**, since the threshold needs tuning per box. Left reports `side: pos`, right `side: neg` — see the [automations guide](docs/automations.md). GPIO14/INT1 is not used: CLICK_SRC is polled instead |
| Fast-forward / rewind | Tilt the box to one side | ⬜ | Separate gesture from the tap; direction is user-configurable in stock |
| Volume | Squeeze big ear up, small ear down | ✅ | Short press, 50–800 ms. A `Max Volume` number caps every path — ears, Home Assistant, Music Assistant. Defaults to 70% |

### Power

| Stock function | Stock behaviour | Here | Notes |
|---|---|---|---|
| Turn on — ear | Press an ear | 🟡 | **Both ears proven on hardware** with an `ext1` mask, including the ears still reading correctly after a wake — but the example config here still ships `ext0`, which takes a single wake pin, so out of the box it is big ear only |
| Turn on — charger | Place on the charging station | 🟡 | Same `ext1` work, also proven on hardware. Not in the example config. GPIO7 sits low while charging, so the mask must be built at sleep time — armed off the charger, excluded while docked — or the box can never sleep on the charger |
| Idle power off | Automatic after 10 minutes | ✅ | Deep sleep with a configurable `Sleep Timeout` in minutes. **Defaults to 5 minutes**; set it to 0 to disable sleep entirely |
| Manual power off | — | ✅ | Hold either ear 1–12 s, or the Power switch in Home Assistant |
| Restart | Upside down + both ears ~10 s, off the charger | ⬜ | All three inputs are already available. Upside down is a steady-state check on the vertical axis rather than a gesture, which makes it far more robust than tap detection. If you build it, gate the ears' power-off hold on *not* being upside down, or an aborted attempt releases straight into a power toggle |
| Factory reset | Upside down + both ears ~10 s, on the charger | ⛔ | Not implemented on purpose — there is no cloud state to reset, and it shares a gesture with restart |

### Indicators

| Stock function | Stock behaviour | Here | Notes |
|---|---|---|---|
| Ready | Steady green | ✅ | Green from boot until the box sleeps, so a wake is visible before any audio plays. Low battery and error colours still need a precedence model |
| Low battery | Steady orange | 🟡 | A `Battery` % sensor exposes the estimate, but nothing drives the LED from it yet — that needs a colour precedence model. Thresholds from the stock NVS dump: 3.60 V critical, 3.67/3.70 V low with 30 mV hysteresis |
| Charging | Reported via LED | 🟡 | The `Charge Sense` ADC reads the charger rail directly (~4.9 V present, 0 V absent), so charging is visible as a number. No binary sensor in the example config |
| Connecting / downloading | Pulsing and flashing blue | ⛔ | No Tonie cloud here; Wi-Fi state is visible in Home Assistant |
| Error | Flashing red plus a spoken message | ⛔ | Errors surface in the ESPHome log |

### Toniebox 2 features

| Feature | Here | Notes |
|---|---|---|
| Sleep timer with light | ⬜ | Reachable — RGB LED plus the media player |
| Sunrise alarm | ⬜ | Reachable, but a sleeping box cannot be woken by Home Assistant; needs the box awake or an RTC timer wake source |
| Bluetooth headphones | ⛔ | No Bluetooth audio path on this hardware |
| USB-C charging | ⛔ | Hardware |
| Tonieplay games | ⛔ | Proprietary content |

### Beyond stock

Things this firmware does that no Toniebox does. Same legend as above, and the
same rule: status is what the example config in this repo gives you, with
anything proven only on the author's boxes called out as such.

| Feature | Here | Notes |
|---|---|---|
| Home Assistant native integration | ✅ | Every sensor, the LED and the media player as first-class entities. No cloud, no account, no app |
| Play anything, not just Tonies | ✅ | Music Assistant streams any source. A figure becomes a trigger for an arbitrary automation rather than a fixed piece of content |
| Any ISO 15693 tag works | ✅ | The UID goes to HA as `tag_scanned`, so a hotel key card or a library tag drives playback exactly as a Tonie figure does |
| Stereo pair across two boxes | 🟡 | `output_channels` per player in Music Assistant splits L/R. Works, but the two boxes drift — they run on independent crystals and a Universal Group carries no clock |
| OTA updates | ✅ | Over Wi-Fi, no disassembly |
| Battery percentage | ✅ | Estimated from the pack curve, verified on hardware at 4.04 V and 3.96 V. Stock shows a three-level indicator |
| Raw pack and charge-rail voltages | ✅ | Real numbers rather than a colour, so charging can be watched rather than guessed at |
| Accelerometer readout | ✅ | All three axes as HA sensors, plus hardware tap detection on the LIS3DH |
| Gestures as HA events | 🟡 | Proven on hardware — `esphome.tonie_tap` carries `axis` and `side` — but the `click:` block is not enabled in the example config, since the threshold needs tuning per box. See the [automations guide](docs/automations.md) |
| Configurable idle timeout | ✅ | `Sleep Timeout` in minutes, 0 to disable. Stock is a fixed 10 minutes with no way to change it |
| Volume ceiling | ✅ | `Max Volume` clamps every path — ears, Home Assistant, Music Assistant. Stock has no cap |
| The LED as an HA light | ✅ | Turning `Status LED` off hands the light to Home Assistant entirely, so any automation can paint it |
| Test tone | ✅ | A button that proves I2S → DAC → amp → speaker with no network involved. Worth keeping during bring-up |
| Headphone detect | 🟡 | Exposed as a binary sensor on GPIO48. Nothing acts on it and the polarity has not been verified |
| Timer wake from deep sleep | ⬜ | A sleeping box can only be woken by a wake pin. Waking on a schedule would need `esp_sleep_enable_timer_wakeup` alongside the wake mask |
| TTS announcements over music | ⛔ | Unlikely to fit at 44.1 kHz on this hardware — there is no PSRAM, so every buffer is internal SRAM |

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
- **The idle timeout will fire mid-track if you re-arm it only from
  `on_state`.** That trigger fires on state *publishes* — play, pause, volume —
  not continuously, so a long track that publishes nothing for `Sleep Timeout`
  minutes lets the box sleep while someone is listening. Observed on hardware
  2026-08-15. The example config now polls the player state on a 10 s interval
  instead; keep that interval if you cut anything else out of the config.

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
