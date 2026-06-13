// Copy to secrets.h and fill in before flashing.
#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASS "your-wifi-password"

// Server location. SERVER_HOST may be a hostname or IP.
// SERVER_TLS 0 -> http://HOST:PORT...   (WiFiClient)
// SERVER_TLS 1 -> https://HOST:PORT...  (WiFiSSLClient on the ESP32; cert
//   must chain to a root CA in the ESP32's NINA firmware bundle)
#define SERVER_TLS  1
#define SERVER_HOST "ai-pixel-art-frame.leo.gd"
#define SERVER_PORT 443

#define SERVER_PATH_ANIMATION "/animation"
#define SERVER_PATH_POLL      "/poll"

// This frame's identity. Register the frame in the admin panel to get its
// slug and one-time device key, then paste them here before flashing. Each
// of your boards gets a DIFFERENT slug/key. FRAME_SLUG goes in the request
// URL (?frame=...); FRAME_KEY is sent as the X-Frame-Key header.
#define FRAME_SLUG "living-room"
#define FRAME_KEY  "paste-the-device-key-from-the-admin-panel"
