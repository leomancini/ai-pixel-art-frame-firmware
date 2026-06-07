// USB <-> ESP32 (NINA) serial bridge for the MatrixPortal M4.
// Forces the ESP32 into its serial bootloader at startup (GPIO0 low
// through reset), so flash with: esptool --before no_reset --after no_reset

unsigned long baud = 115200;

void setup() {
  Serial.begin(baud);
  SerialNina.begin(baud);

  pinMode(NINA_GPIO0, OUTPUT);
  pinMode(NINA_RESETN, OUTPUT);

  // Hold GPIO0 low while pulsing reset (active low) -> download mode.
  digitalWrite(NINA_GPIO0, LOW);
  digitalWrite(NINA_RESETN, LOW);
  delay(100);
  digitalWrite(NINA_RESETN, HIGH);
  delay(100);
}

void loop() {
  if (Serial.available()) {
    SerialNina.write(Serial.read());
  }
  if (SerialNina.available()) {
    Serial.write(SerialNina.read());
  }
  // track host baud changes (esptool may switch rates)
  if (Serial.baud() != baud) {
    baud = Serial.baud();
    SerialNina.begin(baud);
  }
}
