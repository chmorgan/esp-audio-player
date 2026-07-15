# ESP-BOX MP3 playback example

This is a headless example for the original **ESP32-S3-BOX**. It initializes
the board's ES8311 audio codec and I2S output, then continuously loops an MP3
embedded in the application firmware.

The example does not initialize or use the display, LVGL, touch input,
buttons, sensors, microphone, USB audio, networking, or filesystem storage.

## Requirements

- An original ESP32-S3-BOX (not ESP32-S3-BOX-3 or ESP32-S3-BOX-Lite)
- ESP-IDF, tested with the repository's expected ESP-IDF 5.4 setup
- A USB cable capable of data transfer

The ESP-IDF Component Manager resolves the ESP-BOX board support package and
uses this repository as the `esp-audio-player` component. No symlinks or
manual component copying are required.

## Build

Export the ESP-IDF environment, change to the example directory, select the
ESP32-S3 target, and build:

```sh
source ~/esp/esp-idf/export.sh
cd examples/esp_box_mp3
idf.py set-target esp32s3
idf.py build
```

If ESP-IDF is installed somewhere else, source `export.sh` from that location
instead.

For CI, or to build without changing directories, run this from the repository
root after exporting the ESP-IDF environment:

```sh
idf.py -C examples/esp_box_mp3 -D IDF_TARGET=esp32s3 build
```

## Flash and monitor

From the example directory, replace `PORT` with the ESP-BOX serial port:

```sh
idf.py -p PORT flash monitor
```

The sample starts automatically after boot and repeats until the board is
reset or powered off. Press <kbd>Ctrl</kbd>+<kbd>]</kbd> to leave the serial
monitor.

## Embedded sample

The MP3 is compiled into the firmware, so an SD card or filesystem image is
not needed. Attribution and licensing information for the bundled recording
are provided alongside the sample in `main/sample.mp3.license`.
