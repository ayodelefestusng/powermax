#include <Arduino.h>

const int ANALOG_PIN = A0;

// Trigger ON if signal deviates above or below baseline (790)
const int THRESHOLD_HIGH = 840; // +50 above idle baseline (790)
const int THRESHOLD_LOW  = 740; // -50 below idle baseline (790)

const unsigned long HOLD_TIME_MS = 500; // Hold ON state for 500ms after last wave edge

bool deviceActive = false;
unsigned long lastActiveTime = 0;

void processSensorReading() {
  int rawVal = analogRead(ANALOG_PIN);
  unsigned long currentMillis = millis();

  // Detect wave activity (either peak > 840 or valley < 740)
  if (rawVal >= THRESHOLD_HIGH || rawVal <= THRESHOLD_LOW) {
    lastActiveTime = currentMillis;
    
    if (!deviceActive) {
      deviceActive = true;
      Serial.printf("[STATE] ON - Wave detected (Value: %d)\n", rawVal);
    }
  }

  // Turn OFF only after signal stays inside quiet zone (741-839) for HOLD_TIME_MS
  if (deviceActive && (currentMillis - lastActiveTime >= HOLD_TIME_MS)) {
    deviceActive = false;
    Serial.printf("[STATE] OFF - Signal quiet (Value: %d)\n", rawVal);
  }
}

void setup() {
  Serial.begin(115200);
}

void loop() {
  processSensorReading();
  delay(10);
}