# Home Assistant automations

The firmware deliberately does not decide what a Tonie figure plays. The box
reports **what happened** — a tag arrived, a tag left, an ear was squeezed, the
box was tapped — and Home Assistant decides what that means. This keeps media
library changes out of the firmware and off the flash cycle.

All examples below use placeholder tag IDs (`E0040150XXXXXXXX`). Replace them
with your own: place a figure on the box and read the **NFC Tag UID** sensor, or
watch the ESPHome log for `tag_scanned`.

Entity IDs follow from the device name in your YAML. A box named
`tonie-the-example` gives `media_player.tonie_the_example_speaker`, and Music
Assistant usually registers its own player alongside it — often with a `_2`
suffix, because the names collide. Check yours before copying:

```jinja
{{ states.media_player | map(attribute='entity_id') | select('search','tonie') | list }}
```

---

## One automation for every box

The obvious approach is one automation per tag per box, which becomes
unmanageable quickly. Home Assistant's tag trigger includes the **device that
did the scanning**, so a single automation can serve every box you own and act
on whichever one the figure was placed on.

`device_entities(trigger.event.data.device_id)` is the whole trick.

```yaml
alias: Tonie — play a tag on whichever box scanned it
mode: parallel          # NOT single: two children scanning at once must both work
max: 10
triggers:
  - trigger: tag
    tag_id:
      - E0040150AAAAAAAA
      - E0040150BBBBBBBB
conditions: []
actions:
  - variables:
      # Tag -> media. Everything box-specific stays out of this map.
      tag_map:
        E0040150AAAAAAAA: "Some Playlist"
        E0040150BBBBBBBB: "Another Playlist"
      # The ESPHome media player belonging to the box that scanned.
      box: >
        {{ device_entities(trigger.event.data.device_id)
           | select('match', 'media_player\.') | first }}
  # Volume BEFORE playback, so a box that was left loud does not blast.
  - action: media_player.volume_set
    target:
      entity_id: "{{ box }}"
    data:
      volume_level: 0.4
  - delay:
      milliseconds: 300
  - action: music_assistant.play_media
    target:
      entity_id: "{{ box }}_2"
    data:
      media_id: "{{ tag_map[trigger.event.data.tag_id] }}"
      media_type: playlist
```

Notes on the parts that are easy to get wrong:

- **`mode: parallel`.** The default `single` silently drops a second scan while
  the first is still running. With more than one box in the house that is a
  real bug, not a theoretical one.
- **Volume first, then a short delay.** Without the pause the first frames can
  play at the previous volume. If you never hear that, remove the delay.
- **`{{ box }}_2`** assumes Music Assistant's player sits on a different device
  and got a suffix. Verify with the Jinja snippet at the top. If Music Assistant
  is not involved at all, drop that step and use `media_player.play_media`
  against `{{ box }}` directly.

---

## Pause when the figure is lifted

Removing a figure has no tag event — Home Assistant's tag trigger only fires on
arrival. The firmware instead sets the **NFC Tag UID** sensor to `none`, so
trigger on that transition and use `device_id()` to find the box:

```yaml
alias: Tonie — pause when the figure is lifted
mode: parallel
max: 10
triggers:
  - trigger: state
    entity_id:
      - sensor.tonie_the_example_nfc_tag_uid
    to: "none"
conditions: []
actions:
  - variables:
      box: >
        {{ device_entities(device_id(trigger.entity_id))
           | select('match', 'media_player\.') | first }}
  - action: media_player.media_pause
    target:
      entity_id: "{{ box }}_2"
```

`entity_id` still has to list each box's sensor, since a state trigger needs
concrete entities. Everything after that is dynamic. Adding a box means adding
one line here, not copying the automation.

Pair it with a resume automation keyed on the same sensor leaving `none`, and
you have stock's pause-on-lift behaviour.

Tag removal is debounced in the firmware (1.5 s by default, `removal_debounce`
on the `trf7962a` component). A figure knocked askew and put straight back does
not trigger this.

---

## Tap gestures

Unlike the tag and sensor examples above, this one needs a change on the box
first. `lis3dh` gives `on_click` the raw `CLICK_SRC` byte and nothing more — the
example config does not enable `click:` at all, and no firmware here fires a tap
event on its own. Send one explicitly:

```yaml
lis3dh:
  id: accelerometer
  click:
    threshold: 40         # tune per box, see below
    on_click:
      - homeassistant.event:
          event: esphome.tonie_tap
          # data:, NOT variables:. See "Why data: and not variables:" below —
          # getting this wrong produces an event that fires but carries nothing.
          data:
            # Which axis was struck. One physical tap trips two or three of
            # them, so if you act on the axis you must pick one.
            axis: !lambda |-
              if (src & 0x01) return "x";
              if (src & 0x02) return "y";
              return "z";
            # Bit 3 of CLICK_SRC is the sign bit: which side of the axis was
            # struck. That is what separates "tap the left" from "tap the right".
            side: !lambda 'return (src & 0x08) ? "neg" : "pos";'
```

### Why `data:` and not `variables:`

`homeassistant.event` takes three maps and only two of them reach Home
Assistant:

| Key | What it does |
|---|---|
| `data:` | Copied verbatim into the event data. Values are templatable, so a lambda is evaluated on the device. |
| `data_template:` | Rendered by Home Assistant as Jinja, with `variables` as the context, then merged into the event data. |
| `variables:` | **Render context for `data_template` and nothing else.** Never reaches the event on its own. |

Home Assistant's `esphome/manager.py` starts from `service_data = service.data`
and only consults `variables` while rendering `data_template`. An event sent
with `variables:` and no `data_template:` therefore arrives carrying `device_id`
and nothing else — and it arrives *successfully*, so the box logs a tap, the
event listener shows the event, and every template reading `trigger.event.data`
silently sees an undefined value. Either `data:` with lambdas (above) or
`variables:` paired with a matching `data_template:` works; `variables:` alone
never does.

### The automation

Home Assistant's ESPHome integration adds `device_id` to every `esphome.*`
event, so the same resolve-the-box trick works here:

```yaml
alias: Tonie — tap to skip
mode: parallel
max: 10
triggers:
  - trigger: event
    event_type: esphome.tonie_tap
conditions:
  # Side taps land on y. Without this, any knock hard enough to trip the click
  # detector on an upright box skips in whichever direction that axis's sign
  # bit happens to carry — setting a box down on a table was measured doing
  # exactly that. See "Things that are easy to get wrong" below.
  - condition: template
    value_template: "{{ trigger.event.data.axis == 'y' }}"
actions:
  - variables:
      box: >
        {{ device_entities(trigger.event.data.device_id)
           | select('match', 'media_player\.') | first }}
  - choose:
      # neg = right side = forward. Verified on hardware 2026-08-16; see
      # "Which side is which" below before swapping these.
      - conditions:
          - condition: template
            value_template: "{{ trigger.event.data.side == 'neg' }}"
        sequence:
          - action: media_player.media_next_track
            target:
              entity_id: "{{ box }}_2"
    default:
      - action: media_player.media_previous_track
        target:
          entity_id: "{{ box }}_2"
```

### Which side is which

**Left is `side: pos`. Right is `side: neg`.** Measured on hardware 2026-08-16
with the accelerometer mounted as it is in this board revision, so it should
hold for any box built the same way — but it costs one tap to confirm, and
getting it backwards is invisible until you wonder why skipping feels wrong.

With the mapping above, a right tap goes to the next track and a left tap
restarts the current one. That left behaviour is `media_previous_track`'s normal
two-stage semantics — first press restarts, second press goes back — and it
happens to be what a stock Toniebox does, so it is worth keeping rather than
working around.

### Things that are easy to get wrong

- **The threshold is per box.** Set it with `threshold:` under `click:` and
  watch the ESPHome log, which prints every `CLICK_SRC` it sees at DEBUG.
- **One physical tap usually trips two or three axes**, ~100–230 ms apart, so
  the box fires two or three events per tap. Debounce on the device rather than
  in Home Assistant — a `globals:` `uint32_t last_tap_ms` and a condition of
  `(millis() - id(last_tap_ms)) > 400` around the whole `on_click` body costs
  nothing and keeps the automation stateless.
- **Filter on `axis`.** With the upright gate below in place, a left/right tap
  lands on `y`, so add `trigger.event.data.axis == 'y'` as a condition. Without
  it, any knock hard enough to trip the click detector while the box is sitting
  upright fires a skip in whichever direction that axis's sign bit happens to
  carry. Observed on hardware: setting a box down on a table produced
  `CLICK_SRC=0x5C` — Z, negative — which passed the upright gate cleanly and
  would have skipped a track.
  There is a cost, and it is worth knowing. Only the *first* axis to trip is
  reported, because of the debounce above, so a side tap that happens to
  register on `x` first is dropped. That failure is quiet and specific: the
  device log prints `tap: CLICK_SRC=0x..` and nothing skips. In practice
  deliberate side taps land on `y` and stray knocks do not, so the filter wins
  — but if taps start feeling unreliable, this is the first thing to remove.

If you also use tilt gestures, note that a tap fires *during* a tilt
(`CLICK_SRC=0x59` was measured mid-gesture), so the two will trip each other.
Gate `on_click` on the box being upright — with the LIS3DH mounted as it is in
this hardware, that is X ≈ −0.97 G at rest and `Y + Z` ≈ 0, against `Y + Z` ≈
±1.1 while tilted.

---

## Sunrise alarm

A Toniebox 2 feature, and one that needs no firmware at all — the LED is an
ordinary Home Assistant light, so a schedule, a transition and a `play_media`
call are the whole thing.

**The catch is that the box has to be awake when the alarm fires.** Deep sleep
means no API, and a wake pin cannot be a clock, so an automation aimed at a
sleeping box does nothing and reports no error. Set `Sleep Timeout` to 0 on the
box that acts as the alarm clock and leave it on its charger overnight. Nothing
below works around this; it is the one thing to get right.

```yaml
alias: Tonie — sunrise alarm
mode: single
triggers:
  - trigger: time
    at: "07:00:00"
conditions:
  # Weekdays only. Drop this if you want it every day.
  - condition: time
    weekday: [mon, tue, wed, thu, fri]
  # A box that slept anyway cannot be woken from here, and painting its light
  # would fail silently. Better to skip the alarm than to half-run it.
  - condition: template
    value_template: "{{ states('light.tonie_the_example_led') != 'unavailable' }}"
actions:
  # Start dim, at the bottom of what is actually visible — see below.
  - action: light.turn_on
    target:
      entity_id: light.tonie_the_example_led
    data:
      brightness_pct: 60
      rgb_color: [255, 120, 20]     # warm amber
  # Ramp to full daylight over twenty minutes.
  - action: light.turn_on
    target:
      entity_id: light.tonie_the_example_led
    data:
      brightness_pct: 100
      rgb_color: [255, 200, 120]
      transition: 1200
  # Audio after the light has been climbing a while, not at the same instant.
  - delay:
      minutes: 15
  - action: media_player.volume_set
    target:
      entity_id: media_player.tonie_the_example_speaker
    data:
      volume_level: 0.2
  - action: music_assistant.play_media
    target:
      entity_id: media_player.tonie_the_example_speaker_2
    data:
      media_id: "Morning Playlist"
      media_type: playlist
```

**Start the ramp at 60%, not at 0.** Below roughly 60% brightness this LED is
not visible through the plastic shell — 25% measures as indistinguishable from
off. A ramp from zero is therefore invisible for most of its length: a
twenty-minute climb from 0 would show nothing at all until about minute twelve,
then appear to snap on. The usable range is the top of the scale, so ramp within
it.

**If your config paints the LED on the device**, as the sibling configs behind
this repo do, add a switch that suppresses that painting and turn it off at the
start of the automation, then back on at the end — otherwise a device-side
status colour will repaint over the sunrise within seconds. The example config
in this repo does no device-side painting, so nothing is needed there.

**Waking a box that has genuinely slept is a different problem** and this does
not solve it. That needs `esp_sleep_enable_timer_wakeup` in the firmware
alongside the existing wake mask, so the box brings itself up on a schedule
rather than being reached over the network.

---

## Things worth knowing

**A sleeping box is unreachable.** Deep sleep means no API, so any automation
targeting a box that has slept will silently do nothing. This does not affect
the automations above — they are all triggered *by* the box, which means it is
awake and connected. It does affect anything Home Assistant initiates on a
schedule.

**Volume is capped on the device.** The `Max Volume` number clamps every path,
so `volume_set` above the ceiling is pulled back down. Raise the ceiling on the
box rather than fighting it from Home Assistant.

**Tag IDs are 8 bytes, printed most-significant byte first.** The firmware
reverses the byte order it reads off the tag so the value matches what other
tools show for the same figure.
