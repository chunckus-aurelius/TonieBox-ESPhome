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

Tag removal is debounced in the firmware (3 s by default, `removal_debounce` on
the `trf7962a` component). A figure knocked askew and put straight back does not
trigger this.

---

## Tap gestures

Boxes with `click:` configured on the `lis3dh` component fire an
`esphome.tonie_tap` event carrying which axis was struck and on which side:

```yaml
alias: Tonie — tap to skip
mode: parallel
max: 10
triggers:
  - trigger: event
    event_type: esphome.tonie_tap
conditions: []
actions:
  - variables:
      box: >
        {{ device_entities(trigger.event.data.device_id)
           | select('match', 'media_player\.') | first }}
  - choose:
      - conditions:
          - condition: template
            value_template: "{{ trigger.event.data.side == 'pos' }}"
        sequence:
          - action: media_player.media_next_track
            target:
              entity_id: "{{ box }}_2"
    default:
      - action: media_player.media_previous_track
        target:
          entity_id: "{{ box }}_2"
```

Tap detection needs tuning per box before this is usable — see the `Tap
Threshold` control the config exposes, and tune it while watching the ESPHome
log rather than by reflashing.

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
