#include <Arduino.h>
#include <Ds1302.h>

#define RTC_RST_PIN D7  // Chip Enable / Reset
#define RTC_CLK_PIN D2  // Clock
#define RTC_DAT_PIN D1  // Data

Ds1302 rtc(RTC_RST_PIN, RTC_CLK_PIN, RTC_DAT_PIN);

void logInfo(const String& msg) {
    Serial.printf("[INFO] %s\n", msg.c_str());
}

bool isRtcDateTimeInvalid(const Ds1302::DateTime& dt) {
    return dt.year == 0 && dt.month == 0 && dt.day == 0 &&
           dt.hour == 0 && dt.minute == 0 && dt.second == 0;
}

void initializeRtcTimeIfNeeded() {
    Ds1302::DateTime dt;
    rtc.getDateTime(&dt);

    if (isRtcDateTimeInvalid(dt)) {
        dt.year = 26;     // 2026 (YY format)
        dt.month = 8;
        dt.day = 12;
        dt.hour = 8;
        dt.minute = 26;
        dt.second = 37;
        dt.dow = 3;       // Wednesday (DS1302 uses 1=Mon, 7=Sun)

        rtc.setDateTime(&dt);
        logInfo("RTC was unset; initialized to 2026-08-12 08:26:37");
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    rtc.init();
    initializeRtcTimeIfNeeded();
}

void loop() {
    Ds1302::DateTime dt;
    rtc.getDateTime(&dt);

    Serial.printf("[READBACK] 20%02d-%02d-%02d %02d:%02d:%02d\n",
                  dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    delay(2000);
}