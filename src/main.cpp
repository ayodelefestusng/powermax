#include <Arduino.h>
#include <Ds1302.h>

// Pin mapping for Wemos D1 Mini
#define RST_PIN D7   // Enable / reset pin
#define DAT_PIN D6   // Data pin
#define CLK_PIN D5   // Clock pin

// Create DS1302 object
Ds1302 rtc(RST_PIN, CLK_PIN, DAT_PIN);

static void printTwoDigits(int value) {
  if (value < 10) {
    Serial.print('0');
  }
  Serial.print(value);
}

void setup() {
  Serial.begin(115200);

  // Initialize the RTC chip
  rtc.init();
}

void loop() {
  static Ds1302::DateTime lastTime = {0};
  Ds1302::DateTime now;
  rtc.getDateTime(&now);

  bool changed = (now.year != lastTime.year) || (now.month != lastTime.month) ||
                 (now.day != lastTime.day) || (now.hour != lastTime.hour) ||
                 (now.minute != lastTime.minute) || (now.second != lastTime.second);

  if (changed) {
    Serial.print("20");
    printTwoDigits(now.year);
    Serial.print('-');
    printTwoDigits(now.month);
    Serial.print('-');
    printTwoDigits(now.day);
    Serial.print(' ');
    printTwoDigits(now.hour);
    Serial.print(':');
    printTwoDigits(now.minute);
    Serial.print(':');
    printTwoDigits(now.second);
    Serial.println();
    lastTime = now;
  }

  delay(1000);
}