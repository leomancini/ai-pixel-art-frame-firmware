// MatrixPortal M4 animation player.
//
// Downloads a complete animation from the server into RAM (up to 64 frames),
// plays it in a loop at full speed, and keeps an HTTP long-poll outstanding
// on the same keep-alive connection so a changed animation is picked up
// within milliseconds. This sidesteps the ESP32's ~25KB/s TLS throughput
// limit: downloads may take a few seconds, but playback is local.
//
// Server endpoints (all responses use Content-Length, never chunked):
//   GET /animation     -> ANM0, uint32 id, uint16 frameCount, uint16 delayMs,
//                         then frameCount * 2048 bytes RGB565 LE pixels
//   GET /poll?id=N     -> held until animation id != N (or ~20s), then "ID <n>\n"

#include <Adafruit_Protomatter.h>
#include <SPI.h>
#include <WiFiNINA.h>
#include "secrets.h"

#define WIDTH 32
#define HEIGHT 32
#define FRAME_PIXELS (WIDTH * HEIGHT)
#define MAX_FRAMES 64
#define POLL_TIMEOUT_MS 30000 // server holds ~20s; past 30s assume dead

// MatrixPortal M4 fixed wiring to the HUB75 connector
uint8_t rgbPins[]  = {7, 8, 9, 10, 11, 12};
uint8_t addrPins[] = {17, 18, 19, 20};
uint8_t clockPin   = 14;
uint8_t latchPin   = 15;
uint8_t oePin      = 16;

// Double-buffered (last arg): we draw into a back buffer and show() swaps
// between refresh passes — without it, the panel rescans the buffer while
// it's being drawn, which flickers stray scanlines on high-contrast frames.
Adafruit_Protomatter matrix(WIDTH, 4, 1, rgbPins, 4, addrPins,
                            clockPin, latchPin, oePin, true);

#if SERVER_TLS
WiFiSSLClient client;
#else
WiFiClient client;
#endif

// Animation storage: 64 frames x 2KB = 128KB of the SAMD51's 192KB RAM
uint16_t frames[MAX_FRAMES][FRAME_PIXELS];
uint16_t frameCount = 0;
uint16_t frameDelayMs = 60;
uint32_t animId = 0;

bool pollOutstanding = false;
uint32_t pollSentAt = 0;
uint16_t playIdx = 0;
uint32_t lastFrameAt = 0;

// Shared progress indicator: a 1px trace around the screen perimeter,
// running clockwise from the top-left corner.
#define PERIM (2 * (WIDTH + HEIGHT) - 4)

static void perimPixel(int i, int16_t *x, int16_t *y) {
  if (i < WIDTH) {              // top edge, left -> right
    *x = i; *y = 0; return;
  }
  i -= WIDTH;
  if (i < HEIGHT - 1) {         // right edge, top -> bottom
    *x = WIDTH - 1; *y = 1 + i; return;
  }
  i -= HEIGHT - 1;
  if (i < WIDTH - 1) {          // bottom edge, right -> left
    *x = WIDTH - 2 - i; *y = HEIGHT - 1; return;
  }
  i -= WIDTH - 1;               // left edge, bottom -> top
  *x = 0; *y = HEIGHT - 2 - i;
}

static void drawPerimSpan(int start, int len, uint8_t bright) {
  uint16_t c = ((bright & 0xF8) << 8) | ((bright & 0xFC) << 3) | (bright >> 3);
  matrix.fillScreen(0);
  int16_t x, y;
  for (int k = 0; k < len; k++) {
    perimPixel((start + k) % PERIM, &x, &y);
    matrix.drawPixel(x, y, c);
  }
  matrix.show();
}

// Indeterminate state (booting/connecting): a short white segment sweeping
// left -> right inside the same bar, wrapping around — always the same
// direction as the download fill. Driven from a timer interrupt (below) so
// it keeps animating while the main thread is blocked inside WiFiNINA
// calls (WiFi.begin, TLS handshake). Shown only before the first animation
// is loaded; once playing, connection hiccups stay invisible.
volatile bool connectingAnim = false;
// Download progress, animated by the timer ISR: the download loop publishes
// a target length and the ISR grows the shown ring 1px per tick toward it.
// Once the ring is complete it fades to black, then the ISR goes idle
// (progressTarget = -1), which tells the main thread playback may begin.
#define RING_BRIGHT 26 // loading states at ~10% brightness

volatile int16_t progressTarget = -1; // -1 = ISR idle / not downloading
volatile int16_t progressShown = 0;
volatile int16_t progressBright = 0;
volatile int16_t connectBright = 0;

// Enable the connecting sweep, fading it in from black (no-op if already on)
static void startConnecting(void) {
  if (!connectingAnim) {
    connectBright = 0;
    connectingAnim = true;
  }
}

static void drawConnecting(void) {
  if (frameCount > 0) return; // keep showing the animation we have
  // 16px segment chasing clockwise around the perimeter
  drawPerimSpan((millis() / 25) % PERIM, 16, connectBright);
}

void TC3_Handler(void) {
  TC3->COUNT16.INTFLAG.reg = TC_INTFLAG_OVF;
  if (connectingAnim) {
    if (connectBright < RING_BRIGHT) {
      connectBright += 3; // fade in
      if (connectBright > RING_BRIGHT) connectBright = RING_BRIGHT;
    }
    drawConnecting();
  } else if (progressTarget >= 0) {
    if (progressShown < progressTarget) progressShown++; // filling
    if (progressShown >= PERIM) {
      progressBright -= 3; // ring complete: fade out
      if (progressBright <= 0) {
        progressBright = 0;
        progressTarget = -1; // done — signal the main thread
      }
    } else if (progressBright < RING_BRIGHT) {
      progressBright += 3; // fade in as the fill begins
      if (progressBright > RING_BRIGHT) progressBright = RING_BRIGHT;
    }
    drawPerimSpan(0, progressShown, progressBright);
  }
}

// TC3 at ~20Hz. Priority is set below Protomatter's refresh timer so the
// panel can still refresh (and complete show()'s buffer swap) while our
// handler runs.
static void setupAnimTimer(void) {
  GCLK->PCHCTRL[TC3_GCLK_ID].reg = GCLK_PCHCTRL_GEN_GCLK0 | GCLK_PCHCTRL_CHEN;
  while (!(GCLK->PCHCTRL[TC3_GCLK_ID].reg & GCLK_PCHCTRL_CHEN));
  TC3->COUNT16.CTRLA.bit.ENABLE = 0;
  while (TC3->COUNT16.SYNCBUSY.bit.ENABLE);
  TC3->COUNT16.CTRLA.reg = TC_CTRLA_MODE_COUNT16 | TC_CTRLA_PRESCALER_DIV1024;
  TC3->COUNT16.WAVE.reg = TC_WAVE_WAVEGEN_MFRQ;
  TC3->COUNT16.CC[0].reg = F_CPU / 1024 / 60; // ~60Hz: 1px/tick ring growth
                                              // outpaces the ~30px/s download
  while (TC3->COUNT16.SYNCBUSY.bit.CC0);
  TC3->COUNT16.INTENSET.reg = TC_INTENSET_OVF;
  NVIC_SetPriority(TC3_IRQn, 3);
  NVIC_EnableIRQ(TC3_IRQn);
  TC3->COUNT16.CTRLA.bit.ENABLE = 1;
  while (TC3->COUNT16.SYNCBUSY.bit.ENABLE);
}

static void connectWiFi(void) {
  startConnecting();
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print("WiFi: connecting to ");
    Serial.println(WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    for (int i = 0; i < 100 && WiFi.status() != WL_CONNECTED; i++) delay(100);
  }
  Serial.print("WiFi: connected, ip=");
  Serial.println(WiFi.localIP());
}

// Cheap stall detector; client.connected() costs an SPI round trip, so it
// is queried at most every 250ms and only once the stream has gone quiet.
static bool streamDead(uint32_t lastData) {
  uint32_t quiet = millis() - lastData;
  if (quiet > 5000) return true;
  if (quiet > 250) {
    static uint32_t lastCheck = 0;
    if (millis() - lastCheck > 250) {
      lastCheck = millis();
      if (!client.connected()) return true;
    }
  }
  return false;
}

static bool readFully(uint8_t *dst, size_t n) {
  size_t got = 0;
  uint32_t lastData = millis();
  while (got < n) {
    int r = client.read(dst + got, n - got);
    if (r > 0) {
      got += r;
      lastData = millis();
    } else if (streamDead(lastData)) {
      return false;
    }
  }
  return true;
}

// Parse status line + headers. Returns true on a 200 with the body next;
// fills contentLength (-1 if absent) and whether the server will close.
static bool readHttpHeaders(long *contentLength, bool *willClose) {
  char line[160];
  size_t len = 0;
  bool firstLine = true, ok = false;
  *contentLength = -1;
  *willClose = false;
  uint32_t lastData = millis();
  for (;;) {
    int b = client.read();
    if (b < 0) {
      if (streamDead(lastData)) return false;
      continue;
    }
    lastData = millis();
    if (b != '\n') {
      if (b != '\r' && len < sizeof(line) - 1) line[len++] = (char)b;
      continue;
    }
    line[len] = '\0';
    if (len == 0) return ok; // blank line: body follows
    if (firstLine) {
      ok = strstr(line, " 200 ") != NULL;
      firstLine = false;
    } else if (strncasecmp(line, "Content-Length:", 15) == 0) {
      *contentLength = atol(line + 15);
    } else if (strncasecmp(line, "Connection:", 11) == 0 &&
               strstr(line, "lose") != NULL) { // "close"/"Close"
      *willClose = true;
    }
    len = 0;
  }
}

static void sendRequest(const char *path) {
  client.print("GET ");
  client.print(path);
  client.print(" HTTP/1.1\r\nHost: " SERVER_HOST
               "\r\nX-Frame-Key: " FRAME_KEY
               "\r\nConnection: keep-alive\r\n\r\n");
}

// Draw a playback frame brightness-scaled (255 = as-is)
static void drawFrameScaled(const uint16_t *frame, uint8_t bright) {
  static uint16_t tmp[FRAME_PIXELS];
  uint32_t f = bright + 1; // 1..256 so we can shift instead of divide
  for (int i = 0; i < FRAME_PIXELS; i++) {
    uint16_t p = frame[i];
    tmp[i] = ((((p >> 11) * f) >> 8) << 11) |
             (((((p >> 5) & 0x3F) * f) >> 8) << 5) |
             (((p & 0x1F) * f) >> 8);
  }
  matrix.drawRGBBitmap(0, 0, tmp, WIDTH, HEIGHT);
  matrix.show();
}

// Fade playback in or out over ~320ms; the animation keeps advancing
// through the fade so it reads as a transition, not a freeze.
static void fadePlayback(bool fadeIn) {
  if (frameCount == 0) return;
  for (int step = 0; step <= 16; step++) {
    int v = step * 16;
    if (v > 255) v = 255;
    uint8_t bright = fadeIn ? v : 255 - v;
    if (millis() - lastFrameAt >= frameDelayMs) {
      lastFrameAt = millis();
      playIdx = (playIdx + 1) % frameCount;
    }
    drawFrameScaled(frames[playIdx], bright);
    delay(20);
  }
}

static bool downloadAnimation(void) {
  Serial.println("Anim: downloading");
  fadePlayback(false); // fade out whatever is playing (no-op on first boot)
  sendRequest(SERVER_PATH_ANIMATION "?frame=" FRAME_SLUG);
  long contentLength;
  bool willClose;
  if (!readHttpHeaders(&contentLength, &willClose)) return false;

  uint8_t hdr[12];
  if (!readFully(hdr, sizeof(hdr))) return false;
  if (memcmp(hdr, "ANM0", 4) != 0) {
    Serial.println("Anim: bad magic");
    return false;
  }
  uint32_t id = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
  uint16_t count = hdr[8] | (hdr[9] << 8);
  uint16_t delayMs = hdr[10] | (hdr[11] << 8);
  if (count == 0 || count > MAX_FRAMES) {
    Serial.print("Anim: bad frame count ");
    Serial.println(count);
    return false;
  }
  // The download overwrites the live frame buffer, so the old animation is
  // gone from here on (no RAM for two). Show a progress bar while the new
  // animation downloads; playback starts only once it is fully in RAM.
  frameCount = 0; // also: if we fail midway, don't play a half-mixed buffer
  // hand the display over from the connecting sweep to the progress ring,
  // which fades in from black as it starts filling
  progressShown = 0;
  progressBright = 0;
  progressTarget = 0;
  connectingAnim = false;
  for (uint16_t i = 0; i < count; i++) {
    if (!readFully((uint8_t *)frames[i], FRAME_PIXELS * 2)) {
      progressTarget = -1;
      return false;
    }
    progressTarget = ((uint32_t)(i + 1) * PERIM) / count;
  }
  // let the ISR finish filling the ring and fade it out before playback
  progressTarget = PERIM;
  uint32_t fadeStart = millis();
  while (progressTarget != -1 && millis() - fadeStart < 4000) delay(5);
  animId = id;
  frameCount = count;
  frameDelayMs = delayMs;
  playIdx = 0;
  lastFrameAt = millis();
  fadePlayback(true); // fade the new animation in
  if (willClose) client.stop();
  Serial.print("Anim: id=");
  Serial.print(id);
  Serial.print(" frames=");
  Serial.print(count);
  Serial.print(" delay=");
  Serial.println(delayMs);
  return true;
}

// Read the tiny long-poll response; returns the reported id, or 0 on error.
static uint32_t readPollResponse(void) {
  long contentLength;
  bool willClose;
  if (!readHttpHeaders(&contentLength, &willClose)) return 0;
  char body[32];
  if (contentLength <= 0 || contentLength >= (long)sizeof(body)) return 0;
  if (!readFully((uint8_t *)body, contentLength)) return 0;
  body[contentLength] = '\0';
  if (willClose) client.stop();
  uint32_t id = 0;
  if (sscanf(body, "ID %lu", &id) != 1) return 0;
  return id;
}

void setup(void) {
  Serial.begin(115200);
  if (matrix.begin() != PROTOMATTER_OK) {
    for (;;);
  }
  matrix.setRotation(3); // panel is mounted rotated -90deg
  setupAnimTimer();
  connectWiFi();
  Serial.print("ESP32 NINA firmware: ");
  Serial.println(WiFi.firmwareVersion());
}

void loop(void) {
  // ── network management ──
  if (WiFi.status() != WL_CONNECTED) {
    pollOutstanding = false;
    connectWiFi();
  }
  if (!client.connected()) {
    client.stop();
    pollOutstanding = false;
    startConnecting(); // timer ISR animates the ring while we block
    Serial.println("Server: connecting");
    if (!client.connect(SERVER_HOST, SERVER_PORT)) {
      delay(1000);
      return;
    }
    if (frameCount == 0 || !pollOutstanding) {
      // fresh connection: fetch (or re-fetch) the animation
      if (!downloadAnimation()) {
        client.stop();
        startConnecting();
        delay(1000);
        return;
      }
    }
    connectingAnim = false;
  }

  // keep exactly one long-poll outstanding
  if (!pollOutstanding && client.connected()) {
    char path[160]; // room for the slug + key-less query
    snprintf(path, sizeof(path), "%s?frame=%s&id=%lu", SERVER_PATH_POLL,
             FRAME_SLUG, (unsigned long)animId);
    sendRequest(path);
    pollOutstanding = true;
    pollSentAt = millis();
  }

  // poll response ready? (available() is cheap enough between frames)
  if (pollOutstanding && client.available()) {
    uint32_t id = readPollResponse();
    pollOutstanding = false;
    if (id == 0) {
      client.stop(); // garbled response: reconnect from scratch
    } else if (id != animId) {
      Serial.print("Poll: new animation ");
      Serial.println(id);
      if (client.connected()) {
        if (!downloadAnimation()) client.stop();
      }
    }
  } else if (pollOutstanding && millis() - pollSentAt > POLL_TIMEOUT_MS) {
    Serial.println("Poll: timed out, reconnecting");
    client.stop();
    pollOutstanding = false;
  }

  // ── playback ──
  if (frameCount > 0 && millis() - lastFrameAt >= frameDelayMs) {
    lastFrameAt = millis();
    if (playIdx >= frameCount) playIdx = 0;
    matrix.drawRGBBitmap(0, 0, frames[playIdx], WIDTH, HEIGHT);
    matrix.show();
    playIdx = (playIdx + 1) % frameCount;
  }
}
