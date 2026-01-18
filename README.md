# Tanmatsu template app project

This template project shows how to build an app for Tanmatsu using the [PAX graphics](https://github.com/robotman2412/pax-graphics/tree/release/1.1.1/docs) library.

## Features

- **Graphics**: PAX graphics library for display rendering
- **Input**: Keyboard, navigation, action, and scancode event handling
- **Audio**: I2S audio output with ES8156 codec support
  - Tone generation
  - Volume control
  - Simple beep functionality
  - MOD file playback (MOD/XM/S3M/IT formats via libxmp-lite)
- **WiFi**: WiFi connectivity support
- **LED**: RGB LED control

## Audio

The template includes basic audio functionality via the ES8156 audio codec:
- Initialize audio system with `audio_init()`
- Play tones with `audio_play_tone(frequency, duration_ms, volume)`
- Play beeps with `audio_beep(duration_ms)`
- Control volume with `audio_set_volume(0.0-1.0)`

The audio implementation uses the BSP (Board Support Package) audio functions, which automatically handle all GPIO pin configuration. No manual pin setup is required.

## MOD File Playback

The template includes MOD file playback support using libxmp-lite. A test MOD file (space_debris.mod) is already embedded and ready to use.

**Usage:**
- Press **'M'** key to start/stop MOD playback
- The MOD player runs in a separate task and mixes with other audio

**Components:**
- libxmp component in `components/libxmp/` (configured for lite subset)
- Test MOD file in `main/test_mod.h`
- MOD player API in `main/mod_player.c/h`

See [MOD_PLAYBACK.md](MOD_PLAYBACK.md) for more details.

For more information visit the [documentation website](https://docs.tanmatsu.cloud).

## License

The contents of this repository may be considered in the public domain or [CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0) licensed at your disposal.

At Nicolai Electronics we love open source so we recommend licensing your work based on this template under terms of the [MIT license](https://opensource.org/license/mit). The MIT license allows others to build upon your work without restrictions while also making sure you retain your attribution.
