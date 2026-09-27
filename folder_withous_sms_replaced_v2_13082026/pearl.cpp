/**
 * ============================================================
 * PEARL DT — Three-Phase Power Tracker Firmware (ESP32)
 * ============================================================
 * Hardware  : ESP32 DevKit
 * ADC Pins  : GPIO36 (Red), GPIO39 (Yellow), GPIO34 (Blue)
 * GSM UART  : GPIO16 (RX ← GSM TX), GPIO17 (TX → GSM RX)
 * GSM RTS   : GPIO4 (Sleep/RTS control)
 * RTC Pins  : GPIO25 (CLK), GPIO33 (DAT), GPIO32 (RST)
 *
 * Comms Priority:
 *  1. WiFi HTTP POST  (V40 Lite)
 *  2. GSM GPRS HTTP   (Airtel APN)
 *  3. Persistent LittleFS FIFO Queue with Backoff Retries
 *
 * Triggers on Phase Drop-Off or Recovery Events (ON ↔ OFF).
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <HardwareSerial.h>
#include <Ds1302.h>
#include <LittleFS.h>
#include <time.h>
#include <algorithm>
#include <cmath>
#include <vector>

// ─── ADC Pin Definitions ────────────────────────────────────
const int ADC_PIN_RED    = 36; // VP
const int ADC_PIN_YELLOW = 39; // VN
const int ADC_PIN_BLUE   = 34; // D34

// ─── Thresholds & State Logic ────────────────────────────────
const float VOLTAGE_THRESHOLD_ON  = 95.0f;  
const float VOLTAGE_THRESHOLD_OFF = 65.0f;  
const float MAX_VALID_VOLTAGE     = 280.0f;

// ─── Calibration Gains & Offsets ─────────────────────────────
const float CAL_GAIN_R   = 12.968f;
const float CAL_OFFSET_R = 0.00f;

const float CAL_GAIN_Y   = 0.4598f; 
const float CAL_OFFSET_Y = 0.00f;

const float CAL_GAIN_B   = 0.4802f; 
const float CAL_OFFSET_B = 0.00f;

const float NOISE_FLOOR_CUTOFF = 5.0f;
const float RED_NOISE_FLOOR_CUTOFF = 10.0f;

// ─── GSM UART (ESP32 HardwareSerial 2) ──────────────────────
const int GSM_RX_PIN  = 16; // GPIO16 ← GSM TXD
const int GSM_TX_PIN  = 17; // GPIO17 → GSM RXD
const int GSM_RTS_PIN = 4;  // GSM sleep/RTS control

HardwareSerial gsm(2); // UART2

// ─── DS1302 RTC Pins ────────────────────────────────────────
#define RTC_CLK_PIN 25  // Clock
#define RTC_DAT_PIN 33  // Data
#define RTC_RST_PIN 32  // Reset / Chip Enable

Ds1302 rtc(RTC_RST_PIN, RTC_CLK_PIN, RTC_DAT_PIN);

// ─── WiFi Credentials ───────────────────────────────────────
const char* WIFI_SSID     = "V40 Lite";
const char* WIFI_PASSWORD = "@Ajibandele612";
const unsigned long WIFI_TIMEOUT_MS = 15000UL;

// ─── Server Configuration ───────────────────────────────────
const int LOCAL_TIME_OFFSET_SECONDS = 3600; // UTC+1 (Nigeria / West Africa Time)
const char* SERVER_IP   = "24.144.119.35";
const char* SERVER_PORT = "8000";
const char* ENDPOINT    = "/power-tracker-gateway/";

// ─── GSM APN ────────────────────────────────────────────────
const char* CURRENT_APN = "web.gprs.mtnnigeria.net";

// ─── Node / DT Identity ─────────────────────────────────────
const char* FEEDER_NAME  = "Pearl Feeder";
const char* TRANSFORMER  = "PEARL DT";
const char* DT_CODE      = "PEARL";

// ─── GPS Fallback Coordinates ───────────────────────────────
const char* USER_LATITUDE  = "6.5230";
const char* USER_LONGITUDE = "3.3420";

// ─── Cache File Path ────────────────────────────────────────
const char* CACHE_FILE = "/pearl_cache.json";

// ─── Runtime State ──────────────────────────────────────────
bool wifi_connected = false;
bool gsm_connected  = false;
bool is_rtc_available = false;

String sim_serial = "UNKNOWN";

uint32_t network_base_epoch = 0;
unsigned long epoch_sync_ms  = 0;
unsigned long last_reset_handled = 0;

// ─── Three-Phase State ──────────────────────────────────────
struct PhaseReadings {
    float voltsR, voltsY, voltsB;
    bool  stateR, stateY, stateB;
    bool  readSuccess;
};

float smoothR = 0.0f;
float smoothY = 0.0f;
float smoothB = 0.0f;

// Last confirmed states for transition & drop-off tracking
bool lastStateR = false;
bool lastStateY = false;
bool lastStateB = false;

// ─── FIFO Queue & Backoff Engine ────────────────────────────
// Retry backoff intervals: 3s, 10s, 30s, 60s, 120s
const uint32_t BACKOFF_INTERVALS_MS[] = {3000, 10000, 30000, 60000, 120000};
const uint8_t MAX_BACKOFF_INDEX       = 4;

uint8_t backoff_index          = 0;
unsigned long last_retry_ms    = 0;
bool is_retry_active           = false;

// ─── Function Prototypes ────────────────────────────────────
PhaseReadings readPhaseSensors();
float calculateRMS(const std::vector<int>& samples, float gain, float offset, const char* phaseLabel);
float updatePhaseFilter(float currentSmooth, float rawVolts);
bool evaluatePhaseState(float volts, bool currentState);

bool initWiFi();
bool initGSM();
void fetchSIMIdentity();
void syncNetworkTime();
uint32_t getCurrentTimestamp();
bool sendHTTPWiFi(const PhaseReadings &p, uint32_t ts);
bool sendHTTPGSM(const PhaseReadings &p, uint32_t ts);
void enqueueEvent(const PhaseReadings &p, uint32_t ts);
bool flushQueue();
bool hasPendingQueue();
void monitorForReset();
void setGsmRts(bool enabled);
String sendAT(String cmd, int timeout_ms);
String cleanResponse(String resp);
String buildJSONPayload(const PhaseReadings &p, uint32_t ts);

// ==========================================
// RTC & TIME UTILITIES
// ==========================================
bool isRtcDateTimeInvalid(const Ds1302::DateTime& dt) {
    return dt.year == 0 && dt.month == 0 && dt.day == 0 &&
           dt.hour == 0 && dt.minute == 0 && dt.second == 0;
}

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
    if (!is_rtc_available) {
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
    Serial.printf("[RTC] Synchronized from NTP time: 20%02d/%02d/%02d %02d:%02d:%02d\n",
                  rtcNow.year, rtcNow.month, rtcNow.day, rtcNow.hour, rtcNow.minute, rtcNow.second);
    return true;
}

void printCurrentRtcDateTime() {
    if (!is_rtc_available) {
        Serial.println(F("[RTC] unavailable"));
        return;
    }

    Ds1302::DateTime dt;
    rtc.getDateTime(&dt);
    if (isRtcDateTimeInvalid(dt)) {
        Serial.println(F("[RTC] invalid / unset"));
        return;
    }

    Serial.printf("[RTC] 20%02d-%02d-%02d %02d:%02d:%02d\n",
                  dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
}

void syncNetworkTime() {
    Serial.println(F("[TIME] Querying GSM network time (AT+CCLK?)..."));
    String resp = sendAT("AT+CCLK?", 2000);
    int idx = resp.indexOf("+CCLK: \"");
    if (idx == -1) {
        Serial.println(F("[TIME] NITZ unavailable. Retaining current DS1302 settings."));
        return;
    }

    int si = idx + 8;
    if ((unsigned int)(si + 17) > resp.length()) return;

    String rtcDate = resp.substring(si, si + 8);      // yy/mm/dd
    String rtcTime = resp.substring(si + 9, si + 17); // HH:MM:SS

    int year   = 2000 + rtcDate.substring(0, 2).toInt();
    int month  = rtcDate.substring(3, 5).toInt();
    int day    = rtcDate.substring(6, 8).toInt();
    int hour   = rtcTime.substring(0, 2).toInt();
    int minute = rtcTime.substring(3, 5).toInt();
    int second = rtcTime.substring(6, 8).toInt();

    if (year >= 2026 && month > 0 && day > 0) {
        if (is_rtc_available) {
            Ds1302::DateTime netTime = {0};
            netTime.year   = year - 2000;
            netTime.month  = month;
            netTime.day    = day;
            netTime.hour   = hour;
            netTime.minute = minute;
            netTime.second = second;
            rtc.setDateTime(&netTime);
            Serial.println(F("[RTC] Synchronized hardware DS1302 with Cell Tower time."));
        }

        long numDays = 0;
        int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) daysInMonth[1] = 29;
        for (int i = 1970; i < year; i++) {
            numDays += 365;
            if (i % 4 == 0 && (i % 100 != 0 || i % 400 == 0)) numDays++;
        }
        for (int i = 0; i < month - 1; i++) numDays += daysInMonth[i];
        numDays += day - 1;

        network_base_epoch = (numDays * 86400UL) + (hour * 3600UL) + (minute * 60UL) + second;
        epoch_sync_ms  = millis();
        Serial.printf("[TIME] NITZ epoch synced: %u\n", network_base_epoch);
    }
}

uint32_t getCurrentTimestamp() {
    if (is_rtc_available) {
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

    if (network_base_epoch > 0) {
        unsigned long offset = (millis() - epoch_sync_ms) / 1000UL;
        return network_base_epoch + (uint32_t)offset;
    }
    return (uint32_t)(millis() / 1000UL);
}

// ════════════════════════════════════════════════════════════
// SETUP
// ════════════════════════════════════════════════════════════
void setup() {
    try {
        Serial.begin(115200);
        delay(200);
        Serial.println(F("\n=== PEARL DT — Three-Phase Power Tracker Boot ==="));

        // Configure ADC pins and 11dB attenuation
        analogReadResolution(12);
        analogSetPinAttenuation(ADC_PIN_RED, ADC_11db);
        analogSetPinAttenuation(ADC_PIN_YELLOW, ADC_11db);
        analogSetPinAttenuation(ADC_PIN_BLUE, ADC_11db);

        pinMode(ADC_PIN_RED, INPUT);
        pinMode(ADC_PIN_YELLOW, INPUT);
        pinMode(ADC_PIN_BLUE, INPUT);

        // GSM RTS / sleep control
        pinMode(GSM_RTS_PIN, OUTPUT);
        setGsmRts(true);

        // Mount LittleFS
        if (!LittleFS.begin(true)) {
            Serial.println(F("[CRITICAL] LittleFS mount failed — FIFO queue unavailable."));
        } else {
            Serial.println(F("[INFO] LittleFS mounted OK."));
        }

        // Initialize DS1302 RTC
        rtc.init();
        Ds1302::DateTime now;
        rtc.getDateTime(&now);
        is_rtc_available = true;
        Serial.println(F("[INFO] DS1302 RTC initialized."));

        // Stage 1: Try WiFi
        wifi_connected = initWiFi();

        if (wifi_connected) {
            configTime(LOCAL_TIME_OFFSET_SECONDS, 0, "pool.ntp.org", "time.google.com");
            Serial.println(F("[TIME] NTP sync requested via WiFi."));
            if (waitForNtpSync(15000)) {
                syncRtcFromNtp();
            } else {
                Serial.println(F("[TIME] NTP sync timed out."));
            }
        } else {
            Serial.println(F("[WIFI] WiFi unavailable — attempting GSM GPRS pipeline."));

            // Stage 2: Try GSM
            gsm.begin(4800, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
            delay(500);
            gsm.println("AT+IPR=4800");
            delay(500);
            while (gsm.available()) gsm.read();

            sendAT("AT+CLTS=1", 1000);
            sendAT("AT&W", 1000);

            int gsm_attempts = 0;
            while (!gsm_connected && gsm_attempts < 3) {
                gsm_attempts++;
                gsm_connected = initGSM();
                if (!gsm_connected) {
                    Serial.printf("[GSM] Attempt %d failed. Retrying...\n", gsm_attempts);
                    delay(5000);
                }
            }

            if (gsm_connected) {
                fetchSIMIdentity();
                syncNetworkTime();
            }
        }

        // Read initial phase states
        PhaseReadings initReadings = readPhaseSensors();
        lastStateR = initReadings.stateR;
        lastStateY = initReadings.stateY;
        lastStateB = initReadings.stateB;

        Serial.printf("[INIT] Initial Phase States → R:%s Y:%s B:%s\n",
                      initReadings.stateR ? "ON" : "OFF",
                      initReadings.stateY ? "ON" : "OFF",
                      initReadings.stateB ? "ON" : "OFF");
        Serial.printf("[INIT] Initial Voltages     → R:%.1fV Y:%.1fV B:%.1fV\n",
                      initReadings.voltsR, initReadings.voltsY, initReadings.voltsB);

        uint32_t initTs = getCurrentTimestamp();
        enqueueEvent(initReadings, initTs);

        // Attempt initial flush
        if (flushQueue()) {
            Serial.println(F("[INIT] Initial queue flush completed successfully."));
            is_retry_active = false;
        } else {
            Serial.println(F("[INIT] Initial flush incomplete. Starting backoff retry engine."));
            is_retry_active = true;
            backoff_index = 0;
            last_retry_ms = millis();
        }
    } catch (...) {
        Serial.println(F("[ERROR] Exception caught during setup initialization."));
    }
}

// ════════════════════════════════════════════════════════════
// MAIN LOOP
// ════════════════════════════════════════════════════════════
void loop() {
    try {
        static unsigned long lastRtcPrintMs = 0;
        if (millis() - lastRtcPrintMs >= 5000UL) {
            lastRtcPrintMs = millis();
            printCurrentRtcDateTime();
        }

        PhaseReadings current = readPhaseSensors();

        if (current.readSuccess) {
            // Detect any state change (drop-off or restoration on any phase)
            bool r_changed = (current.stateR != lastStateR);
            bool y_changed = (current.stateY != lastStateY);
            bool b_changed = (current.stateB != lastStateB);

            if (r_changed || y_changed || b_changed) {
                // Debounce confirmation window (1.2s)
                delay(1200);
                current = readPhaseSensors();

                bool r_still_changed = (current.stateR != lastStateR);
                bool y_still_changed = (current.stateY != lastStateY);
                bool b_still_changed = (current.stateB != lastStateB);

                if (r_still_changed || y_still_changed || b_still_changed) {
                    bool drop_off_occurred = (lastStateR && !current.stateR) ||
                                             (lastStateY && !current.stateY) ||
                                             (lastStateB && !current.stateB);

                    if (drop_off_occurred) {
                        Serial.println(F("[EVENT] ALERT: Phase drop-off detected!"));
                    } else {
                        Serial.println(F("[EVENT] INFO: Phase restoration detected!"));
                    }

                    Serial.printf("[EVENT] New States → R:%s Y:%s B:%s\n",
                                  current.stateR ? "ON" : "OFF",
                                  current.stateY ? "ON" : "OFF",
                                  current.stateB ? "ON" : "OFF");

                    lastStateR = current.stateR;
                    lastStateY = current.stateY;
                    lastStateB = current.stateB;

                    uint32_t eventTs = getCurrentTimestamp();
                    enqueueEvent(current, eventTs);

                    // Dispatch a state-change event immediately; backoff is only for failures.
                    if (flushQueue()) {
                        is_retry_active = false;
                        backoff_index = 0;
                    } else {
                        is_retry_active = true;
                        backoff_index = 0;
                        last_retry_ms = millis();
                    }
                }
            }
        }

        // Verify WiFi status transitions
        if (!wifi_connected && WiFi.status() == WL_CONNECTED) {
            wifi_connected = true;
            Serial.println(F("[WIFI] WiFi reconnected."));
            if (hasPendingQueue()) {
                is_retry_active = true;
                backoff_index = 0;
                last_retry_ms = 0; // Trigger immediate flush
            }
        } else if (wifi_connected && WiFi.status() != WL_CONNECTED) {
            wifi_connected = false;
            Serial.println(F("[WIFI] WiFi connection lost."));
        }

        // FIFO Queue Retry & Exponential Backoff Engine
        if (is_retry_active) {
            if (millis() - last_retry_ms >= BACKOFF_INTERVALS_MS[backoff_index]) {
                last_retry_ms = millis();
                Serial.printf("[RETRY] Backoff timer triggered (%us, Level %u). Checking network & flushing queue...\n",
                              BACKOFF_INTERVALS_MS[backoff_index] / 1000, backoff_index + 1);

                bool flushed = flushQueue();
                if (flushed) {
                    Serial.println(F("[RETRY] All queued events confirmed delivered and purged. Stopping retry checks."));
                    is_retry_active = false;
                    backoff_index = 0;
                } else {
                    if (backoff_index < MAX_BACKOFF_INDEX) {
                        backoff_index++;
                    }
                    Serial.printf("[RETRY] Flush incomplete. Next retry in %us.\n",
                                  BACKOFF_INTERVALS_MS[backoff_index] / 1000);
                }
            }
        }

        if (!wifi_connected) {
            monitorForReset();
        }

        delay(100);
        yield();
    } catch (...) {
        Serial.println(F("[ERROR] Exception caught in main loop execution."));
    }
}

// ════════════════════════════════════════════════════════════
// TRUE RMS CALCULATOR WITH DYNAMIC MIDPOINT BIAS
// ════════════════════════════════════════════════════════════
float calculateRMS(const std::vector<int>& samples, float gain, float offset, const char* phaseLabel) {
    if (samples.empty()) return 0.0f;

    try {
        double sumSamples = 0.0;
        for (int raw : samples) {
            sumSamples += raw;
        }
        double dynamicMidpoint = sumSamples / static_cast<double>(samples.size());

        double sumSquaredDiff = 0.0;
        for (int raw : samples) {
            double diff = static_cast<double>(raw) - dynamicMidpoint;
            sumSquaredDiff += (diff * diff);
        }

        double meanSquare = sumSquaredDiff / static_cast<double>(samples.size());
        float rawRms = static_cast<float>(std::sqrt(meanSquare));

        float noiseFloorCutoff = NOISE_FLOOR_CUTOFF;
        if (phaseLabel != nullptr && phaseLabel[0] == 'R') {
            noiseFloorCutoff = RED_NOISE_FLOOR_CUTOFF;
        }

        if (rawRms < noiseFloorCutoff) {
            return 0.0f;
        }

        return (rawRms * gain) + offset;
    } catch (...) {
        Serial.println(F("[ERROR] Exception trapped during RMS calculation."));
        return 0.0f;
    }
}

// ════════════════════════════════════════════════════════════
// DYNAMIC ADAPTIVE EMA FILTER
// ════════════════════════════════════════════════════════════
float updatePhaseFilter(float currentSmooth, float rawVolts) {
    try {
        if (currentSmooth < 10.0f && rawVolts >= 15.0f) {
            return rawVolts;
        }

        if (rawVolts < 15.0f && currentSmooth > 50.0f) {
            return rawVolts;
        }

        if (rawVolts < 10.0f && currentSmooth < 15.0f) {
            return 0.0f;
        }

        float diff = std::fabs(rawVolts - currentSmooth);
        float alpha = 0.15f; 

        if (diff > 50.0f) {
            alpha = 0.85f; 
        } else if (diff > 20.0f) {
            alpha = 0.45f;
        }

        return (currentSmooth * (1.0f - alpha)) + (rawVolts * alpha);
    } 
    catch (...) {
        Serial.println(F("[ERROR] Exception trapped in updatePhaseFilter."));
        return currentSmooth;
    }
}

bool evaluatePhaseState(float volts, bool currentState) {
    if (currentState) {
        return (volts >= VOLTAGE_THRESHOLD_OFF);
    } else {
        return (volts >= VOLTAGE_THRESHOLD_ON);
    }
}

// ════════════════════════════════════════════════════════════
// PHASE SENSOR SAMPLING & SENSING ENGINE
// ════════════════════════════════════════════════════════════
PhaseReadings readPhaseSensors() {
    PhaseReadings data = {0.0f, 0.0f, 0.0f, false, false, false, false};

    try {
        std::vector<int> samplesR;
        std::vector<int> samplesY;
        std::vector<int> samplesB;

        samplesR.reserve(300);
        samplesY.reserve(300);
        samplesB.reserve(300);

        // Sample across 200ms (~10 full 50Hz cycles)
        unsigned long start = millis();
        while (millis() - start < 200UL) {
            int r = analogRead(ADC_PIN_RED);
            int y = analogRead(ADC_PIN_YELLOW);
            int b = analogRead(ADC_PIN_BLUE);

            if (r >= 0 && r <= 4095) samplesR.push_back(r);
            if (y >= 0 && y <= 4095) samplesY.push_back(y);
            if (b >= 0 && b <= 4095) samplesB.push_back(b);

            delayMicroseconds(150);
        }

        // Calculate RMS per phase
        float rawVoltsR = calculateRMS(samplesR, CAL_GAIN_R, CAL_OFFSET_R, "R");
        float rawVoltsY = calculateRMS(samplesY, CAL_GAIN_Y, CAL_OFFSET_Y, "Y");
        float rawVoltsB = calculateRMS(samplesB, CAL_GAIN_B, CAL_OFFSET_B, "B");

        // Cap at upper limit
        rawVoltsR = std::min(rawVoltsR, MAX_VALID_VOLTAGE);
        rawVoltsY = std::min(rawVoltsY, MAX_VALID_VOLTAGE);
        rawVoltsB = std::min(rawVoltsB, MAX_VALID_VOLTAGE);

        // Exponential smoothing filter
        smoothR = updatePhaseFilter(smoothR, rawVoltsR);
        smoothY = updatePhaseFilter(smoothY, rawVoltsY);
        smoothB = updatePhaseFilter(smoothB, rawVoltsB);

        data.voltsR = smoothR;
        data.voltsY = smoothY;
        data.voltsB = smoothB;

        // Return candidates only. The main loop commits them after debounce.
        data.stateR = evaluatePhaseState(data.voltsR, lastStateR);
        data.stateY = evaluatePhaseState(data.voltsY, lastStateY);
        data.stateB = evaluatePhaseState(data.voltsB, lastStateB);

        data.readSuccess = true;
        return data;

    } catch (...) {
        Serial.println(F("[ERROR] Exception in readPhaseSensors execution."));
        data.readSuccess = false;
        return data;
    }
}

// ════════════════════════════════════════════════════════════
// PAYLOAD BUILDER
// ════════════════════════════════════════════════════════════
String buildJSONPayload(const PhaseReadings &p, uint32_t ts) {
    String combined_stat;
    if (p.stateR && p.stateY && p.stateB) {
        combined_stat = "on";
    } else if (!p.stateR && !p.stateY && !p.stateB) {
        combined_stat = "off";
    } else {
        combined_stat = "partial_off";
    }

    String j = "{";
    j += "\"dt\":\"" + String(DT_CODE) + "\",";
    j += "\"fdr\":\"" + String(FEEDER_NAME) + "\",";
    j += "\"tf\":\"" + String(TRANSFORMER) + "\",";
    j += "\"timestamp\":" + String(ts) + ",";
    j += "\"stat_r\":\"" + String(p.stateR ? "ON" : "OFF") + "\",";
    j += "\"volt_r\":" + String(p.stateR ? p.voltsR : 0.0f, 1) + ",";
    j += "\"stat_y\":\"" + String(p.stateY ? "ON" : "OFF") + "\",";
    j += "\"volt_y\":" + String(p.stateY ? p.voltsY : 0.0f, 1) + ",";
    j += "\"stat_b\":\"" + String(p.stateB ? "ON" : "OFF") + "\",";
    j += "\"volt_b\":" + String(p.stateB ? p.voltsB : 0.0f, 1) + ",";
    j += "\"stat\":\"" + combined_stat + "\",";
    j += "\"val\":" + String(max(p.voltsR, max(p.voltsY, p.voltsB)), 1) + ",";
    j += "\"ccid\":\"" + sim_serial + "\"";
    j += "}";
    return j;
}

// ════════════════════════════════════════════════════════════
// ROUTE 1: WIFI HTTP POST
// ════════════════════════════════════════════════════════════
bool sendHTTPWiFi(const PhaseReadings &p, uint32_t ts) {
    if (WiFi.status() != WL_CONNECTED) return false;

    try {
        String url = String("http://") + SERVER_IP + ":" + SERVER_PORT + ENDPOINT;
        String payload = buildJSONPayload(p, ts);

        Serial.printf("[WiFi-HTTP] POST → %s\n", url.c_str());
        Serial.printf("[WiFi-HTTP] Outbound Payload: %s\n", payload.c_str());

        HTTPClient http;
        http.begin(url);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Connection", "close");
        http.setTimeout(10000);

        int code = http.POST(payload);
        String serverResponseBody = http.getString();
        http.end();

        Serial.printf("[WiFi-HTTP] Response code: %d | Body: %s\n", code, serverResponseBody.c_str());
        return (code >= 200 && code < 300);
    } catch (...) {
        Serial.println(F("[ERROR] WiFi HTTP submission crashed."));
        return false;
    }
}

// ════════════════════════════════════════════════════════════
// ROUTE 2: GSM GPRS HTTP POST
// ════════════════════════════════════════════════════════════
bool sendHTTPGSM(const PhaseReadings &p, uint32_t ts) {
    try {
        if (!gsm_connected) {
            gsm_connected = initGSM();
            if (!gsm_connected) return false;
        }

        String payload = buildJSONPayload(p, ts);
        Serial.printf("[GSM-HTTP] Outbound Payload: %s\n", payload.c_str());

        sendAT("AT+HTTPTERM", 1000);
        delay(150);

        String initResp = sendAT("AT+HTTPINIT", 1500);
        if (initResp.indexOf("ERROR") != -1) {
            sendAT("AT+HTTPTERM", 1000);
            initResp = sendAT("AT+HTTPINIT", 1500);
            if (initResp.indexOf("ERROR") != -1) {
                Serial.println(F("[GSM-HTTP] HTTP engine init failed."));
                gsm_connected = false;
                return false;
            }
        }

        sendAT("AT+HTTPPARA=\"CID\",1", 1200);
        String url = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + ENDPOINT;
        sendAT("AT+HTTPPARA=\"URL\",\"" + url + "\"", 1500);
        sendAT("AT+HTTPPARA=\"CONTENT\",\"application/json\"", 1500);

        String dataCmd = "AT+HTTPDATA=" + String(payload.length()) + ",5000";
        if (sendAT(dataCmd, 2000).indexOf("DOWNLOAD") == -1) {
            Serial.println(F("[GSM-HTTP] Modem rejected payload buffer command."));
            sendAT("AT+HTTPTERM", 1000);
            return false;
        }

        gsm.print(payload);
        delay(500);

        String actionResp = sendAT("AT+HTTPACTION=1", 15000);
        bool completed = false;
        if (actionResp.indexOf("+HTTPACTION:") != -1) {
            int fc = actionResp.indexOf(',');
            int sc = actionResp.indexOf(',', fc + 1);
            if (fc != -1 && sc != -1) {
                String code = actionResp.substring(fc + 1, sc);
                Serial.printf("[GSM-HTTP] Server response code: %s\n", code.c_str());
                if (code.startsWith("2")) completed = true;
            }
        }

        String serverResponseBody = sendAT("AT+HTTPREAD", 3000);
        Serial.printf("[GSM-HTTP] Gateway Server Response Body: %s\n", serverResponseBody.c_str());

        sendAT("AT+HTTPTERM", 1200);
        return completed;
    } catch (...) {
        Serial.println(F("[ERROR] GSM HTTP transaction encountered an error."));
        return false;
    }
}

// ════════════════════════════════════════════════════════════
// FIFO QUEUE & RETRY BACKOFF ENGINE
// ════════════════════════════════════════════════════════════
void enqueueEvent(const PhaseReadings &p, uint32_t ts) {
    try {
        File cache = LittleFS.open(CACHE_FILE, "a");
        if (!cache) {
            Serial.println(F("[CACHE] Cannot open cache file for enqueue."));
            return;
        }
        String record = String(ts) + "," +
                        (p.stateR ? "ON" : "OFF") + "," + String(p.voltsR, 1) + "," +
                        (p.stateY ? "ON" : "OFF") + "," + String(p.voltsY, 1) + "," +
                        (p.stateB ? "ON" : "OFF") + "," + String(p.voltsB, 1) + "\n";
        cache.print(record);
        cache.close();
        Serial.printf("[FIFO] Enqueued 3-Phase event: ts=%u\n", ts);

        is_retry_active = true;
        backoff_index = 0;
        last_retry_ms = 0;
    } catch (...) {
        Serial.println(F("[ERROR] Failed to write event to LittleFS FIFO queue."));
    }
}

bool hasPendingQueue() {
    if (!LittleFS.exists(CACHE_FILE)) return false;
    File cache = LittleFS.open(CACHE_FILE, "r");
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
        if (!LittleFS.exists(CACHE_FILE)) return true;

        File cache = LittleFS.open(CACHE_FILE, "r");
        if (!cache) return true;

        String tempPath = "/pearl_temp.json";
        File tmp = LittleFS.open(tempPath, "w");
        if (!tmp) {
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
                tmp.println(record);
                retainedCount++;
                allDelivered = false;
                continue;
            }

            int c1 = record.indexOf(',');
            int c2 = record.indexOf(',', c1 + 1);
            int c3 = record.indexOf(',', c2 + 1);
            int c4 = record.indexOf(',', c3 + 1);
            int c5 = record.indexOf(',', c4 + 1);
            int c6 = record.indexOf(',', c5 + 1);

            if (c1 < 0 || c2 < 0 || c3 < 0 || c4 < 0 || c5 < 0 || c6 < 0) continue;

            uint32_t ts_cached = record.substring(0, c1).toInt();
            PhaseReadings p_cached = {};
            p_cached.stateR = (record.substring(c1 + 1, c2) == "ON");
            p_cached.voltsR = record.substring(c2 + 1, c3).toFloat();
            p_cached.stateY = (record.substring(c3 + 1, c4) == "ON");
            p_cached.voltsY = record.substring(c4 + 1, c5).toFloat();
            p_cached.stateB = (record.substring(c5 + 1, c6) == "ON");
            p_cached.voltsB = record.substring(c6 + 1).toFloat();
            p_cached.readSuccess = true;

            bool sent = false;
            // Route 1: WiFi HTTP
            if (WiFi.status() == WL_CONNECTED) {
                sent = sendHTTPWiFi(p_cached, ts_cached);
            }

            // Route 2: GSM GPRS HTTP
            if (!sent) {
                if (!gsm_connected) {
                    gsm_connected = initGSM();
                }
                if (gsm_connected) {
                    sent = sendHTTPGSM(p_cached, ts_cached);
                }
            }

            if (sent) {
                flushedCount++;
                Serial.printf("[FIFO] Event Confirmed & Purged: ts=%u\n", ts_cached);
            } else {
                Serial.printf("[FIFO] Event Delivery Failed: ts=%u. Retaining in queue.\n", ts_cached);
                tmp.println(record);
                retainedCount++;
                allDelivered = false;
                deliveryBlocked = true;
            }
        }

        cache.close();
        tmp.close();

        LittleFS.remove(CACHE_FILE);
        if (retainedCount > 0) {
            LittleFS.rename(tempPath, CACHE_FILE);
        } else {
            LittleFS.remove(tempPath);
        }

        Serial.printf("[FIFO] Queue Flush Result -> Delivered: %u, Retained: %u\n", flushedCount, retainedCount);
        return allDelivered;
    } catch (...) {
        Serial.println(F("[ERROR] Error occurred while flushing FIFO storage queue."));
        return false;
    }
}

// ════════════════════════════════════════════════════════════
// WIFI INITIALISATION
// ════════════════════════════════════════════════════════════
bool initWiFi() {
    Serial.printf("[WIFI] Connecting to SSID: %s\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_TIMEOUT_MS) {
        delay(500);
        Serial.print(".");
        yield();
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }

    Serial.println(F("\n[WIFI] Connection failed."));
    return false;
}

// ════════════════════════════════════════════════════════════
// GSM INITIALISATION (GPRS Bearer)
// ════════════════════════════════════════════════════════════
bool initGSM() {
    Serial.println(F("[GSM] Initialising GPRS pipeline..."));
    setGsmRts(true);
    sendAT("AT", 500);
    sendAT("ATE1", 1000);

    String cpin = "";
    for (int i = 0; i < 3; i++) {
        cpin = sendAT("AT+CPIN?", 2000);
        if (cpin.indexOf("READY") != -1) break;
        sendAT("AT+CFUN=0", 2000);
        delay(1500);
        sendAT("AT+CFUN=1", 2000);
        delay(2000);
    }
    if (cpin.indexOf("READY") == -1) {
        Serial.println(F("[GSM] SIM card not ready."));
        return false;
    }

    sendAT("AT+CSQ", 1500);
    String creg = sendAT("AT+CREG?", 2000);
    if (creg.indexOf(",1") == -1 && creg.indexOf(",5") == -1) {
        Serial.println(F("[GSM] Not registered on network."));
        return false;
    }

    sendAT("AT+CIPSHUT", 3000);
    delay(1000);
    yield();

    sendAT("AT+CGATT=1", 3000);
    sendAT("AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\"", 2000);
    sendAT("AT+SAPBR=3,1,\"APN\",\"" + String(CURRENT_APN) + "\"", 2000);
    sendAT("AT+SAPBR=0,1", 2000);
    delay(500);
    sendAT("AT+SAPBR=1,1", 5000);

    String sapbrCheck = sendAT("AT+SAPBR=2,1", 3000);
    if (sapbrCheck.indexOf("0.0.0.0") != -1 || sapbrCheck.indexOf("ERROR") != -1) {
        Serial.println(F("[GSM] Bearer IP allocation failed."));
        return false;
    }

    Serial.println(F("[GSM] GPRS bearer active."));
    return true;
}

// ════════════════════════════════════════════════════════════
// GSM RESET MONITOR
// ════════════════════════════════════════════════════════════
void monitorForReset() {
    if (!gsm.available()) return;
    String buf = "";
    while (gsm.available()) buf += (char)gsm.read();

    if ((buf.indexOf("RDY") != -1 || buf.indexOf("+CFUN: 1") != -1 || buf.indexOf("Call Ready") != -1) &&
        (millis() - last_reset_handled > 10000UL)) {
        Serial.println(F("[GSM] Modem reset detected — reinitialising."));
        last_reset_handled = millis();
        gsm_connected = false;
        delay(8000);
        for (int i = 0; i < 3 && !gsm_connected; i++) {
            gsm_connected = initGSM();
            if (!gsm_connected) delay(5000UL * (i + 1));
        }
        if (gsm_connected) {
            fetchSIMIdentity();
            syncNetworkTime();
            if (hasPendingQueue()) {
                is_retry_active = true;
                backoff_index = 0;
                last_retry_ms = 0;
            }
        }
    }
}

// ════════════════════════════════════════════════════════════
// SIM IDENTITY
// ════════════════════════════════════════════════════════════
void fetchSIMIdentity() {
    sendAT("AT+CNUM", 1500);
    String resp = sendAT("AT+CCID", 1000);
    sim_serial = cleanResponse(resp);
    if (sim_serial.length() < 5 || sim_serial.indexOf("ERROR") != -1) sim_serial = "UNKNOWN";
    Serial.printf("[SIM] Serial: %s\n", sim_serial.c_str());
}

// ════════════════════════════════════════════════════════════
// AT COMMAND ENGINE
// ════════════════════════════════════════════════════════════
void setGsmRts(bool enabled) {
    digitalWrite(GSM_RTS_PIN, enabled ? LOW : HIGH);
}

String sendAT(String cmd, int timeout_ms) {
    int fc = 0;
    while (gsm.available() && fc++ < 128) {
        gsm.read();
        yield();
    }

    Serial.print("→ AT: ");
    Serial.println(cmd);
    gsm.println(cmd);

    String response = "";
    unsigned long t = millis();
    while (millis() - t < (unsigned long)timeout_ms) {
        while (gsm.available()) {
            char c = (char)gsm.read();
            response += c;
            t = millis();
        }
        yield();
    }
    return response;
}

String cleanResponse(String resp) {
    resp.replace("AT+CCID", "");
    resp.replace("AT+CNUM", "");
    resp.replace("OK", "");
    resp.replace("Call Ready", "");
    resp.replace("\r", "");
    resp.replace("\n", "");
    resp.trim();
    return resp;
}