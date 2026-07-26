---
name: flash-frame
description: This skill should be used when the user asks to "flash a frame", "flash firmware", "flash new firmware", "update firmware on a frame", "re-flash a board", or "provision a frame" — flashing the matrix_stream firmware onto an Adafruit MatrixPortal M4 pixel-art frame connected over USB. Interactively gathers which frame, WiFi name/password, and the frame's device key, then compiles, uploads, and verifies. Works on a fresh machine: installs the toolchain and assumes no saved local state.
---

# Flash a pixel-art frame

Flash the `matrix_stream` player firmware onto a MatrixPortal M4 board over USB.
Written to work on a **fresh machine**: install whatever toolchain is missing and
assume no `secrets.*.h` or other state is saved locally. The authoritative
runbook is `docs/PROVISIONING.md` in this repo — read it whenever a step here is
unclear or fails. Paths below are relative to the repo root.

Flash **one board at a time**: the port name (`/dev/cu.usbmodem*`) is tied to the
USB slot, not the board, so two boards are indistinguishable by port alone.

## 1. Preflight

Check each tool and install what's missing (macOS; `arduino-cli` lands in
`/opt/homebrew/bin`, which may not be on PATH in non-login shells):

```sh
arduino-cli version || brew install arduino-cli
arduino-cli config add board_manager.additional_urls \
  https://adafruit.github.io/arduino-board-index/package_adafruit_index.json
arduino-cli core update-index
arduino-cli core list | grep -q adafruit:samd || arduino-cli core install adafruit:samd
arduino-cli lib install "Adafruit Protomatter" "WiFiNINA"
python3 -m esptool version || python3 -m pip install esptool
python3 -c "import serial"  || python3 -m pip install pyserial
```

Then check for a connected board: `ls /dev/cu.usbmodem*`. If none appears, ask
the user to plug the board in with a **data** USB cable and re-check. Don't
proceed without exactly one port.

## 2. Ask which frame, WiFi credentials, and the device key

Gather inputs with AskUserQuestion (the user can pick "Other" to type free text):

1. **Which frame?** If the "AI Pixel Art Frame" MCP is connected, build the
   options from `list_frames` (show name + slug) plus a "New frame (register on
   server)" option. If that MCP is **not** available on this machine, just ask
   the user for the frame's slug as free text (e.g. `frame-001`).
2. **WiFi network name (SSID)** — typed via "Other". Note: the AirLift ESP32 is
   **2.4 GHz only** — a 5 GHz-only network won't work.
3. **WiFi password** — typed via "Other".
4. **Device key** — does the user have this frame's device key saved (password
   manager, notes)? If yes they paste it; if not, rotate it (step 3).

## 3. Frame identity (slug + device key)

The server stores only the SHA-256 of each device key — the plaintext is
unrecoverable, so if it isn't saved anywhere it must be rotated. Rotation
requires SSH access to the VPS (`ssh leo@root.noshado.ws`) from this machine.
Rotating invalidates the old key **immediately** — a board out in the world
still using it will stop authenticating until re-flashed — so only rotate when
about to flash that board.

```sh
ssh leo@root.noshado.ws
export NVM_DIR="$HOME/.nvm"; . "$NVM_DIR/nvm.sh"
cd /home/leo/full-stack-apps/ai-pixel-art-frame
node -e '
  const crypto = require("crypto");
  const db = require("better-sqlite3")("data.sqlite");
  const slug = "frame-001";                        // <-- set this
  const key = crypto.randomBytes(24).toString("hex");
  const hash = crypto.createHash("sha256").update(key).digest("hex");
  const info = db.prepare("UPDATE frames SET device_key_hash=? WHERE slug=?").run(hash, slug);
  console.log(info.changes ? "ROTATED " + slug : "NO SUCH SLUG");
  console.log("DEVICE_KEY=" + key);                // shown once — save it
'
pm2 restart ai-pixel-art-frame --update-env && pm2 save
```

Suggest the user save the new key in their password manager so future re-flashes
don't need another rotation.

For a **new frame**, create it per `docs/PROVISIONING.md` §1 (web Admin tab, or
the headless node one-liner on the VPS) and capture the slug + one-time key.

## 4. Write secrets and verify server access

Create `matrix_stream/secrets.<slug>.h` from `matrix_stream/secrets.example.h`
with: `WIFI_SSID`, `WIFI_PASS`, `SERVER_TLS 1`,
`SERVER_HOST "ai-pixel-art-frame.leo.gd"`, `SERVER_PORT 443`, `FRAME_SLUG`,
`FRAME_KEY`. Then `cp` it to `matrix_stream/secrets.h` (the active build).
These files are gitignored — never commit them, and don't echo the key or
password back into chat beyond what's necessary.

Confirm the identity works **before** flashing:

```sh
curl -s -o /dev/null -w '%{http_code}\n' \
  -H 'X-Frame-Key: KEY' 'https://ai-pixel-art-frame.leo.gd/animation?frame=SLUG'
# 200 = good; 404 = unknown slug; 401/403 = wrong/missing key
```

## 5. Compile and upload

```sh
arduino-cli compile --fqbn adafruit:samd:adafruit_matrixportal_m4 matrix_stream
arduino-cli upload  --fqbn adafruit:samd:adafruit_matrixportal_m4 -p <detected port> matrix_stream
```

## 6. Verify over serial

Read the board's log at 115200 for ~30 s, pulsing DTR for a clean boot (use the
python snippet in `docs/PROVISIONING.md` §4 with the detected port). Healthy
boot:

```
WiFi: connected, ip=192.168.x.y
ESP32 NINA firmware: 1.7.x        <- 1.2.2 here means go to step 7
Anim: downloading
Anim: id=N frames=48 delay=70
```

If WiFi fails to connect, re-check SSID/password (and 2.4 GHz availability)
before suspecting the board.

## 7. NINA WiFi firmware update (only if serial shows 1.2.2)

Symptom: WiFi connects but the board loops `Server: connecting` — stock NINA
1.2.2's CA bundle predates Let's Encrypt's root, so TLS fails. Follow
`docs/PROVISIONING.md` §5 exactly: flash `tools/SerialNINAPassthrough`, write
the vendored `tools/NINA_W102-1.7.7.bin` with
`python3 -m esptool --before no_reset --after no_reset write_flash 0x0 ...`,
then re-copy the frame's secrets to `secrets.h`, re-flash `matrix_stream`
(step 5), and re-verify (step 6).

## 8. Confirm end-to-end

Push an animation to the frame (`show_animation` via the AI Pixel Art Frame MCP
if connected, or the web app) and confirm the panel updates within a second or
two — that proves the long-poll push path works. Ask the user to confirm what
the panel shows.
