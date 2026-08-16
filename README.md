# TonieBox ESPHome

**Custom firmware that turns a Toniebox into a Home Assistant speaker. No cloud,
no account, no app.**

Put a figure on the box and it tells Home Assistant which one. What happens next
is entirely yours to decide — a playlist, an audiobook, the radio, a bedtime
routine, a light that comes on in another room. The Boxine cloud is never
contacted, and nothing here stops working if it goes away.

The box still behaves like a Toniebox. Squeeze the ears for volume, tap the
sides to change track, hold an ear to switch it off. It sleeps on its own and
wakes when you press an ear or drop it on the charger.

- **Any tag works.** Tonie figures, hotel key cards, library tags — any ISO 15693
  tag becomes a trigger for anything Home Assistant can do.
- **Play anything.** Your own audiobooks and music through Music Assistant, not
  just purchased content.
- **Everything is an entity.** NFC reader, accelerometer, battery percentage,
  buttons, RGB LED and speaker, all first-class in Home Assistant.
- **Pair two boxes** as left and right channels.
- **OTA updates.** Solder once, then never open the box again.

**What it takes:** opening the shell, soldering three wires for a one-time
serial flash, and the ESPHome + Home Assistant setup you probably already run.
The [flashing guide](docs/flashing-guide.md) walks through it with photos.

> **Board revision.** The pin map and the two ADC divider ratios come from a rev
> v1.6.C board — silkscreen `TONIEBOX-ESP32 1.6.C`. Other ESP32-S3 Tonieboxes
> are likely close but unverified, so confirm the `board:` key against your own
> module and check the voltage readings look sane.

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

`lis3dh` can use the LIS3DH's hardware click detector. The example config
enables it, and exposes the threshold as a `Tap Threshold` number so you can
tune it live rather than reflashing to find the right value:

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
  `uint32_t` holding the last `millis()` and a 400 ms gate is enough. Note the
  debounce reports only the *first* axis to trip, which matters if you then
  filter on axis in Home Assistant: see the
  [automations guide](docs/automations.md) for both sides of that trade.
- If you also use tilt gestures, note that a tap fires *during* a tilt, so the
  two trip each other unless `on_click` is gated on the box being upright. The
  [automations guide](docs/automations.md) has the measured orientation figures.

The click registers are read back off the chip after they are written, so a
misapplied setting shows up in the boot log rather than as silence.

## Stock Toniebox vs this firmware

Every function in the official tonies manual, and whether it works here.
Status is what runs on hardware today, not what is planned — nothing below is
marked working because it ought to be.

**Legend:** ✅ working · 🟡 partial · ⬜ not started · ⛔ deliberately out of scope

### Core playback

| Stock function | Stock behaviour | Here | Notes |
|---|---|---|---|
| Play a Tonie | Place figure on top, audio starts | ✅ | Tag UID goes to Home Assistant as `tag_scanned`; HA/Music Assistant decides what plays |
| Pause on removal | Lift the figure, playback pauses | 🟡 | The box reports removal; pausing is a Home Assistant automation — see the [automations guide](docs/automations.md). Removal is debounced 1.5 s in `trf7962a`, so a knocked figure does not trigger it |
| Resume position | Replacing the same figure resumes where it stopped, unless another was played in between | ⬜ | Stock keys this on last-played tag, not a timer |
| Skip chapter / change track | Tap either side of the box | ✅ | Working end to end against a Music Assistant queue. Taps are debounced 400 ms and gated on the box being upright, so a tap during a tilt does not trip both gestures. Left reports `side: pos`, right `side: neg`; the [automations guide](docs/automations.md) has the automation. `Tap Threshold` is a live number — tune it rather than reflashing. GPIO14/INT1 is not used: CLICK_SRC is polled instead |
| Fast-forward / rewind | Tilt the box to one side | 🟡 | The box fires `esphome.tonie_tilt` with `side: pos`/`neg` while a tilt is held, repeating every 600 ms so Home Assistant can seek in steps. **No example automation consumes it yet** — that is the missing half. The tilt signal is `Y + Z`, not either axis alone: the part sits rotated ~45°, so the two move together. Measured +0.84 tilted one way, −0.73 the other, against ~0 upright |
| Volume | Squeeze big ear up, small ear down | ✅ | Short press, 50–800 ms. A `Max Volume` number caps every path — ears, Home Assistant, Music Assistant. Defaults to 70% |

### Power

| Stock function | Stock behaviour | Here | Notes |
|---|---|---|---|
| Turn on — ear | Press an ear | ✅ | Both ears, via an `ext1` mask built at sleep time. The wake pads are held through deep sleep to keep their pullups and released in `on_boot` — get that wrong and the buttons read stuck after every wake |
| Turn on — charger | Place on the charging station | ✅ | Same `ext1` mask. GPIO7 sits low while charging, so it is armed off the charger and excluded while docked — an already-low pin in an `ANY_LOW` mask is an instant wake, so a docked box that included it could never sleep |
| Idle power off | Automatic after 10 minutes | ✅ | Deep sleep with a configurable `Sleep Timeout` in minutes. **Defaults to 5 minutes**; set it to 0 to disable sleep entirely |
| Manual power off | — | ✅ | Hold either ear 1–12 s, or the Power switch in Home Assistant |
| Restart | Upside down + both ears ~10 s, off the charger | ✅ | Upside down is a steady-state check on the vertical axis, far more robust than tap detection. Three things in here cost time to find: the threshold allows for the box being **held** (X reads 0.79–1.26 inverted in the hand, so a tight one resets the counter on hand shake); the ears' power-off hold is gated on *not* being upside down, or an aborted attempt releases into a power toggle; and it blinks the LED first, because the reboot takes ~5 s and is otherwise completely silent. Charger state is not checked — with no factory reset to distinguish it from, it has nothing to select between |
| Factory reset | Upside down + both ears ~10 s, on the charger | ⛔ | Not implemented on purpose — there is no cloud state to reset, and it shares a gesture with restart |

### Indicators

| Stock function | Stock behaviour | Here | Notes |
|---|---|---|---|
| Ready | Steady green | ✅ | Green from boot until the box sleeps, so a wake is visible before any audio plays. A `Status LED` precedence model decides which state wins: Home Assistant first, then Bedtime, then charging, then low battery, then plain awake |
| Low battery | Steady orange | 🟡 | Off the charger and under 20%, the LED paints orange instead of green. **Written and in the config, but not yet seen fire** — it needs a pack that low. A missing reading is treated as "unknown", never as "flat", so a box that has never been measured off its charger does not cry wolf on every boot. Stock NVS thresholds for reference: 3.60 V critical, 3.67/3.70 V low with 30 mV hysteresis |
| Charging | Reported via LED | ✅ | The LED ramps red through green as the charge climbs, then breathes green when the pack is full. `Charge Sense` also reads the charger rail directly (~4.9 V present, 0 V absent), so charging is visible as a number as well as a colour |
| Connecting / downloading | Pulsing and flashing blue | ⛔ | No Tonie cloud here; Wi-Fi state is visible in Home Assistant |
| Error | Flashing red plus a spoken message | ⛔ | Errors surface in the ESPHome log |

### Toniebox 2 features

| Feature | Here | Notes |
|---|---|---|
| Sleep timer with light | 🟡 | A `Bedtime` switch and a `Bedtime Timer` number. Runs alongside whatever is already playing — as the stock timer does — holds the LED warm amber, fades it over the final minute, then stops and sleeps. **In the config, not yet confirmed on hardware.** Note the fade leaves the visible band about nine seconds in: below ~60% this LED is not visible through the shell |
| Sunrise alarm | 🟡 | **No firmware needed** — the LED is an ordinary HA light, so the alarm is one automation, written up in the [automations guide](docs/automations.md). The box must be awake when it fires, so leave it on its charger with `Stay Awake on Charger` on, or set `Sleep Timeout` to 0. Waking a box that genuinely slept is the separate problem, and needs a timer wake source |
| Bluetooth headphones | ⛔ | No Bluetooth audio path on this hardware |
| USB-C charging | ⛔ | Hardware |
| Tonieplay games | ⛔ | Proprietary content |

### Beyond stock

Things this firmware does that no Toniebox does. Same legend as above, and the
same rule: status is what the example config in this repo gives you.

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
| Gestures as HA events | ✅ | `esphome.tonie_tap` carries `axis` and `side`; `esphome.tonie_tilt` carries `side` and repeats while held. Any automation can consume them — see the [automations guide](docs/automations.md) |
| Configurable idle timeout | ✅ | `Sleep Timeout` in minutes, 0 to disable. Stock is a fixed 10 minutes with no way to change it |
| Volume ceiling | ✅ | `Max Volume` clamps every path — ears, Home Assistant, Music Assistant. Stock has no cap |
| The LED as an HA light | ✅ | An RGB light entity, and a `Status LED` switch that hands it to Home Assistant entirely — with that off, nothing on the device repaints over an automation's colour. That is what makes the sunrise alarm possible without firmware |
| Test tone | 🟡 | A button that proves I2S → DAC → amp → speaker with no network involved — the fastest way to tell a wiring or codec fault from a Music Assistant one. **Shipped commented out** in `TonieESP.yaml`; uncomment the `rtttl:` and `button:` blocks for a first flash on new hardware, then comment them out again |
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
