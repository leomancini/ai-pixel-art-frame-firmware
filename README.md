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

## Frame identity

The server is multi-frame: every board reports a unique `FRAME_SLUG` and
authenticates with a per-board `FRAME_KEY` (sent as an `X-Frame-Key` header).
Register each frame in the web app's **Admin** tab to mint its slug + one-time
key, then put them in that board's `secrets.h`. Requests become
`GET /animation?frame=<slug>` and `GET /poll?frame=<slug>&id=N`.

## Build & flash

```sh
cp matrix_stream/secrets.example.h matrix_stream/secrets.h  # then edit
arduino-cli compile --fqbn adafruit:samd:adafruit_matrixportal_m4 matrix_stream
arduino-cli upload --fqbn adafruit:samd:adafruit_matrixportal_m4 -p /dev/cu.usbmodem* matrix_stream
```

Requires the `adafruit:samd` core, plus the **Adafruit Protomatter** and
**WiFiNINA** libraries.

### Flashing multiple boards

`secrets.h` is gitignored and holds one board's identity. Keep a copy per
board (e.g. `secrets.living-room.h`, `secrets.studio.h`) differing only in
`FRAME_SLUG`/`FRAME_KEY`, then before flashing each device:

```sh
cp matrix_stream/secrets.living-room.h matrix_stream/secrets.h
arduino-cli compile --fqbn adafruit:samd:adafruit_matrixportal_m4 matrix_stream
arduino-cli upload  --fqbn adafruit:samd:adafruit_matrixportal_m4 -p /dev/cu.usbmodem* matrix_stream
```

Plug in one board at a time so `/dev/cu.usbmodem*` is unambiguous.

For the full step-by-step — creating the frame on the server, flashing, and
updating NINA when needed — see [docs/PROVISIONING.md](docs/PROVISIONING.md).

## tools/SerialNINAPassthrough

USB↔ESP32 serial bridge used to update the AirLift's NINA firmware (boards ship
with NINA 1.2.2, whose CA bundle predates Let's Encrypt's ISRG Root X1, so the
TLS handshake to the HTTPS server fails — the board connects to WiFi but loops
on `Server: connecting`). The 1.7.7 image is vendored at
[`tools/NINA_W102-1.7.7.bin`](tools/NINA_W102-1.7.7.bin) (from the
[adafruit/nina-fw](https://github.com/adafruit/nina-fw/releases/tag/1.7.7)
release — *not* `arduino/nina-fw`). Flash the bridge, then write it:

```sh
arduino-cli compile --fqbn adafruit:samd:adafruit_matrixportal_m4 tools/SerialNINAPassthrough
arduino-cli upload  --fqbn adafruit:samd:adafruit_matrixportal_m4 -p /dev/cu.usbmodem* tools/SerialNINAPassthrough

python3 -m esptool --port /dev/cu.usbmodem* --baud 115200 --before no_reset \
        --after no_reset write_flash 0x0 tools/NINA_W102-1.7.7.bin
```

Then re-flash `matrix_stream` (see PROVISIONING.md step 5). The bridge forces the
ESP32 into download mode at boot, which is why esptool uses `no_reset`. The CLI
is `python3 -m esptool`, not `esptool.py`.
