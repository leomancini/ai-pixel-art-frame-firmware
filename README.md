# AI Pixel Art Frame — Firmware

Animation player for an Adafruit MatrixPortal M4 driving a 32×32 HUB75 LED
matrix. Downloads complete animations over HTTPS from the
[server](https://github.com/leomancini/ai-pixel-art-frame-server), plays
them from RAM, and keeps an HTTP long-poll outstanding so new art appears
within milliseconds of being pushed.

## Architecture

The ESP32 AirLift co-processor tops out around 25 KB/s over TLS — too slow
to stream frames live. Instead the board:

1. `GET /animation` — downloads the full animation (up to 64 frames ×
   2 KB RGB565 = 128 KB of the SAMD51's 192 KB RAM)
2. Plays the loop locally at full speed
3. Keeps `GET /poll?id=N` outstanding on the same keep-alive connection;
   the server holds it (~20 s max) until the animation changes

All loading states render as a dim 1 px ring around the panel edge —
a chasing segment while connecting, a clockwise fill while downloading —
driven from a TC3 timer ISR at 60 Hz so they animate even while the main
thread is blocked inside WiFiNINA calls. Animations fade in/out on every
transition.

## Build & flash

```sh
cp matrix_stream/secrets.example.h matrix_stream/secrets.h  # then edit
arduino-cli compile --fqbn adafruit:samd:adafruit_matrixportal_m4 matrix_stream
arduino-cli upload --fqbn adafruit:samd:adafruit_matrixportal_m4 -p /dev/cu.usbmodem* matrix_stream
```

Requires the `adafruit:samd` core, plus the **Adafruit Protomatter** and
**WiFiNINA** libraries.

## tools/SerialNINAPassthrough

USB↔ESP32 serial bridge used to update the AirLift's NINA firmware
(boards ship with NINA 1.2.2, whose CA bundle predates Let's Encrypt's
ISRG Root X1 — TLS fails until updated). Flash it, then:

```sh
esptool --port /dev/cu.usbmodem* --baud 115200 --before no_reset \
        --after no_reset write_flash 0x0 NINA_W102-1.7.7.bin
```
