# Contributing

Issues and pull requests are welcome. This is a small hobby project maintained
in spare time, so replies may take a few days.

## Reporting a problem

Include:

- **Board revision**, from the silkscreen (e.g. `TONIEBOX-ESP32 1.6.C`).
- **ESPHome version** the firmware was built with.
- **Music Assistant version**, for anything involving playback.
- **The log**, from boot through the failure, as text rather than a
  screenshot.
- **What you changed** from `TonieESP.yaml`, if anything.

For audio failures (`Not enough memory`, `allocate DMA buffer failed`,
`Speaker task setup failed`), first check the
[mono setting](README.md#things-that-will-bite-you). If it still fails, add a
temporary `debug:` component and include the heap readings. The largest free
block is the number that matters, not total free heap.

## Pull requests

- **Keep it small.** One change per PR is easier to review and to test on
  hardware.
- **Say how it was tested.** Which board revision, and what you saw on the
  box. "Compiles" and "works on my box" are both useful, but they are
  different claims, so say which one applies.
- **Keep `TonieESP.yaml` in step with the README.** If a feature is documented,
  the example config should ship it.
- **Respect the memory budget.** There is no PSRAM. A change that adds a
  buffer or a component should note its heap cost.
- **Prefer configuration over vendored code.** Copying ESPHome's own
  components into this repo pins everyone to one ESPHome version. If a fix
  belongs upstream, an ESPHome PR plus a link here is usually better.

Larger changes, such as new hardware support or a new component, are worth
an issue first, so we can agree on the approach before you spend the time.

## License

By submitting a pull request you agree that your contribution is licensed
under [GPLv3](LICENSE), or [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/)
for 3D models in `docs/3d/`, matching the rest of the repository.
