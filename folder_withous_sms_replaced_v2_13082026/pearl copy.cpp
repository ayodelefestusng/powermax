#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <SoftwareSerial.h>
#include <Ds1302.h>
#include <LittleFS.h>


// ==========================================
// HARDWARE PIN MAPPINGS (ESP8266 / D1 MINI)
// ==========================================
#define SENSOR_ADC_PIN    A0

// GSM SIM900/SIM800 Pins
#define GSM_RX_PIN        D5  // ESP RX  <-- SIM TX
#define GSM_TX_PIN        D6  // ESP TX  --> SIM RX
#define GSM_RTS_PIN       D8  // SIM RTS Control

// DS1302 RTC Pins
#define RTC_CLK_PIN       D2  // Clock
#define RTC_DAT_PIN       D1  // Data
#define RTC_RST_PIN       D7  // Reset / Chip Enable

// ==========================================
// CALIBRATED HARDWARE THRESHOLDS
// ==========================================
#define AC_SAMPLE_WINDOW_MS  100

// Measured A0 levels: ON is approximately 338 and OFF is approximately 9.
// Hysteresis thresholds: Turn ON at 100, stay ON while > 50
// ON threshold must be HIGHER than OFF threshold for proper hysteresis.
const uint16_t RAW_THRESHOLD_ON  = 100;  // Transition from OFF to ON requires reaching this level
const uint16_t RAW_THRESHOLD_OFF = 50;   // Once ON, must drop below this to transition to OFF

// ==========================================
// NETWORK & SERVER CONFIGURATION
// ==========================================
const int LOCAL_TIME_OFFSET_SECONDS = 3600; // UTC+1 (Nigeria / West Africa Time)

const char* SERVER_IP          = "24.144.119.35";
const char* SERVER_PORT        = "8000";
const char* CURRENT_APN        = "9mobile";

// WiFi Credentials
const char* wifi_ssid          = "V40 Lite";
const char* wifi_password      = "@Ajibandele612";
const unsigned long WIFI_TIMEOUT_MS = 15000UL;

// Static Node Metadata
const char* FEEDER_NAME        = "Oke Ira";
const char* TRANSFORMER_NAME   = "Mechanic DT";
const char* USER_LATITUDE      = "6.5230";
const char* USER_LONGITUDE     = "3.3420";

// ==========================================
// STORAGE CONSTANTS
// ==========================================
#define CACHE_FILE_PATH       "/telemetry_cache.json"

// ==========================================
// GLOBAL HARDWARE OBJECTS & RUNTIME STATE
// ==========================================
SoftwareSerial gsmSerialPort(GSM_RX_PIN, GSM_TX_PIN);
Ds1302 rtc(RTC_RST_PIN, RTC_CLK_PIN, RTC_DAT_PIN);

bool lastPowerStatus = false;
bool isWifiConnected = false;
bool isGsmConnected  = false;
bool isRtcAvailable  = false;
String simSerial     = "UNKNOWN";

uint32_t networkBaseEpoch = 0;
unsigned long epochSyncMillis = 0;
unsigned long lastResetHandled = 0;

// ==========================================
// FIFO QUEUE & RETRY BACKOFF CONFIGURATION
// ==========================================
const uint32_t BACKOFF_INTERVALS_MS[] = {3000, 10000, 30000, 60000, 120000};
const uint8_t MAX_BACKOFF_INDEX       = 4;

uint8_t backoffIndex         = 0;
unsigned long lastRetryAttemptMs = 0;
bool isRetryActive           = false;

// Forward Declarations
bool sendTelemetryHttpWiFi(const String& status, uint32_t timestamp, uint16_t peakVal);
bool sendTelemetryHttpGPRS(const String& status, uint32_t timestamp, uint16_t peakVal);
bool initGSM();
bool flushQueue();
bool readPowerStatus(uint16_t &outPkToPk, float &outVoltage, bool previousState);

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

bool isRtcDateTimeReasonable(const Ds1302::DateTime& dt) {
    if (isRtcDateTimeInvalid(dt)) {
        return false;
    }

    if (dt.year < 24 || dt.year > 99) {
        return false;
    }
    if (dt.month < 1 || dt.month > 12) {
        return false;
    }
    if (dt.day < 1 || dt.day > 31) {
        return false;
    }
    if (dt.hour > 23 || dt.minute > 59 || dt.second > 59) {
        return false;
    }

    return true;
}

bool waitForNtpSync(uint32_t timeoutMs) {
    try {
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
    } catch (...) {
        logError("Exception in waitForNtpSync.");
        return false;
    }
}

bool syncRtcFromNtp() {
    try {
        if (!isRtcAvailable) return false;

        time_t now = time(nullptr);
        if (now <= 1700000000UL) return false;

        struct tm* timeInfo = localtime(&now);
        if (timeInfo == nullptr) return false;

        Ds1302::DateTime rtcNow = {0};
        rtcNow.year   = timeInfo->tm_year % 100;
        rtcNow.month  = timeInfo->tm_mon + 1;
        rtcNow.day    = timeInfo->tm_mday;
        rtcNow.hour   = timeInfo->tm_hour;
        rtcNow.minute = timeInfo->tm_min;
        rtcNow.second = timeInfo->tm_sec;
        rtcNow.dow    = timeInfo->tm_wday + 1;

        if (!isRtcDateTimeReasonable(rtcNow)) {
            logError("Refusing to write invalid NTP time to DS1302.");
            return false;
        }

        rtc.setDateTime(&rtcNow);

        Ds1302::DateTime validated = {0};
        rtc.getDateTime(&validated);
        if (!isRtcDateTimeReasonable(validated)) {
            logError("RTC still invalid after NTP write. Check RTC power or wiring.");
            isRtcAvailable = false;
            return false;
        }

        logInfo("RTC synchronized from NTP time: " + String(rtcNow.year) + "/" + String(rtcNow.month) + "/" + String(rtcNow.day));
        isRtcAvailable = true;
        return true;
    } catch (...) {
        logError("Exception in syncRtcFromNtp.");
        return false;
    }
}

void printCurrentRtcDateTime() {
    try {
        if (!isRtcAvailable) {
            Serial.println("[RTC] unavailable");
            return;
        }

        Ds1302::DateTime dt;
        rtc.getDateTime(&dt);
        if (!isRtcDateTimeReasonable(dt)) {
            Serial.println("[RTC] invalid / unset / stale - attempting recovery...");
            delay(50);
            rtc.getDateTime(&dt);
            if (!isRtcDateTimeReasonable(dt)) {
                logError("RTC still invalid after retry. Check: 1) Battery backup, 2) Pin connections, 3) RTC wiring.");
                isRtcAvailable = false;
                return;
            }
        }

        Serial.printf("[RTC] 20%02d-%02d-%02d %02d:%02d:%02d\n",
                      dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    } catch (...) {
        logError("Exception printing RTC date time.");
    }
}

// ==========================================
// GSM RTS & AT COMMUNICATION ENGINE
// ==========================================
void setGsmRts(bool enabled) {
    digitalWrite(GSM_RTS_PIN, enabled ? LOW : HIGH);
}

String sendAT(const String& cmd, uint32_t timeoutMs) {
    try {
        uint8_t clearCount = 0;
        while (gsmSerialPort.available() && clearCount < 128) {
            gsmSerialPort.read();
            clearCount++;
            yield();
        }

        logInfo("Outbound AT -> " + cmd);
        gsmSerialPort.println(cmd);

        String response = "";
        uint32_t t = millis();

        while (millis() - t < timeoutMs) {
            while (gsmSerialPort.available()) {
                char c = (char)gsmSerialPort.read();
                response += c;
                t = millis();
            }
            yield();
            ESP.wdtFeed();
        }

        String trimmedResp = response;
        trimmedResp.trim();
        if (trimmedResp.length() > 0) {
            logInfo("Inbound AT Response <- " + trimmedResp);
        } else {
            logInfo("Inbound AT Response <- [NO RESPONSE / TIMEOUT]");
        }

        return response;
    } catch (...) {
        logError("Exception during sendAT execution.");
        return "";
    }
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

// ==========================================
// WIFI & SIM NETWORK INIT
// ==========================================
bool initWiFi() {
    try {
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
    } catch (...) {
        logError("Exception in initWiFi.");
        return false;
    }
}

void fetchSIMIdentity() {
    try {
        sendAT("AT+CNUM", 1500);
        String response = sendAT("AT+CCID", 1000);
        simSerial = cleanResponse(response);
        if (simSerial.length() < 5 || simSerial.indexOf("ERROR") != -1) {
            simSerial = "UNKNOWN";
        }
        logInfo("SIM Serial Number Identified: " + simSerial);
    } catch (...) {
        logError("Exception fetching SIM identity.");
        simSerial = "UNKNOWN";
    }
}

bool initGSM() {
    try {
        logInfo("Synchronizing GSM interface configurations...");
        setGsmRts(true);
        sendAT("AT", 500);
        sendAT("ATE1", 1000);

        String cpin = "";
        for (uint8_t simRetry = 0; simRetry < 3; simRetry++) {
            cpin = sendAT("AT+CPIN?", 2000);
            if (cpin.indexOf("READY") != -1) break;
            sendAT("AT+CFUN=0", 2000);
            delay(1500);
            sendAT("AT+CFUN=1", 2000);
            delay(2000);
        }

        if (cpin.indexOf("READY") == -1) {
            logError("Critical: SIM Card dropped or unpowered.");
            return false;
        }

        sendAT("AT+CSQ", 1500);
        String creg = sendAT("AT+CREG?", 2000);
        if (creg.indexOf(",1") == -1 && creg.indexOf(",5") == -1) {
            logError("GSM Network registration failed.");
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
            logError("Bearer context IP allocation returned invalid structure.");
            return false;
        }

        logInfo("Production GSM GPRS IP Pipeline Active.");
        return true;
    } catch (...) {
        logError("Exception during GSM initialization.");
        return false;
    }
}

// ==========================================
// TIME & RTC ENGINE
// ==========================================
uint32_t ds1302ToEpoch(const Ds1302::DateTime &dt) {
    try {
        if (!isRtcDateTimeReasonable(dt)) {
            logError("Rejecting invalid DS1302 timestamp for epoch conversion.");
            return 0;
        }

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
    } catch (...) {
        logError("Exception in ds1302ToEpoch.");
        return 0;
    }
}

void syncNetworkTime() {
    try {
        logInfo("Querying network baseline time parameters (AT+CCLK?)...");
        String resp = sendAT("AT+CCLK?", 2000);

        int idx = resp.indexOf("+CCLK: \"");
        if (idx != -1) {
            uint16_t startIdx = idx + 8;
            if (resp.length() >= (uint32_t)(startIdx + 17)) {
                String rtcDate = resp.substring(startIdx, startIdx + 8);
                String rtcTime = resp.substring(startIdx + 9, startIdx + 17);

                uint16_t year   = 2000 + rtcDate.substring(0, 2).toInt();
                uint8_t month   = rtcDate.substring(3, 5).toInt();
                uint8_t day     = rtcDate.substring(6, 8).toInt();
                uint8_t hour    = rtcTime.substring(0, 2).toInt();
                uint8_t minute  = rtcTime.substring(3, 5).toInt();
                uint8_t second  = rtcTime.substring(6, 8).toInt();

                if (year >= 2026 && month > 0 && day > 0) {
                    if (isRtcAvailable) {
                        Ds1302::DateTime netTime = {0};
                        netTime.year   = year - 2000;
                        netTime.month  = month;
                        netTime.day    = day;
                        netTime.hour   = hour;
                        netTime.minute = minute;
                        netTime.second = second;
                        rtc.setDateTime(&netTime);
                        logInfo("Synchronized hardware DS1302 with Cell Tower time.");
                    }

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

                    networkBaseEpoch = (numDays * 86400UL) + (hour * 3600UL) + (minute * 60UL) + second;
                    epochSyncMillis = millis();
                    return;
                }
            }
        }
        logInfo("Network clock unavailable. Retaining current DS1302 settings.");
    } catch (...) {
        logError("Exception encountered during network time sync.");
    }
}

uint32_t getCurrentTimestamp() {
    try {
        if (isRtcAvailable) {
            Ds1302::DateTime dt;
            rtc.getDateTime(&dt);
            if (isRtcDateTimeReasonable(dt)) {
                return ds1302ToEpoch(dt);
            }

            logError("RTC timestamp was invalid; falling back to network or boot time.");
            isRtcAvailable = false;
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
// RAW ANALOG SAMPLING & HYSTERESIS
// ==========================================
uint16_t getAverageAdcValue() {
    try {
        uint32_t totalRead = 0;
        uint16_t sampleCount = 0;
        uint32_t startMs = millis();

        // Average the absolute A0 level over the sample window.
        while (millis() - startMs < AC_SAMPLE_WINDOW_MS) {
            totalRead += analogRead(SENSOR_ADC_PIN);
            sampleCount++;
            yield();
        }

        return sampleCount > 0 ? (uint16_t)(totalRead / sampleCount) : 0;
    } catch (...) {
        logError("Exception sampling raw A0 level.");
        return 0;
    }
}

bool evaluateState(uint16_t rawValue, bool state) {
    try {
        if (state) {
            return rawValue >= RAW_THRESHOLD_OFF;
        } else {
            return rawValue >= RAW_THRESHOLD_ON;
        }
    } catch (...) {
        return false;
    }
}

bool readPowerStatus(uint16_t &outPkToPk, float &outVoltage, bool previousState) {
    try {
        outPkToPk = getAverageAdcValue();
        outVoltage = (float)outPkToPk;
        return evaluateState(outPkToPk, previousState);
    } catch (...) {
        logError("Exception reading power analog status.");
        return previousState;
    }
}

// ==========================================
// DISPATCH PIPELINES: WIFI & GPRS HTTP
// ==========================================
bool sendTelemetryHttpWiFi(const String& status, uint32_t timestamp, uint16_t peakVal) {
    if (WiFi.status() != WL_CONNECTED) return false;

    try {
        String payload = "{";
        payload += "\"stat\":\"" + status + "\",";
        payload += "\"timestamp\":" + String(timestamp) + ",";
        payload += "\"val\":" + String(peakVal) + ",";
        payload += "\"fdr\":\"" + String(FEEDER_NAME) + "\",";
        payload += "\"tf\":\"" + String(TRANSFORMER_NAME) + "\",";
        payload += "\"ccid\":\"" + simSerial + "\"";
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
        return (httpCode >= 200 && httpCode < 300);
    } catch (...) {
        logError("Exception during sendTelemetryHttpWiFi execution.");
        return false;
    }
}

bool sendTelemetryHttpGPRS(const String& status, uint32_t timestamp, uint16_t peakVal) {
    try {
        if (!isGsmConnected) {
            isGsmConnected = initGSM();
            if (!isGsmConnected) return false;
        }

        // Validate GPRS Bearer status before HTTP operations
        String bearerStatus = sendAT("AT+SAPBR=2,1", 2000);
        if (bearerStatus.indexOf("0.0.0.0") != -1 || bearerStatus.indexOf("ERROR") != -1) {
            logError("GPRS Bearer lost. Re-initializing GSM interface...");
            isGsmConnected = initGSM();
            if (!isGsmConnected) return false;
        }

        String payload = "{";
        payload += "\"stat\":\"" + status + "\",";
        payload += "\"timestamp\":" + String(timestamp) + ",";
        payload += "\"val\":" + String(peakVal) + ",";
        payload += "\"fdr\":\"" + String(FEEDER_NAME) + "\",";
        payload += "\"tf\":\"" + String(TRANSFORMER_NAME) + "\",";
        payload += "\"ccid\":\"" + simSerial + "\"";
        payload += "}";

        logInfo("[GSM-HTTP] Outbound Payload -> " + payload);

        sendAT("AT+HTTPTERM", 1000);
        delay(150);
        String initResp = sendAT("AT+HTTPINIT", 1500);
        if (initResp.indexOf("ERROR") != -1) {
            sendAT("AT+HTTPTERM", 1000);
            initResp = sendAT("AT+HTTPINIT", 1500);
            if (initResp.indexOf("ERROR") != -1) {
                logError("HTTP engine initialization crashed.");
                isGsmConnected = false;
                return false;
            }
        }

        sendAT("AT+HTTPPARA=\"CID\",1", 1200);
        String url = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/power-tracker-gateway/";
        sendAT("AT+HTTPPARA=\"URL\",\"" + url + "\"", 1500);
        sendAT("AT+HTTPPARA=\"CONTENT\",\"application/json\"", 1500);

        String dataCmd = "AT+HTTPDATA=" + String(payload.length()) + ",5000";
        if (sendAT(dataCmd, 2000).indexOf("DOWNLOAD") == -1) {
            logError("Modem failed to enter raw payload download stream mode.");
            sendAT("AT+HTTPTERM", 1000);
            return false;
        }

        gsmSerialPort.print(payload);
        delay(500);

        String actionResp = sendAT("AT+HTTPACTION=1", 15000);
        bool completed = false;

        if (actionResp.indexOf("+HTTPACTION:") != -1) {
            int firstComma = actionResp.indexOf(',');
            int secondComma = actionResp.indexOf(',', firstComma + 1);
            if (firstComma != -1 && secondComma != -1) {
                String responseCode = actionResp.substring(firstComma + 1, secondComma);
                logInfo("[GSM-HTTP] Gateway Server Response Code: " + responseCode);
                if (responseCode.startsWith("2")) {
                    completed = true;
                }
            }
        }

        String serverResponseBody = sendAT("AT+HTTPREAD", 3000);
        logInfo("[GSM-HTTP] Gateway Server Response Body: " + serverResponseBody);

        sendAT("AT+HTTPTERM", 1200);
        return completed;
    } catch (...) {
        logError("Exception during sendTelemetryHttpGPRS execution.");
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
        logInfo("FIFO Queue Enqueued: Status=" + status + ", Timestamp=" + String(timestamp) + ", Pk-Pk=" + String(peakVal));

        isRetryActive = true;
        backoffIndex = 0;
        lastRetryAttemptMs = 0;
    } catch (...) {
        logError("Exception while enqueuing event into FIFO queue.");
    }
}

bool hasPendingQueue() {
    try {
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
    } catch (...) {
        logError("Exception checking pending queue.");
        return false;
    }
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

                if (!sent) {
                    if (!isGsmConnected) {
                        isGsmConnected = initGSM();
                    }
                    if (isGsmConnected) {
                        sent = sendTelemetryHttpGPRS(status, timestamp, peakVal);
                    }
                }

                if (sent) {
                    flushedCount++;
                    logInfo("FIFO Event Delivery Confirmed: Timestamp=" + String(timestamp) + " State=" + status);
                } else {
                    logError("FIFO Event Delivery Failed: Timestamp=" + String(timestamp) + ". Retaining in queue.");
                    tempCache.println(record);
                    retainedCount++;
                    allDelivered = false;
                    deliveryBlocked = true;
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

void monitorForReset() {
    try {
        if (gsmSerialPort.available()) {
            String buffer = "";
            while (gsmSerialPort.available()) {
                buffer += (char)gsmSerialPort.read();
            }
            if ((buffer.indexOf("RDY") != -1 || buffer.indexOf("+CFUN: 1") != -1 || buffer.indexOf("Call Ready") != -1) && (millis() - lastResetHandled > 10000UL)) {
                logError("Hardware transceiver reset detected!");
                lastResetHandled = millis();
                isGsmConnected = false;
                delay(8000);
                uint8_t attempts = 0;
                while (!isGsmConnected && attempts < 3) {
                    attempts++;
                    isGsmConnected = initGSM();
                    if (!isGsmConnected) delay(5000UL * attempts);
                }
                if (isGsmConnected) {
                    fetchSIMIdentity();
                    syncNetworkTime();
                    if (hasPendingQueue()) {
                        isRetryActive = true;
                        backoffIndex = 0;
                        lastRetryAttemptMs = 0;
                    }
                }
            }
        }
    } catch (...) {
        logError("Exception in monitorForReset listener.");
    }
}

// ==========================================
// ARDUINO MAIN ENTRY POINTS
// ==========================================
void setup() {
    try {
        Serial.begin(115200);

        pinMode(GSM_RTS_PIN, OUTPUT);
        setGsmRts(true);
        gsmSerialPort.begin(4800);

        logInfo("Booting Feeder Power Monitor Node (" + String(FEEDER_NAME) + ")...");

        if (!LittleFS.begin()) {
            logError("LittleFS mount failed! Persistent FIFO queue unavailable.");
        }

        rtc.init();
        Ds1302::DateTime now;
        rtc.getDateTime(&now);

        if (!isRtcDateTimeReasonable(now)) {
            Ds1302::DateTime bootstrap = {0};
            bootstrap.year = 26;      // 2026
            bootstrap.month = 8;
            bootstrap.day = 31;
            bootstrap.hour = 0;
            bootstrap.minute = 0;
            bootstrap.second = 0;
            bootstrap.dow = 7;        // Sunday
            rtc.setDateTime(&bootstrap);
            rtc.getDateTime(&now);
            logInfo("DS1302 RTC was invalid or uninitialized. Set bootstrap time: 2026-08-31 00:00:00");
        }

        if (!isRtcDateTimeReasonable(now)) {
            logError("DS1302 RTC remains invalid after bootstrap write. Treating RTC as unavailable.");
            isRtcAvailable = false;
        } else {
            logInfo("DS1302 RTC initialized with time: " + String(now.year) + "/" + String(now.month) + "/" + String(now.day));
            isRtcAvailable = true;
        }

        delay(1000);

        gsmSerialPort.println("AT+IPR=4800");
        delay(500);
        while (gsmSerialPort.available()) {
            gsmSerialPort.read();
        }

        sendAT("AT+CLTS=1", 1000);
        sendAT("AT&W", 1000);

        isWifiConnected = initWiFi();

        if (isWifiConnected) {
            configTime(LOCAL_TIME_OFFSET_SECONDS, 0, "pool.ntp.org", "time.google.com");
            logInfo("NTP time sync requested via WiFi for UTC+1 local time.");
            if (waitForNtpSync(15000)) {
                syncRtcFromNtp();
            } else {
                logError("NTP time sync did not complete within timeout.");
            }
        } else {
            logInfo("WiFi unavailable — attempting GSM GPRS pipeline...");
            uint8_t initAttempts = 0;
            while (!isGsmConnected) {
                initAttempts++;
                isGsmConnected = initGSM();
                if (!isGsmConnected) {
                    logError("Base GPRS Pipeline Failed (Attempt " + String(initAttempts) + "). Retrying...");
                    delay(5000);
                }
                if (initAttempts >= 3 && !isGsmConnected) break;
            }

            if (isGsmConnected) {
                fetchSIMIdentity();
                syncNetworkTime();
            }
        }

        // Initial boot voltage and power status check
        uint16_t initPkToPk = 0;
        float initVoltage = 0.0f;
        bool initialState = readPowerStatus(initPkToPk, initVoltage, lastPowerStatus);
        lastPowerStatus = initialState;
        uint32_t initTs = getCurrentTimestamp();

        logInfo("Initial Power Status -> " + String(lastPowerStatus ? "ON" : "OFF") + 
                " | Voltage: " + String(initVoltage, 1) + " V | Pk-Pk: " + String(initPkToPk));
        enqueueEvent(lastPowerStatus ? "on" : "off", initTs, initPkToPk);

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

        // 1. Power status change detection using AC peak-to-peak voltage calculation
        uint16_t currentPkToPk = 0;
        float currentVoltage = 0.0f;
        bool previousState = lastPowerStatus;
        bool currentStatus = readPowerStatus(currentPkToPk, currentVoltage, previousState);

        if (currentStatus != previousState) {
            delay(1200); // Debounce confirmation window
            bool confirmedStatus = readPowerStatus(currentPkToPk, currentVoltage, lastPowerStatus);
            if (confirmedStatus != previousState) {
                lastPowerStatus = confirmedStatus;
                uint32_t eventTime = getCurrentTimestamp();
                logInfo("Power state transition detected! New State: " + String(lastPowerStatus ? "ON" : "OFF") + 
                        " | Voltage: " + String(currentVoltage, 1) + " V | Pk-Pk: " + String(currentPkToPk));
                
                // Enqueue event
                enqueueEvent(lastPowerStatus ? "on" : "off", eventTime, currentPkToPk);
                
                // Trigger immediate flush attempt on power state change
                if (flushQueue()) {
                    isRetryActive = false;
                    backoffIndex = 0;
                } else {
                    isRetryActive = true;
                    backoffIndex = 0;
                    lastRetryAttemptMs = millis();
                }
            }
        }

        // 2. Monitor WiFi status transitions
        if (!isWifiConnected && WiFi.status() == WL_CONNECTED) {
            isWifiConnected = true;
            logInfo("WiFi reconnected.");
            if (hasPendingQueue()) {
                isRetryActive = true;
                backoffIndex = 0;
                lastRetryAttemptMs = 0;
            }
        } else if (isWifiConnected && WiFi.status() != WL_CONNECTED) {
            isWifiConnected = false;
            logError("WiFi connection lost.");
        }

        // 3. FIFO Queue Retry & Exponential Backoff Engine
        if (isRetryActive) {
            if (millis() - lastRetryAttemptMs >= BACKOFF_INTERVALS_MS[backoffIndex]) {
                lastRetryAttemptMs = millis();
                logInfo("Backoff retry timer triggered. Checking network & flushing queue...");

                bool flushed = flushQueue();
                if (flushed) {
                    logInfo("All queued events confirmed delivered and purged.");
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

        if (!isWifiConnected) {
            monitorForReset();
        }

        delay(100);
        yield();
    } catch (...) {
        logError("Exception inside main system loop.");
    }
}