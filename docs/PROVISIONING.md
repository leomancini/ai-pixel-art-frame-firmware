# Provisioning a new frame

End-to-end runbook for bringing a new MatrixPortal board online as a frame:
create the frame on the server, flash the player firmware, and — if the board
still has stock WiFi firmware — update its NINA co-processor so HTTPS works.

Written so a human or an agent can repeat it. Commands assume macOS with the
board connected over USB. **Flash one board at a time** — the port name
(`/dev/cu.usbmodem*`) is tied to the physical USB slot, not the board, so two
boards (or a swap into the same slot) are indistinguishable by port name alone.

## Prerequisites (one-time)

```sh
brew install arduino-cli                 # installs to /opt/homebrew/bin
arduino-cli config add board_manager.additional_urls \
  https://adafruit.github.io/arduino-board-index/package_adafruit_index.json
arduino-cli core update-index
arduino-cli core install adafruit:samd
arduino-cli lib install "Adafruit Protomatter" "WiFiNINA"
python3 -m esptool version               # esptool comes via pip; invoke as `python3 -m esptool`
python3 -c "import serial"               # pyserial, used to read the board's serial log
```

Note: the CLI tool is **`python3 -m esptool`**, not `esptool.py` (the latter is
usually not on PATH). `arduino-cli` may not be on PATH in non-login shells —
use `/opt/homebrew/bin/arduino-cli` or `export PATH="/opt/homebrew/bin:$PATH"`.

## 1. Create the frame on the server

Each frame has a stable `slug` (baked into firmware, used as `?frame=<slug>`)
and a secret device key (sent as the `X-Frame-Key` header; only the SHA-256 is
stored, so the plaintext is shown **once**).

**Preferred — web Admin tab:** sign in as the admin, open **Admin → Add a
frame**, give it a name (the slug is derived from the name), and copy the
one-time device key.

**Headless — no browser session** (mirrors `POST /api/admin/frames` exactly).
On the VPS (`ssh leo@root.noshado.ws`, app dir
`/home/leo/full-stack-apps/ai-pixel-art-frame`; pm2 lives under nvm):

```sh
export NVM_DIR="$HOME/.nvm"; . "$NVM_DIR/nvm.sh"
cd /home/leo/full-stack-apps/ai-pixel-art-frame
node -e '
  const crypto = require("crypto");
  const db = require("better-sqlite3")("data.sqlite");
  const sha256 = s => crypto.createHash("sha256").update(s).digest("hex");
  const name = "frame-003";                       // <-- set this
  let slug = name, base = name;                   // slug must be unique
  for (let n = 2; db.prepare("SELECT 1 FROM frames WHERE slug=?").get(slug); n++) slug = base + "-" + n;
  const key = crypto.randomBytes(24).toString("hex");
  const info = db.prepare(
    "INSERT INTO frames (slug,name,device_key_hash,active_kind,active_preset_key) VALUES (?,?,?,?,?)"
  ).run(slug, name, sha256(key), "preset", "plasma");   // "plasma" = DEFAULT_PRESET_KEY
  console.log("CREATED id=" + info.lastInsertRowid + " slug=" + slug);
  console.log("DEVICE_KEY=" + key);                // shown once — save it
'
pm2 restart ai-pixel-art-frame --update-env && pm2 save   # reload so the new frame runtime is registered
```

**Renaming a slug** (e.g. `default` → `frame-001`): there is no API for it — the
slug is intentionally immutable (`PATCH /api/admin/frames/:id` only changes the
display name). Update it directly and restart, then re-flash any board using the
old slug:

```sh
node -e 'require("better-sqlite3")("data.sqlite").prepare("UPDATE frames SET slug=?, name=? WHERE slug=?").run("frame-001","frame-001","default")'
pm2 restart ai-pixel-art-frame --update-env && pm2 save
```

**Verify the frame serves** (replace SLUG/KEY):

```sh
curl -s -o /dev/null -w '%{http_code}\n' \
  -H 'X-Frame-Key: KEY' 'https://ai-pixel-art-frame.leo.gd/animation?frame=SLUG'
# 200 = good; 404 = unknown slug; 401/403 = wrong/missing key
```

## 2. Put the frame identity in secrets

`secrets.h` and `secrets.*.h` are gitignored. Keep one file per board so you can
re-flash any of them later:

```sh
cd matrix_stream
cp secrets.example.h secrets.frame-003.h     # then edit:
#   WIFI_SSID / WIFI_PASS    — the board's network
#   SERVER_TLS 1, SERVER_HOST "ai-pixel-art-frame.leo.gd", SERVER_PORT 443
#   FRAME_SLUG "frame-003"   — the slug from step 1
#   FRAME_KEY  "<device key from step 1>"
cp secrets.frame-003.h secrets.h             # make it the active build
```

## 3. Flash the player firmware

```sh
cd ..                                        # firmware/ root
arduino-cli compile --fqbn adafruit:samd:adafruit_matrixportal_m4 matrix_stream
arduino-cli upload  --fqbn adafruit:samd:adafruit_matrixportal_m4 -p /dev/cu.usbmodem* matrix_stream
```

## 4. Verify over serial

The board logs at 115200. This reads ~30 s, pulsing DTR to force a clean boot:

```sh
python3 - <<'PY'
import serial, time
p = serial.Serial('/dev/cu.usbmodem201101', 115200, timeout=1)   # set your port
p.setDTR(False); time.sleep(0.2); p.setDTR(True)
end = time.time() + 30
while time.time() < end:
    line = p.readline().decode(errors='replace').rstrip()
    if line: print(line)
p.close()
PY
```

Healthy boot looks like:

```
WiFi: connected, ip=192.168.x.y
ESP32 NINA firmware: 1.7.x        <- 1.2.2 here means go to step 5
Anim: downloading
Anim: id=N frames=48 delay=70
```

`Poll: new animation N` followed by a download is the long-poll push working.
Identifying which board is on a port: reset it (DTR pulse) — a board running this
firmware prints the banner above; a silent board is running something else.

## 5. Update the NINA WiFi firmware (only if step 4 shows 1.2.2)

**Symptom:** serial shows `ESP32 NINA firmware: 1.2.2` and then loops
`Server: connecting` forever. Boards ship with NINA 1.2.2, whose CA bundle
predates Let's Encrypt's ISRG Root X1, so the TLS handshake to the HTTPS server
fails. Updating to 1.7.7 fixes it. (WiFi connects fine on 1.2.2 — only TLS fails.)

The firmware image is **vendored** at `tools/NINA_W102-1.7.7.bin`
(source: <https://github.com/adafruit/nina-fw/releases/tag/1.7.7> — the Adafruit
fork, *not* `arduino/nina-fw`; sha256
`0a9857de7adb6389bafe56e651aea6c35387bdd45e08d084d55d80a7ce4469f5`).

```sh
# 5a. Flash the USB<->ESP32 bridge. It forces the ESP32 into download mode at
#     boot, which is why esptool uses --before/--after no_reset.
arduino-cli compile --fqbn adafruit:samd:adafruit_matrixportal_m4 tools/SerialNINAPassthrough
arduino-cli upload  --fqbn adafruit:samd:adafruit_matrixportal_m4 -p /dev/cu.usbmodem* tools/SerialNINAPassthrough

# 5b. (optional) confirm esptool can talk to the ESP32
python3 -m esptool --port /dev/cu.usbmodem201101 --baud 115200 \
  --before no_reset --after no_reset flash_id

# 5c. Write the firmware to 0x0 (~1 min; ends with "Hash of data verified.")
python3 -m esptool --port /dev/cu.usbmodem201101 --baud 115200 \
  --before no_reset --after no_reset write_flash 0x0 tools/NINA_W102-1.7.7.bin

# 5d. Put the player back on, then re-verify (step 4)
cp matrix_stream/secrets.frame-003.h matrix_stream/secrets.h   # if it got swapped
arduino-cli compile --fqbn adafruit:samd:adafruit_matrixportal_m4 matrix_stream
arduino-cli upload  --fqbn adafruit:samd:adafruit_matrixportal_m4 -p /dev/cu.usbmodem* matrix_stream
```

After 5d, step 4 should show `ESP32 NINA firmware: 1.7.7` and successful
`Anim: downloading` / `Anim: id=...` lines.

## Server operations reference

See the server repo's deploy notes for how the app is hosted (Apache → Node on
:3136, PM2 under nvm, `git push origin main` → GitHub Action → VPS post-receive
hook). The frame's `device_key_hash`, active selection, and gallery all live in
`data.sqlite` on the VPS.
