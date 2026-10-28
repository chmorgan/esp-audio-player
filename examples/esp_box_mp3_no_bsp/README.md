# ESP-BOX MP3 playback without BSP

This headless example continuously loops an embedded MP3 through the original
**ESP32-S3-BOX** speaker. It initializes the board's ES8311 codec, speaker
amplifier, and audio buses directly using registry codec drivers and native
ESP-IDF drivers.

## Requirements

- An original ESP32-S3-BOX; ESP32-S3-BOX-3 and ESP32-S3-BOX-Lite are not supported
- ESP-IDF 6.x (`>=6.0,<7.0`) with the ESP-IDF Component Manager
- The `esp32s3` target
- A USB cable capable of data transfer

The example builds with ESP-IDF 6.1.0 for ESP32-S3, verified by running the
`esp-idf-6-build` workflow job locally with `act`. Speaker playback on physical
hardware remains unverified.

## Dependencies

The Component Manager fetches
[`espressif/esp_codec_dev` 1.6.2](https://components.espressif.com/components/espressif/esp_codec_dev/versions/1.6.2).
That component supplies the ES8311 driver, codec bus adapters, and volume,
mute, and amplifier control. The example creates its I2C master bus and I2S
transmit channel using the drivers included with ESP-IDF.

The example manifest uses this repository as the `chmorgan/esp-audio-player`
component through `override_path`. Its `esp-libhelix-mp3` dependency is resolved
automatically. No symlinks or manual component copying are needed.

The new example has no `espressif/esp-box` BSP dependency and does not copy a
BSP implementation. It does not initialize the display, LVGL, touch input,
buttons, sensors, microphone, USB audio, networking, or filesystem storage.
The existing `../esp_box_mp3` example remains separate.

## Board audio setup

`main/box_audio.c` creates the audio hardware once at startup. Its resources
remain allocated for the application's lifetime. The configuration uses
the original ESP32-S3-BOX wiring:

| Signal | ESP32-S3 GPIO |
| --- | ---: |
| I2C SDA | 8 |
| I2C SCL | 18 |
| I2S MCLK | 2 |
| I2S BCLK | 17 |
| I2S WS | 47 |
| I2S DOUT to ES8311 | 15 |
| Speaker amplifier enable, active high | 46 |

The ES8311 uses clocks supplied by ESP32-S3. I2S is configured for playback
only, initially at 44.1 kHz with 16-bit stereo samples. The player's clock
callback updates the codec format to match decoded audio.

`sdkconfig.defaults` selects 16 MB QIO flash and the USB Serial/JTAG console.
It enables MP3 playback and ES8311 support and disables WAV playback, HTTP
streaming, and legacy I2C compatibility. The codec uses the modern I2C master
bus API.

Speaker volume is set to 60% through `BOX_AUDIO_VOLUME_PERCENT` in
`main/box_audio.h`. The playback callback restores this level when unmuting.

## Build and playback

Activate your ESP-IDF 6.x environment, then build from the repository root:

```sh
idf.py -C examples/esp_box_mp3_no_bsp -D IDF_TARGET=esp32s3 build
```

Flash the application to the original ESP32-S3-BOX and open its serial monitor,
replacing `PORT` with the board's serial port:

```sh
idf.py -C examples/esp_box_mp3_no_bsp -p PORT flash monitor
```

The `esp-idf-6-build` job in `.github/workflows/esp_box_mp3_build.yml` builds
this example with the `espressif/idf:v6.1` container image.

Playback starts automatically after boot and repeats until the board is
reset or powered off. The monitor reports the embedded MP3 size when the
loop starts. Check that audio remains audible across repeated loops without
playback errors or resets.

The resolved dependency graph should contain the codec driver, local audio
player, its MP3 decoder, and ESP-IDF components, with no ESP-BOX BSP or BSP UI
dependency chain.

## Embedded sample

The recording and its attribution are copied from the existing ESP-BOX MP3
example. The sample is compiled into the firmware, so an SD card or filesystem
image is not needed. Licensing information is provided alongside the recording
in `main/sample.mp3.license`.
