#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <Ds1302.h>
#include <LittleFS.h>

// ==========================================
// HARDWARE PIN MAPPINGS (ESP8266 / D1 MINI)
// ==========================================
#define SENSOR_ADC_PIN    A0

// DS1302 RTC Pins
#define RTC_CLK_PIN       D2  // Clock
#define RTC_DAT_PIN       D1  // Data
#define RTC_RST_PIN       D7  // Reset / Chip Enable

// ==========================================
// CALIBRATED HARDWARE THRESHOLDS & FILTERING
// ==========================================
// Diagnostic stream showed OFF baseline around 20-22 raw ADC and ON peaks around 360 raw ADC.
// Use a tight hysteresis band around the real signal transition.
const int ADC_OFF_BASE        = 35;  // OFF release: drop below ~0.11V baseline
const int ADC_THRESHOLD       = 80;  // ON trigger: true AC peak above ~0.26V
const int CONSECUTIVE_SAMPLES = 3;   // Debounce counter for signal stability

#define AC_SAMPLE_WINDOW_MS   100    // 100ms window captures 5 full 50Hz AC cycles

// ==========================================
// NETWORK & SERVER CONFIGURATION
// ==========================================
const int LOCAL_TIME_OFFSET_SECONDS = 3600; // UTC+1 (Nigeria / West Africa Time)

const char* SERVER_IP          = "24.144.119.35";
const char* SERVER_PORT        = "8000";
// WiFi Credentials
const char* wifi_ssid          = "igbogbo_modem";
const char* wifi_password      = "@Ajibandele612";
const unsigned long WIFI_TIMEOUT_MS = 15000UL;

// Static Node Metadata
const char* FEEDER_NAME        = "Igbogbo";
const char* TRANSFORMER_NAME   = "Igbogbo Sabo DT";
const char* USER_LATITUDE      = "6.5230";
const char* USER_LONGITUDE     = "3.3420";

// ==========================================
// STORAGE CONSTANTS
// ==========================================
#define CACHE_FILE_PATH       "/telemetry_cache.json"

// ==========================================
// GLOBAL HARDWARE OBJECTS & RUNTIME STATE
// ==========================================
Ds1302 rtc(RTC_RST_PIN, RTC_CLK_PIN, RTC_DAT_PIN);

bool lastPowerStatus = false;
bool isWifiConnected = false;
bool isRtcAvailable  = false;

uint32_t networkBaseEpoch = 0;
unsigned long epochSyncMillis = 0;
unsigned long lastResetHandled = 0;

// Debounce Tracking
int consecutiveSampleCount = 0;
bool pendingTargetStatus   = false;

// ==========================================
// FIFO QUEUE & RETRY BACKOFF CONFIGURATION
// ==========================================
// Retry backoff intervals: 3s, 10s, 30s, 60s, 120s
const uint32_t BACKOFF_INTERVALS_MS[] = {3000, 10000, 30000, 60000, 120000};
const uint8_t MAX_BACKOFF_INDEX       = 4;

uint8_t backoffIndex         = 0;
unsigned long lastRetryAttemptMs = 0;
bool isRetryActive           = false;

// Forward Declarations
bool sendTelemetryHttpWiFi(const String& status, uint32_t timestamp, uint16_t peakVal);
bool flushQueue();

// ==========================================
// LOGGING UTILITIES
// ==========================================
void logInfo(const String& msg) {
    Serial.printf("[LOG_INFO] %s\n", msg.c_str());
}

void logError(const String& msg) {
    Serial.printf("[LOG_ERROR] %s\n", msg.c_str());
}

bool isRtcDateTimeInvalid(const Ds1302::DateTime& dt) {
    return dt.year == 0 && dt.month == 0 && dt.day == 0 &&
           dt.hour == 0 && dt.minute == 0 && dt.second == 0;
}

bool waitForNtpSync(uint32_t timeoutMs) {
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        time_t now = time(nullptr);
        if (now > 1700000000UL) {
            return true;
        }
        delay(250);
        yield();
    }
    return false;
}

bool syncRtcFromNtp() {
    if (!isRtcAvailable) {
        return false;
    }

    time_t now = time(nullptr);
    if (now <= 1700000000UL) {
        return false;
    }

    struct tm* timeInfo = localtime(&now);
    if (timeInfo == nullptr) {
        return false;
    }

    Ds1302::DateTime rtcNow = {0};
    rtcNow.year   = timeInfo->tm_year % 100;
    rtcNow.month  = timeInfo->tm_mon + 1;
    rtcNow.day    = timeInfo->tm_mday;
    rtcNow.hour   = timeInfo->tm_hour;
    rtcNow.minute = timeInfo->tm_min;
    rtcNow.second = timeInfo->tm_sec;
    rtcNow.dow    = timeInfo->tm_wday + 1;

    rtc.setDateTime(&rtcNow);
    logInfo("RTC synchronized from NTP time: " + String(rtcNow.year) + "/" + String(rtcNow.month) + "/" + String(rtcNow.day) + " " +
            String(rtcNow.hour) + ":" + String(rtcNow.minute) + ":" + String(rtcNow.second));
    return true;
}

void printCurrentRtcDateTime() {
    if (!isRtcAvailable) {
        Serial.println("[RTC] unavailable");
        return;
    }

    Ds1302::DateTime dt;
    rtc.getDateTime(&dt);
    if (isRtcDateTimeInvalid(dt)) {
        Serial.println("[RTC] invalid / unset");
        return;
    }

    Serial.printf("[RTC] 20%02d-%02d-%02d %02d:%02d:%02d\n",
                  dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
}

// ==========================================
// WIFI NETWORK INIT
// ==========================================
bool initWiFi() {
    logInfo("Connecting to WiFi SSID: " + String(wifi_ssid));
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid, wifi_password);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_TIMEOUT_MS) {
        delay(500);
        Serial.print(".");
        yield();
    }

    if (WiFi.status() == WL_CONNECTED) {
        logInfo("\nWiFi Connected! IP Address: " + WiFi.localIP().toString());
        return true;
    }

    logError("\nWiFi Connection failed.");
    return false;
}

// ==========================================
// TIME & RTC ENGINE
// ==========================================
uint32_t ds1302ToEpoch(const Ds1302::DateTime &dt) {
    uint16_t year = 2000 + dt.year;
    uint8_t month = dt.month;
    uint8_t day = dt.day;
    uint8_t hour = dt.hour;
    uint8_t minute = dt.minute;
    uint8_t second = dt.second;

    uint32_t numDays = 0;
    uint8_t daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) {
        daysInMonth[1] = 29;
    }
    for (uint16_t i = 1970; i < year; i++) {
        numDays += 365;
        if (i % 4 == 0 && (i % 100 != 0 || i % 400 == 0)) numDays++;
    }
    for (uint8_t i = 0; i < month - 1; i++) {
        numDays += daysInMonth[i];
    }
    numDays += day - 1;

    return (uint32_t)((numDays * 86400UL) + (hour * 3600UL) + (minute * 60UL) + second);
}

uint32_t getCurrentTimestamp() {
    try {
        if (isRtcAvailable) {
            Ds1302::DateTime dt;
            rtc.getDateTime(&dt);
            if (dt.year > 0) {
                return ds1302ToEpoch(dt);
            }
        }

        time_t now = time(nullptr);
        if (now > 1700000000UL) {
            return (uint32_t)now;
        }

        if (networkBaseEpoch > 0) {
            uint32_t relativeOffset = (millis() - epochSyncMillis) / 1000UL;
            return networkBaseEpoch + relativeOffset;
        }
        return (uint32_t)(millis() / 1000UL);
    } catch (...) {
        logError("Exception calculating current timestamp.");
        return (uint32_t)(millis() / 1000UL);
    }
}

// ==========================================
// ANALOG SAMPLING & HYSTERESIS
// ==========================================
uint16_t getFilteredAnalogPeak() {
    uint16_t peakValue = 0;
    uint32_t windowStart = millis();

    while (millis() - windowStart < AC_SAMPLE_WINDOW_MS) {
        uint16_t currentRead = analogRead(SENSOR_ADC_PIN);
        if (currentRead > peakValue) {
            peakValue = currentRead;
        }
        yield();
    }

    return peakValue;
}

bool readPowerStatus(bool previousState) {
    try {
        uint16_t peakVal = getFilteredAnalogPeak();

        if (previousState) {
            return peakVal > ADC_OFF_BASE;
        }

        return peakVal > ADC_THRESHOLD;
    } catch (...) {
        logError("Exception reading power analog status.");
        return previousState;
    }
}

// ==========================================
// DISPATCH PIPELINE: WIFI HTTP
// ==========================================
bool sendTelemetryHttpWiFi(const String& status, uint32_t timestamp, uint16_t peakVal) {
    if (WiFi.status() != WL_CONNECTED) return false;

    try {
        String payload = "{";
        payload += "\"stat\":\"" + status + "\",";
        payload += "\"timestamp\":" + String(timestamp) + ",";
        payload += "\"val\":" + String(peakVal) + ",";
        payload += "\"fdr\":\"" + String(FEEDER_NAME) + "\",";
        payload += "\"tf\":\"" + String(TRANSFORMER_NAME) + "\"";
        payload += "}";

        String url = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/power-tracker-gateway/";
        logInfo("[WiFi-HTTP] Outbound Payload -> " + payload);

        WiFiClient client;
        HTTPClient http;
        http.begin(client, url);
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(10000);

        int httpCode = http.POST(payload);
        String serverResponseBody = http.getString();
        http.end();

        logInfo("[WiFi-HTTP] Gateway Server Response Code: " + String(httpCode));
        logInfo("[WiFi-HTTP] Gateway Server Response Body: " + serverResponseBody);
        if (httpCode < 200 || httpCode >= 300) {
            return false;
        }

        return serverResponseBody.indexOf("\"status\":\"error\"") == -1;
    } catch (...) {
        logError("Exception during sendTelemetryHttpWiFi execution.");
        return false;
    }
}

// ==========================================
// FIFO QUEUE & EXPONENTIAL BACKOFF ENGINE
// ==========================================
void enqueueEvent(const String& status, uint32_t timestamp, uint16_t peakVal) {
    try {
        File cache = LittleFS.open(CACHE_FILE_PATH, "a");
        if (!cache) {
            logError("Unable to open LittleFS queue file. Dropping record.");
            return;
        }
        String lineRecord = status + "," + String(timestamp) + "," + String(peakVal) + "," + String(USER_LATITUDE) + "," + String(USER_LONGITUDE) + "\n";
        cache.print(lineRecord);
        cache.close();
        logInfo("FIFO Queue Enqueued: Status=" + status + ", Timestamp=" + String(timestamp) + ", Peak=" + String(peakVal));

        // Start retry backoff cycle immediately
        isRetryActive = true;
        backoffIndex = 0;
        lastRetryAttemptMs = 0; // Trigger immediate network check
    } catch (...) {
        logError("Exception while enqueuing event into FIFO queue.");
    }
}

bool hasPendingQueue() {
    if (!LittleFS.exists(CACHE_FILE_PATH)) return false;
    File cache = LittleFS.open(CACHE_FILE_PATH, "r");
    if (!cache) return false;
    bool hasData = false;
    while (cache.available()) {
        String line = cache.readStringUntil('\n');
        line.trim();
        if (line.length() >= 5) {
            hasData = true;
            break;
        }
    }
    cache.close();
    return hasData;
}

bool flushQueue() {
    try {
        if (!LittleFS.exists(CACHE_FILE_PATH)) return true;

        File cache = LittleFS.open(CACHE_FILE_PATH, "r");
        if (!cache) return true;

        String tempPath = "/temp_queue.json";
        File tempCache = LittleFS.open(tempPath, "w");
        if (!tempCache) {
            cache.close();
            return false;
        }

        bool allDelivered = true;
        bool deliveryBlocked = false;
        uint16_t flushedCount = 0;
        uint16_t retainedCount = 0;

        while (cache.available()) {
            String record = cache.readStringUntil('\n');
            record.trim();
            if (record.length() < 5) continue;

            if (deliveryBlocked) {
                // Preserve remaining events in strict FIFO order
                tempCache.println(record);
                retainedCount++;
                allDelivered = false;
                continue;
            }

            int idx1 = record.indexOf(',');
            int idx2 = record.indexOf(',', idx1 + 1);
            int idx3 = record.indexOf(',', idx2 + 1);
            int idx4 = record.indexOf(',', idx3 + 1);

            if (idx1 != -1 && idx2 != -1 && idx3 != -1 && idx4 != -1) {
                String status = record.substring(0, idx1);
                uint32_t timestamp = record.substring(idx1 + 1, idx2).toInt();
                uint16_t peakVal = record.substring(idx2 + 1, idx3).toInt();

                bool sent = false;
                if (WiFi.status() == WL_CONNECTED) {
                    sent = sendTelemetryHttpWiFi(status, timestamp, peakVal);
                }

                if (sent) {
                    flushedCount++;
                    logInfo("FIFO Event Delivery Confirmed & Purged: Timestamp=" + String(timestamp) + " State=" + status);
                } else {
                    logError("FIFO Event Delivery Failed: Timestamp=" + String(timestamp) + ". Retaining in queue.");
                    tempCache.println(record);
                    retainedCount++;
                    allDelivered = false;
                    deliveryBlocked = true; // Stop subsequent dispatches this pass
                }
            }
        }

        cache.close();
        tempCache.close();

        LittleFS.remove(CACHE_FILE_PATH);
        if (retainedCount > 0) {
            LittleFS.rename(tempPath, CACHE_FILE_PATH);
        } else {
            LittleFS.remove(tempPath);
        }

        logInfo("FIFO Queue Flush Result -> Delivered: " + String(flushedCount) + ", Retained: " + String(retainedCount));
        return allDelivered;
    } catch (...) {
        logError("Exception encountered during FIFO queue flush.");
        return false;
    }
}

// ==========================================
// ARDUINO MAIN ENTRY POINTS
// ==========================================
void setup() {
    try {
        Serial.begin(115200);

        logInfo("Booting Feeder Power Monitor Node (" + String(FEEDER_NAME) + ")...");

        if (!LittleFS.begin()) {
            logError("LittleFS mount failed! Persistent FIFO queue unavailable.");
        }

        rtc.init();
        Ds1302::DateTime now;
        rtc.getDateTime(&now);
        isRtcAvailable = !isRtcDateTimeInvalid(now);
        if (isRtcAvailable) {
            logInfo("DS1302 RTC initialized.");
        } else {
            logError("DS1302 RTC read is invalid / unset.");
        }

        delay(1000);

        isWifiConnected = initWiFi();

        if (isWifiConnected) {
            configTime(LOCAL_TIME_OFFSET_SECONDS, 0, "pool.ntp.org", "time.google.com");
            logInfo("NTP time sync requested via WiFi for UTC+1 local time.");
            if (waitForNtpSync(15000)) {
                syncRtcFromNtp();
            } else {
                logError("NTP time sync did not complete within timeout.");
            }
        }

        // Capture initial boot state & enqueue baseline
        lastPowerStatus = readPowerStatus(false);
        uint32_t initTs = getCurrentTimestamp();
        uint16_t initPeak = getFilteredAnalogPeak();
        logInfo("Initial Power Status -> " + String(lastPowerStatus ? "ON" : "OFF") + " (Peak: " + String(initPeak) + ")");
        enqueueEvent(lastPowerStatus ? "on" : "off", initTs, initPeak);

        // Attempt initial flush
        if (flushQueue()) {
            logInfo("Initial queue flush completed successfully.");
            isRetryActive = false;
        } else {
            logInfo("Initial queue flush incomplete. Backoff retry cycle active.");
            isRetryActive = true;
            backoffIndex = 0;
            lastRetryAttemptMs = millis();
        }
    } catch (...) {
        logError("Fatal exception encountered during setup.");
    }
}

void loop() {
    try {
        static unsigned long lastRtcPrintMs = 0;
        if (millis() - lastRtcPrintMs >= 5000UL) {
            lastRtcPrintMs = millis();
            printCurrentRtcDateTime();
        }

        // 1. Power status change detection & debouncing
        bool previousState = lastPowerStatus;
        bool candidateState = readPowerStatus(previousState);
        if (candidateState != previousState) {
            delay(1200); // Debounce confirmation window
            bool confirmed = readPowerStatus(candidateState);
            if (confirmed == candidateState) {
                lastPowerStatus = confirmed;
                uint32_t eventTime = getCurrentTimestamp();
                uint16_t currentPeak = getFilteredAnalogPeak();
                logInfo("Power state transition detected! New State: " + String(lastPowerStatus ? "ON" : "OFF") + " at " + String(eventTime));
                enqueueEvent(lastPowerStatus ? "on" : "off", eventTime, currentPeak);
            }
        }

        // 2. Monitor WiFi status transitions
        if (!isWifiConnected && WiFi.status() == WL_CONNECTED) {
            isWifiConnected = true;
            logInfo("WiFi reconnected.");
            if (hasPendingQueue()) {
                isRetryActive = true;
                backoffIndex = 0;
                lastRetryAttemptMs = 0; // Trigger immediate flush
            }
        } else if (isWifiConnected && WiFi.status() != WL_CONNECTED) {
            isWifiConnected = false;
            logError("WiFi connection lost.");
        }

        // 3. FIFO Queue Retry & Exponential Backoff Engine
        if (isRetryActive) {
            if (millis() - lastRetryAttemptMs >= BACKOFF_INTERVALS_MS[backoffIndex]) {
                lastRetryAttemptMs = millis();
                logInfo("Backoff retry timer triggered (Interval: " + String(BACKOFF_INTERVALS_MS[backoffIndex] / 1000) + "s, Level " + String(backoffIndex + 1) + "). Checking network & flushing queue...");

                bool flushed = flushQueue();
                if (flushed) {
                    logInfo("All queued events confirmed delivered and purged. Stopping retry checks until next power change.");
                    isRetryActive = false;
                    backoffIndex = 0;
                } else {
                    if (backoffIndex < MAX_BACKOFF_INDEX) {
                        backoffIndex++;
                    }
                    logInfo("Flush incomplete / offline. Next retry in " + String(BACKOFF_INTERVALS_MS[backoffIndex] / 1000) + "s.");
                }
            }
        }

        delay(100);
        yield();
    } catch (...) {
        logError("Exception inside main system loop.");
    }
}