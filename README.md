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
| `trf7962a_nfc` | SPI | TI TRF7962A ISO15693 NFC reader on ESPHome's `nfc` framework — `on_tag` / `on_tag_removed` and the `nfc` binary sensor |
| `tlv320dac3100` | I2C | TI TLV320DAC3100 I2S audio codec — an `audio_dac` platform: clocking, output routing, volume, mute |
| `dac3100`  | I2C | **Deprecated**, replaced by `tlv320dac3100`. Kept so existing configs still build |
| `trf7962a` | SPI | **Deprecated**, replaced by `trf7962a_nfc`. Kept so existing configs still build |
| `lis3dh`   | I2C | ST LIS3DH 3-axis accelerometer — X/Y/Z as sensors, plus optional hardware tap detection via `click:` |

`lis3dh` reads its configuration back off the chip at the end of `setup()`
and prints it in `dump_config()`, so an init table that failed to apply is
visible in the boot log rather than presenting as a mystery. `tlv320dac3100`
logs the clock settings it computed from `sample_rate`.

`tlv320dac3100` and `trf7962a_nfc` are the drivers proposed for upstream
ESPHome. `tlv320dac3100` has small changes so it also builds on releases
before 2026.11, and `trf7962a_nfc` is the upstream `trf7962a` renamed so it can
sit beside the old one here.

To move a config from `dac3100` to `tlv320dac3100`, replace the `dac3100:`
block with the `audio_dac:` block from [`TonieESP.yaml`](TonieESP.yaml), add
`audio_dac: audio_codec` to the I2S speaker, and swap the name in the
`external_components` list. The new driver is louder at the same volume
setting, so recheck your `max_volume` cap. `dac3100` will be removed in a
later release.

To move a config from `trf7962a` to `trf7962a_nfc`, replace the `trf7962a:`
block with the one from [`TonieESP.yaml`](TonieESP.yaml) and swap the name in
the `external_components` list. `irq_pin` is now required. `on_tag_present`
becomes `on_tag`, and `x` is now a string such as `E0-04-03-50-1E-07-E7-ED`;
the example strips the dashes so Home Assistant receives the same tag IDs as
before. A figure counts as removed after three missed scans, and the
`removal_debounce` option is gone. `trf7962a` will be removed in a later
release.

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
    components: [trf7962a_nfc, tlv320dac3100, lis3dh]
```

Then take [`TonieESP.yaml`](TonieESP.yaml) as your starting point. It is a
complete working config, not a skeleton.

You will need a `secrets.yaml` alongside it defining:

```yaml
wifi_ssid: "..."
wifi_password: "..."
api_key: "..."       # base64 API encryption key
```

OTA updates are encrypted with the same `api_key`, so there is no separate OTA
password. **Updating a box that was flashed with the older config, which used
`ota_password`:** a box running ESPHome 2026.9 or later with an API key takes
the new config straight away. If the install stops with "refusing to send the
image in plaintext", nothing was flashed. Do that one install with this block,
then remove the `password:` and `allow_plaintext_upload` lines again:

```yaml
ota:
  - platform: esphome
    password: !secret ota_password
    encryption:
      allow_plaintext_upload: true
```

Getting it onto the hardware — opening the shell, soldering the UART bridge,
and flashing over ESPHome Web — is covered step by step in the
[flashing guide](docs/flashing-guide.md).

Once it is running, the box reports events and Home Assistant decides what they
mean. The [automations guide](docs/automations.md) has worked examples,
including a single automation that serves every box you own by resolving which
one a figure was placed on.

If you would rather not write automations at all, the community
[TonieESP](https://github.com/vitals5/tonieesp) integration (HACS) does the same
job from a sidebar panel: a shared figure library, resume, and gesture mapping.
It is a separate project, not maintained here. Run it **or** the guide's
figure, tap and tilt automations, not both — with both, every figure starts
playback twice and every gesture skips or seeks twice.

**AirPlay needs a speaker group.** To stream to a box from an Apple device
through Music Assistant, put the box in a Music Assistant speaker group and
pick the group as the AirPlay target — even for a single box on its own. The
box by itself does not work as an AirPlay target.

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
| Fast-forward / rewind | Tilt the box to one side | 🟡 | The box fires `esphome.tonie_tilt` with `side: pos`/`neg` while a tilt is held, repeating every 600 ms so Home Assistant can seek in steps. The device half is verified on hardware; the seek automation in the [automations guide](docs/automations.md) is **written but not yet run**, so this stays partial until it has been. Left is `pos`, right is `neg`, matching tap. The tilt signal is `Y + Z`, not either axis alone: the part sits rotated ~45°, so the two move together. Measured +0.84 leaning left, −0.73 leaning right, against ~0 upright |
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
| Sleep timer with light | ✅ | A `Bedtime` switch and a `Bedtime Timer` number. Runs alongside whatever is already playing — as the stock timer does — holds the LED warm amber, fades it over the final minute, then stops and sleeps. **Confirmed on hardware:** the fade is honoured (it does not snap off) and the box sleeps on schedule. Note the fade leaves the visible band about nine seconds in: below ~60% this LED is not visible through the shell |
| Sunrise alarm | 🟡 | **No firmware needed** — the LED is an ordinary HA light, so the alarm is one automation, written up in the [automations guide](docs/automations.md). The box has to be awake when it fires: leave it on its charger with `Stay Awake on Charger` on, set `Sleep Timeout` to 0, or use the `Wake Alarm` below to have a genuinely sleeping box bring itself up first |
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
| Test tone | ✅ | A button that proves I2S → DAC → amp → speaker with no network involved — the fastest way to tell a wiring or codec fault from a Music Assistant one. Left out of `TonieESP.yaml` on purpose, since it is bring-up equipment rather than an everyday control: add the blocks under [Test tone](#test-tone) for a first flash on new hardware |
| Headphone detect | 🟡 | Exposed as a binary sensor on GPIO48. Nothing acts on it and the polarity has not been verified |
| Timer wake from deep sleep | ✅ | `box_power_down` arms `esp_sleep_enable_timer_wakeup` alongside the `ext1` mask, so a sleeping box can bring itself up. Two ways to set it, both dashboard entities and both off by default: a `Wake Interval` in minutes, and a `Wake Alarm` + `Wake Alarm Time` that wakes two minutes before a wall-clock time. **Confirmed on hardware, both ways:** each armed and self-woke a battery-powered box from deep sleep, with `Last Wake Cause` reading `wake timer` on the way back up. The one open question is drift: the wake timer runs off the internal RC oscillator, so a long alarm may land minutes late — measured only over short hops so far. `Last Wake Cause` against Home Assistant's own timestamp is the diagnostic for it |
| TTS announcements over music | ⛔ | Unlikely to fit at 44.1 kHz on this hardware — there is no PSRAM, so every buffer is internal SRAM |

## 3D-printed parts

Models of Toniebox shell parts are in [`docs/3d/`](docs/3d/):

| File | Size (mm) | What it is |
|------|-----------|------------|
| `Toniebox Top Plate.3mf` | 94 × 94 × 12.5 | Bambu Studio project for the top plate, with multicolour set up to match the stock plates. Two build plates: the standard top plate, and one with room for the metal plate |
| `Top Plate Single Color.stl` | 94 × 94 × 12.5 | The top plate for a single-colour print |
| `Top Plate Single Color with Light Cutouts.stl` | 94 × 94 × 12.5 | As above, with light cutouts |
| `Base Plate.stl` | 80.5 × 80.5 × 18.3 | The cap that closes the bottom of the box |

Printed in PETG on a Bambu Lab X1 Carbon with a 0.4 mm nozzle, using Bambu's
"0.20mm Strength" profile: 0.2 mm layers, 6 walls, 25 % infill, tree supports.
The 3MF carries the full settings.

The models are licensed [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/),
not GPLv3 like the rest of the repository.

## Things that will bite you

Most of the example config's odd-looking choices exist because something failed
in a way that took a while to explain. [How the example config
works](#how-the-example-config-works) covers all of them. The worst ones:

- **There is no PSRAM.** Every audio buffer has to fit in internal SRAM, and
  44.1 kHz playback only just does. Raising any `buffer_duration` or
  `buffer_size` tends to fail at runtime, not at compile time.
- **Keep the `network:` block and the WiFi `sdkconfig_options`.** ESPHome's
  media player switches on "high performance networking", which sizes the TCP
  window and WiFi buffers for a board with PSRAM. On this board that leaves
  too little memory to start a track, and playback fails with
  `i2s_alloc_dma_desc: allocate DMA buffer failed`, often on a track change.
  `network: enable_high_performance: false` only undoes the TCP half, so the
  example also sets the WiFi buffers back to the ESP-IDF defaults. Together
  they raise the largest free block during playback from around 10 KB to
  46 KB or more. Thanks to [vitals5](https://github.com/vitals5) for the report
  in [#1](https://github.com/chunckus-aurelius/TonieBox-ESPhome/issues/1).
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
- **Quiet the codec before resetting it.** `box_power_down` mutes the DAC and
  speaker driver and powers down the class-D amp before the software reset. The
  fix is for a pop heard at shutdown after audio had played. A test build gave
  no pop in 4 shutdowns, but the pop was heard only once and the unfixed build
  was never re-run, so treat it as likely fixed rather than proven.

## How the example config works

`TonieESP.yaml` contains no comments, so its reasoning lives here. It is a
complete config that matches what runs on the author's boxes, apart from the box
name. Pins come from the toniebox-reverse-engineering project's `teddybox`
`board_def.h` for rev v1.6.C.

It deliberately does not decide what a tag plays. The box reports the UID and
Home Assistant decides; see the [automations guide](docs/automations.md). It also
talks to no cloud: there is no Boxine client and nothing phones home.

### Pin map

| GPIO | Use |
|---|---|
| 1 | SPI chip select, TRF7962A |
| 2 / 3 / 4 | SPI MOSI / MISO / CLK. MISO is fixed by board wiring with no pull resistors |
| 5 / 6 | I2C SDA / SCL, 400 kHz |
| 7 | Wake line, an OR of small ear and charger-present |
| 8 | Charge Sense ADC, ×2.0 (R75/R72, 100k/100k) |
| 9 | Battery Voltage ADC, ×4.03 (R57/R58, (100k+33k)/33k) |
| 10 | I2S data |
| 11 / 12 | I2S BCLK / LRCLK |
| 13 | TRF7962A IRQ |
| 17 / 18 / 19 | LED blue / green / red |
| 20 / 21 | Big ear / small ear, physical push buttons, active low |
| 26 | DAC3100 hardware reset |
| 45 | Peripheral rail, high = on |
| 47 | SD power, active low |
| 48 | Headphone detect, exposed only |

The divider ratios come from the toniebox-pcb pinout and were not measured on
every board. The pack is 3-cell NiMH, so about 4.5 V while charging is normal.

### Boot

The first `on_boot` entry runs at priority 1100, ahead of the I2C bus at 1000. In
one lambda it asserts at compile time that the WiFi buffer overrides still apply,
releases the deep sleep pin holds on GPIO45, 47, 7, 20 and 21, raises the rails,
pulses the DAC reset on GPIO26 with blocking delays, and waits 250 ms for the
rails and codec to settle.

Left latched, the rail holds keep the rails from coming back and the wake pin
holds make the ears read stuck.

The -100 entry runs `deep_sleep.prevent`, which is **not** the interlock: a manual
sleep skips that check, so it only blocks the non-manual re-entry from `loop()`.
It also lights the LED at the earliest point the light exists, so the box shows
awake during boot, and publishes `Last Wake Cause` once. Only a power-on, OTA or
restart gesture reads as a cold boot.

### Build, console and logs

- There is no `psram:` block because PSRAM is absent.
- The S3 console defaults to USB_SERIAL_JTAG, unreachable here because GPIO19 and
  20 are the red LED and big ear. `hardware_uart: UART0` moves only ESPHome's app
  logger, so the two `CONFIG_ESP_CONSOLE_*` options move the IDF console too.
  Without them the serial output stops at `entry 0x...`.
- `external_components` is cached for a day by default. Add `refresh: 0s` to
  re-pull on every build.
- The logger keeps `sensor` at WARN (about 5 states per second otherwise; raise to
  INFO for live axis values), and `script` and `preferences` at INFO to hide their
  heartbeat. `trf7962a_nfc` at INFO keeps `Tag detected` and `Tag removed`; DEBUG
  adds collisions and VERBOSE adds each interrupt and timeout.
- `ota: encryption:` reuses the `api:` key.

### NFC and audio

`on_tag`'s `x` is `E0-04-...`, most significant byte first. The dashes are stripped
to keep the IDs the old driver sent. There are no tag tones: a tone fights whatever
Home Assistant is about to start playing.

The audio path is I2S speaker ← mixer ← one resampler per source ← media player.
There is no PSRAM, so 44.1 kHz only just fits and 150 ms of I2S buffer does not.
The MP3 decoder costs about 116 KB on top of the 32768-byte buffer.

`format:` is advertised to Home Assistant, Music Assistant encodes to match, and it
decides which decoder is compiled in. Music Assistant reads that from stored
config-entry diagnostics at player setup, not live, so after a format change
restart it or the device fails with `ESP_ERR_NOT_SUPPORTED`.

The media player's name is just `Speaker` because ESPHome prefixes `friendly_name`.

### Test tone

Proves I2S → DAC → amp → speaker with no network, Home Assistant or decoder
involved, the fastest way to tell a wiring or codec fault from a Music Assistant
one. Add this to `TonieESP.yaml` for a first flash on new hardware, then remove it:

```yaml
rtttl:
  id: toniebox_rtttl
  speaker: toniebox_i2s_speaker

button:
  - platform: template
    name: "Play Test Tone"
    id: play_test_tone
    on_press:
      - rtttl.play: "two_short:d=4,o=5,b=100:16e6,16e6"
```

### Accelerometer and gestures

Orientation is measured, not derivable from a datasheet, so confirm it on your own
board. X is vertical, about -0.97 G at rest and +0.97 G upside down. The part sits
about 45° rotated around X, so `Y + Z` is the tilt signal: +0.84 leaning left and
-0.73 leaning right against about 0 upright. Left is `pos`, right is `neg`, for both
tap and tilt. The guide maps `neg` to forward.

`update_interval: 200ms` is the I2C read cadence, not the ODR. `delta: 0.05` stops
Z noise publishing nearly every sample, but it also gates `.state`, so each axis
the gestures read can be up to 0.05 stale.

**Tap.** Timing counts ODR periods, 2.5 ms at 400 Hz. `on_click` refuses with no
reading yet, when X > -0.80 (tipped or upside down), and when |Y+Z| > 0.25
(mid-tilt), then debounces for 400 ms.

**Tilt.** The box only reports; Home Assistant seeks. A 200 ms interval needs X ≤
-0.30, which rejects the flip for the restart gesture (it swings Y+Z past -1.2).
|Y+Z| under 0.25 re-arms and 0.40 enters, so a box near one threshold cannot
chatter. A tilt counts only after the box was seen upright, so a box stored tilted
never seeks. It repeats every 600 ms and disarms after about 30 s.

`homeassistant.event` and light actions cannot run from a lambda, so `tilt_fire` and
`restart_fire` carry each decision to the action after it.

**Restart.** Upside down plus both ears for about 10 s. The threshold is X ≥ 0.70,
because a box held inverted in the hand reads 0.79 to 1.26. The blue blink uses
`transition_length: 0s` to beat the light's 1 s default, and a separate bare `effect:
"None"` first, because ESPHome rejects both on one action.

**Events** use `data:`, not `variables:`; see the
[automations guide](docs/automations.md).

### LED

Priority, highest first: Home Assistant (`Status LED` off), Bedtime amber, charging
(red to green, breathing green when full), low-battery orange under 20%, then green.
A missing reading never paints as empty or flat. Everything paints at 70% because
under about 60% the LED is not visible through the shell.

### Entities and restore rules

A wake from deep sleep is a boot, so restore modes matter.

| Entity | Why |
|---|---|
| `Power`, `ALWAYS_ON` | A persisted OFF fires `turn_off_action` from `setup()`, before WiFi: the box slept half a second into boot and never got an IP |
| `Bedtime`, `ALWAYS_OFF` | A persisted ON would start a bedtime on every boot |
| `Status LED`, `Stay Awake on Charger`, `Wake Alarm` | No restore trap: the first has safe actions, the others have none and are only read |
| `sleep_armed`, `restore_value: false` | **Must stay false.** A persisted true lets the box sleep before Home Assistant is reachable |
| Tilt, restart, Bedtime and charge-session globals | Not restored, since they describe a moment that is over |
| `last_battery_pct`, restored | Without it a box living on its charger reads unknown forever. -1 = never measured |

`Sleep Timeout` steps by 1 so a 1-minute timeout is selectable. `Tap Threshold` and
`Tap Time Limit` write the chip live; copy the values into `click:` once they feel
right.

Ear presses of 50 to 800 ms set the volume. A 1 to 12 s hold operates `Power`: the big
ear turns it on and the small ear turns it off. Both are refused while upside down
(X < 0.85), so an aborted restart gesture cannot release into a power toggle. A dead
accelerometer passes that check.

### Bedtime

Bedtime runs alongside whatever is playing. It fades over the last minute only,
since a linear fade would look switched off at minute four, and waits 65 s before
powering down. Its switch is turned off after `box_power_down` because two paths
return instead of sleeping: an ear held and Home Assistant never connected.

### Sleep, wake and power down

**The interlock.** `box_power_down` does nothing until `sleep_armed` is true, and only
`api: on_client_connected` sets it. Every boot therefore has an OTA window whatever got
persisted. Do not weaken it.

A 10 s interval re-arms the idle timer while docked with `Stay Awake on Charger` on,
during Bedtime, and during playback.

`box_power_down`, in order:

1. Refuses if an ear is held, above the teardown so a deferral cannot strand a box
   with its rails cut.
2. Stops the player and turns off the LED.
3. Quiets and resets the codec, before the rail cut because GPIO45 powers it:

   | Step | Page | Register | Value | Purpose |
   |---|---|---|---|---|
   | 1 | 0 | `0x40` | `0x0C` | Mute the DAC digitally, then wait 20 ms |
   | 2 | 1 | `0x2A` | `0x00` | Mute the speaker driver |
   | 3 | 1 | `0x20` | `0x06` | Power down the class-D amp, then wait 20 ms |
   | 4 | 0 | `0x01` | `0x01` | Software reset |

   These writes are for `output: SPEAKER`.
4. Cuts both rails and holds the pads, which are outside the S3 RTC domain.
5. Builds the ext1 wake mask at runtime. `deep_sleep:` declares no wake source because
   the mask depends on whether the box is docked: an already-low GPIO7 in an `ANY_LOW`
   mask is an instant wake. GPIO7 is read directly and configured first, since read
   cold it returns 0 and looks docked. The wake pads get pullups through sleep using
   `driver/gpio.h`, because `driver/rtc_io.h` is not in ESPHome's include chain.
6. Arms the timer wake on top of the ext1 mask.
7. Enters deep sleep.

**Never add `sleep_duration:` to `deep_sleep:`.** ESPHome only calls
`esp_sleep_enable_timer_wakeup` when that key exists and never disables a wake source,
which is why the hand-armed timer survives. The soonest of the two sources wins, and
both default to off. The alarm is computed from Home Assistant's clock when the box
sleeps, since there is no RTC, and fires 120 s early with a 60 s minimum. `Sleep Timeout`
must exceed that lead. With no valid time the box sleeps unarmed rather than staying
awake on battery.

### Battery and charging

- `samples: 16` because the divider is about 25k Thevenin, above Espressif's 10k ADC
  guidance. It cuts jitter with no lag.
- `battery_voltage`'s `on_value` drives `charge_progress` then `battery_level`, so
  a wake cannot read NaN and show `Unknown`.
- `charge_progress` maps the 4.35 to 4.65 V constant-current band. Full is detected
  as a 20 mV drop below the session peak for three reads, above 4.50 V. This is
  textbook NiMH, and it has not been confirmed through a full charge on this hardware.
- `battery_level` is a rough estimate, since voltage is a poor NiMH proxy and sags
  under audio. It was checked at 4.04 V/100% and 3.96 V/93%. On the charger it reports
  the higher of the last off-charger reading and charge progress, so it never goes
  backwards.

## Contributing

Issues and pull requests are welcome, whether that's a bug report, a fix, or a
note on what worked or failed on your board revision. See
[CONTRIBUTING.md](CONTRIBUTING.md) for what to include.

## Attribution

These are fresh ESPHome-native implementations, but the hardware knowledge
behind them is not original work. Pin assignments, register maps and init
sequences were derived from the
[toniebox-reverse-engineering](https://github.com/toniebox-reverse-engineering)
project — chiefly `teddybox` (`board_def.h` and its `trf7962a`, `dac3100` and
`lis3dh` components), which in turn credits `RvX_TLV320DAC3100` for the
PLL/NDAC/MDAC clock values and an earlier `electricimp/trf7962a` for the
TRF7962A register map. The deprecated `dac3100` and `trf7962a` follow those
sources; `tlv320dac3100` was written from TI's TLV320DAC3100 datasheet
(SLAS671C), and `trf7962a_nfc` from TI's TRF7962A datasheet and application
note SLOA140B.

TRF7962A quirk handling is cited inline to TI application note `sloa140b.pdf`
by section. Per-file attribution is in the header comment of each component.

## License

[GPLv3](LICENSE), except the 3D models in `docs/3d/`, which are [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/).

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
