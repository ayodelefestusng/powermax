#include <Arduino.h>

const int ADC_PIN = A0;

void setup() {
  // Initialize the hardware serial port at the standard fast baud rate
  Serial.begin(115200);
  delay(1000);
  
  Serial.println(F("=== A0 Raw Analog Value Scanner ==="));
}

void loop() {
  // Read the current raw value from the analog pin (ranges from 0 to 1023)
  int rawValue = analogRead(ADC_PIN);
  
  // Output the reading directly to the serial interface stream
  Serial.print(F("Current A0 Value: "));
  Serial.println(rawValue);
  
  // Wait half a second before scanning again so the monitor remains human-readable
  delay(500);
}