#include <Arduino.h>

#define SENSOR_ADC_PIN A0
#define AC_SAMPLE_WINDOW_MS 100 // 100ms captures 5 full 50Hz AC cycles

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n--- A0 Voltage Diagnostic Stream Started ---");
    Serial.println("Format: Instantaneous Raw A0 | 100ms Window Peak A0 | Approx Voltage (V)");
}

void loop() {
    try {
        uint16_t windowPeak = 0;
        uint32_t startMs = millis();
        uint16_t lastInstantRead = 0;

        // Continuous peak capture over 100ms window
        while (millis() - startMs < AC_SAMPLE_WINDOW_MS) {
            uint16_t currentRead = analogRead(SENSOR_ADC_PIN);
            lastInstantRead = currentRead;
            
            if (currentRead > windowPeak) {
                windowPeak = currentRead;
            }
            yield(); // Keep ESP8266 WDT happy
        }

        // ESP8266 ADC input pin accepts 0.0V - 1.0V (or up to 3.3V on boards with onboard dividers like Wemos D1 Mini)
        float voltage = (windowPeak * 3.3f) / 1023.0f; 

        Serial.printf("Raw A0: %4d | 100ms Peak: %4d | Peak Voltage: %.2f V\n", 
                      lastInstantRead, windowPeak, voltage);

        delay(100); // Stream interval
    } catch (...) {
        Serial.println("[LOG_ERROR] Exception in diagnostic stream loop.");
    }
} 