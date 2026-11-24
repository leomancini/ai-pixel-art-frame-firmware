# TEMP: re-flashing the already-provisioned frames

Temporary note — delete once every frame is on the new firmware.

## What we did (2026-11-23)

Re-flashed **MUGUZO (`frame-005`)** with the firmware at commit `dc7d693`
("Draw playback from the timer ISR so network stalls can't freeze it").

- `matrix_stream/secrets.frame-005.h` was already saved on this Mac, and its
  device key still authenticated against the server (HTTP 200), so no key
  rotation was needed.
- Compile + upload over USB took under a minute.
- Serial log showed a healthy boot: WiFi connected, `ESP32 NINA firmware: 1.7.7`,
  `Anim: id=43 frames=48 delay=60`.
- Push test: switched the frame Plasma → Starfield → Plasma from the server.
  Each time the board logged `Poll: new animation N` and finished downloading
  about 3 s later; the panel visibly changed.

### `dc7d693` was broken — flash `1423280` or later

Shortly after, MUGUZO stopped responding to animation changes while still
playing its current one. Serial showed `Server: connecting`,
`No Socket available`, then `WiFi: connecting to ...` repeating forever.

Cause: `dc7d693` draws frames from a timer interrupt, and a draw landing in the
middle of an SPI transaction with the WiFi chip knocked the two out of sync for
good. It's intermittent — it survived over a minute in the first test, then
died 7 s after playback started in the next.

Fix: commit `1423280` skips the draw while the WiFi chip's chip select is low.
With it, MUGUZO ran 6+ minutes and took ~10 changes in a row without a miss.
That's the only soak it has had, so:

- **Never flash `dc7d693`.** Make sure the checkout is at `1423280` or later
  (`git pull`) before flashing the studio boards.
- After flashing each board, don't stop at one push test. Leave it a few
  minutes and change the animation several times.
- If a board plays fine but ignores changes, that's this bug again — grab the
  serial log before power-cycling it.

## Still on the old firmware

The other registered frames, none of which have been re-flashed:

| Slug        | Name       | Where         |
|-------------|------------|---------------|
| `frame-003` | FCC STUDIO | in the studio |
| `frame-004` | LET IT ME  | in the studio |
| `frame-001` | HBD MEHDI! | not in the studio |

The two studio boards are the ones to do next time we're there. They keep working on the old firmware in the meantime; this is an upgrade, not
a repair.

## How to do the studio boards

The quick way: open Claude Code in this repo and run `/flash-frame` — it walks
through everything below. Manual steps, per board, **one board plugged in at a
time**:

1. **Bring this Mac** (toolchain is installed: `arduino-cli`, `adafruit:samd`
   core, Protomatter, WiFiNINA, pyserial) and a **data** USB-C cable.

2. **Get the board's secrets file.** Only `secrets.frame-005.h` exists on this
   Mac. For each other frame we need `matrix_stream/secrets.<slug>.h` with:
   - `WIFI_SSID` / `WIFI_PASS` — the **studio** network (2.4 GHz; the board
     can't join 5 GHz-only). `secrets.frame-005.h` has the home network, so
     don't just copy it unchanged.
   - `FRAME_SLUG` — from the table above.
   - `FRAME_KEY` — the device key. If it wasn't saved anywhere, rotate it on
     the VPS (snippet in `.claude/skills/flash-frame/SKILL.md` §3). Rotation
     kills the old key immediately, so only rotate with the board in hand,
     and save the new key this time.

3. **Check the identity before flashing** — expect `200`:

   ```sh
   curl -s -o /dev/null -w '%{http_code}\n' \
     -H 'X-Frame-Key: KEY' 'https://ai-pixel-art-frame.leo.gd/animation?frame=SLUG'
   ```

4. **Flash:**

   ```sh
   cp matrix_stream/secrets.<slug>.h matrix_stream/secrets.h
   arduino-cli compile --fqbn adafruit:samd:adafruit_matrixportal_m4 matrix_stream
   arduino-cli upload  --fqbn adafruit:samd:adafruit_matrixportal_m4 -p /dev/cu.usbmodem* matrix_stream
   ```

5. **Verify over serial** (python snippet in `docs/PROVISIONING.md` §4). Want
   to see `WiFi: connected`, `ESP32 NINA firmware: 1.7.x`, then `Anim: id=...`.

6. **Push test:** change the frame's animation from the web app (or the MCP
   `show_animation`) and watch the panel switch within a few seconds. Repeat
   several times over a few minutes (see the `dc7d693` section above), then
   put it back to what it was showing.

## Gotchas

- `secrets.h` is whatever board was flashed last (currently `frame-005`).
  Always re-copy the right `secrets.<slug>.h` before compiling, or the board
  will come up as the wrong frame.
- `esptool` is **not** installed on this Mac. It's only needed if a board
  reports NINA `1.2.2` and loops on `Server: connecting`
  (`python3 -m pip install esptool`, then `docs/PROVISIONING.md` §5). These
  boards were already provisioned, so they should all be on 1.7.7.
- The secrets files are gitignored — they won't travel with a `git clone`, so
  flash from this Mac or copy them over by hand.
