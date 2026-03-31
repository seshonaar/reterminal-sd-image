# reterminal-sd-image

Custom ESPHome external components for the Seeed `reTerminal E1002` to:

- mount the microSD card over SPI
- enumerate `/images`
- pick a random 16-bit BMP at runtime
- load it in chunks outside the display callback
- commit exactly one finished frame for one e-paper refresh

## Status

This project now mounts the SD card over SPI, scans `/images` for BMP files, and loads one random image into PSRAM with Spectra 6 palette mapping plus dithering.

Planned milestones:

1. Integrate the component into the `joke.yaml` transaction flow.
2. Add a companion joke/text coordinator that waits for both image and content.
3. Trigger exactly one screen refresh after image commit plus joke commit.

## Hardware Notes

The reTerminal E1002 schematic shows these microSD SPI signals:

- `GPIO7`  - `SCK`
- `GPIO8`  - `MISO`
- `GPIO9`  - `MOSI`
- `GPIO14` - `SD_CS`
- `GPIO16` - `SD_EN`
- `GPIO15` - `SD_DET`

## Project Layout

- `components/spi_sd_storage/` - SD mount and file enumeration
- `components/random_sd_image/` - random BMP selection, staged load, palette mapping, and committed-frame draw pipeline
- `examples/reterminal_e1002_test.yaml` - integration example
- `examples/joke.yaml` - practical application that displays an image from the sdcard and overlays a joke on top

## Next Step

Use the transaction-style API from `random_sd_image`:

- `request_refresh()` starts a new random image load
- `has_pending_frame()` reports when the next frame is ready
- `commit_refresh()` swaps the finished frame into the committed slot
- `draw_committed(display)` draws only the committed frame

This is designed so one manual or scheduled trigger can lead to one final e-paper refresh.

## Example

Validate or compile the example from this project root:

```bash
esphome config examples/reterminal_e1002_test.yaml
esphome compile examples/reterminal_e1002_test.yaml
```

The example currently:

- powers and mounts the E1002 microSD over SPI
- scans `/images` for `.bmp`
- loads a random image in staged chunks
- commits the finished frame once
- draws only the committed frame on the display
