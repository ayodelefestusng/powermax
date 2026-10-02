#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <SoftwareSerial.h>
#include <Ds1302.h>
#include <LittleFS.h>

// ==========================================
// ==========================================
#define SENSOR_ADC_PIN     A0

// GSM SIM900/SIM800 Pins
#define GSM_RX_PIN        D5  // ESP RX  <-- SIM TX
#define GSM_TX_PIN        D6  // ESP TX  --> SIM RX
#define GSM_RTS_PIN       D8  // SIM RTS Control

// DS1302 RTC Pins
#define RTC_CLK_PIN       D2  // Clock
#define RTC_DAT_PIN       D1  // Data
#define RTC_RST_PIN       D7  // Reset / Chip Enable

// ==========================================
// CALIBRATED HARDWARE THRESHOLDS & FILTERING
// ==========================================
const int ADC_OFF_BASE        = 60;   
const int ADC_THRESHOLD       = 90;  
const int CONSECUTIVE_SAMPLES = 3;   

#define AC_SAMPLE_WINDOW_MS   100    

// ==========================================
// NETWORK & SERVER CONFIGURATION
// ==========================================
const int LOCAL_TIME_OFFSET_SECONDS = 3600; // UTC+1

const char* SERVER_IP         = "24.144.119.35";
const char* SERVER_PORT       = "8000";
const char* CURRENT_APN       = "web.gprs.mtnnigeria.net";

// WiFi Credentials
const char* wifi_ssid         = "V40 Lite";
const char* wifi_password     = "@Ajibandele612";
const unsigned long WIFI_TIMEOUT_MS = 15000UL;

// Static Node Metadata
const char* FEEDER_NAME       = "Testing ";
const char* TRANSFORMER_NAME  = "Testing  DT";
const char* USER_LATITUDE     = "6.5230";
const char* USER_LONGITUDE    = "3.3420";

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

// FIFO QUEUE & RETRY BACKOFF CONFIGURATION (3s, 10s, 20s, 30s, 60s)
const uint32_t BACKOFF_INTERVALS_MS[] = {3000, 10000, 20000, 30000, 60000};
const uint8_t MAX_BACKOFF_INDEX       = 4;

uint8_t backoffIndex         = 0;
unsigned long lastRetryAttemptMs = 0;
bool isRetryActive           = false;

// Forward Declarations
bool sendTelemetryHttpWiFi(const String& status, uint32_t timestamp, uint16_t peakVal);
bool sendTelemetryHttpGPRS(const String& status, uint32_t timestamp, uint16_t peakVal);
bool initGSM();
bool flushQueue();
bool transmitEventNow(const String& status, uint32_t timestamp, uint16_t peakVal);

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

        rtc.setDateTime(&rtcNow);
        logInfo("RTC synchronized from NTP time.");
        return true;
    } catch (...) {
        logError("Exception in syncRtcFromNtp.");
        return false;
    }
}

void printCurrentRtcDateTime() {
    try {
        if (!isRtcAvailable) return;
        Ds1302::DateTime dt;
        rtc.getDateTime(&dt);
        if (isRtcDateTimeInvalid(dt)) return;

        Serial.printf("[RTC] 20%02d-%02d-%02d %02d:%02d:%02d\n",
                      dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    } catch (...) {
        logError("Exception in printCurrentRtcDateTime.");
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

        response.trim();
        if (response.length() > 0) {
            logInfo("Inbound AT Response <- " + response);
        } else {
            logInfo("Inbound AT Response <- [TIMEOUT]");
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
            yield();
        }

        if (WiFi.status() == WL_CONNECTED) {
            logInfo("WiFi Connected! IP Address: " + WiFi.localIP().toString());
            return true;
        }

        logError("WiFi Connection failed.");
        return false;
    } catch (...) {
        logError("Exception during initWiFi.");
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
    } catch (...) {
        logError("Exception fetching SIM identity.");
        simSerial = "UNKNOWN";
    }
}

bool initGSM() {
    try {
        logInfo("Initializing GSM interface...");
        setGsmRts(true);
        sendAT("AT", 500);
        sendAT("ATE1", 1000);

        String cpin = "";
        for (uint8_t simRetry = 0; simRetry < 3; simRetry++) {
            cpin = sendAT("AT+CPIN?", 2000);
            if (cpin.indexOf("READY") != -1) break;
            sendAT("AT+CFUN=0", 2000);
            delay(1000);
            sendAT("AT+CFUN=1", 2000);
            delay(1500);
        }

        if (cpin.indexOf("READY") == -1) {
            logError("SIM Card not ready.");
            return false;
        }

        String creg = sendAT("AT+CREG?", 2000);
        if (creg.indexOf(",1") == -1 && creg.indexOf(",5") == -1) {
            logError("GSM Network registration failed.");
            return false;
        }

        sendAT("AT+CGATT=1", 3000);
        sendAT("AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\"", 2000);
        sendAT("AT+SAPBR=3,1,\"APN\",\"" + String(CURRENT_APN) + "\"", 2000);

        // Check if Bearer 1 is already connected to avoid 'ERROR' response
        String sapbrCheck = sendAT("AT+SAPBR=2,1", 3000);
        if (sapbrCheck.indexOf("0.0.0.0") != -1 || sapbrCheck.indexOf("ERROR") != -1) {
            sendAT("AT+SAPBR=1,1", 4000);
        } else {
            logInfo("GPRS Bearer already active.");
        }

        logInfo("GSM GPRS Pipeline Active.");
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

                if (year >= 2026 && month > 0 && day > 0 && isRtcAvailable) {
                    Ds1302::DateTime netTime = {(uint8_t)(year - 2000), month, day, hour, minute, second, 1};
                    rtc.setDateTime(&netTime);
                }
            }
        }
    } catch (...) {
        logError("Exception during network time sync.");
    }
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
        if (now > 1700000000UL) return (uint32_t)now;
        return (uint32_t)(millis() / 1000UL);
    } catch (...) {
        return (uint32_t)(millis() / 1000UL);
    }
}

// ==========================================
// ANALOG SAMPLING & HYSTERESIS
// ==========================================
uint16_t getFilteredAnalogPeak() {
    try {
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
    } catch (...) {
        return 0;
    }
}

bool readPowerStatus(bool previousState) {
    try {
        uint16_t peakVal = getFilteredAnalogPeak();
        if (previousState) {
            return peakVal > ADC_OFF_BASE;
        }
        return peakVal > ADC_THRESHOLD;
    } catch (...) {
        return previousState;
    }
}

// ==========================================
// DISPATCH PIPELINES (WIFI FIRST, THEN GSM)
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
        WiFiClient client;
        HTTPClient http;
        if (!http.begin(client, url)) return false;

        http.addHeader("Content-Type", "application/json");
        http.setTimeout(5000); // Shorter timeout for faster failover/response

        int httpCode = http.POST(payload);
        http.end();

        return (httpCode >= 200 && httpCode < 300);
    } catch (...) {
        logError("Exception in sendTelemetryHttpWiFi.");
        return false;
    }
}

bool sendTelemetryHttpGPRS(const String& status, uint32_t timestamp, uint16_t peakVal) {
    try {
        if (!isGsmConnected) {
            isGsmConnected = initGSM();
            if (!isGsmConnected) return false;
        }

        // Force terminate any lingering HTTP sessions to prevent state lockup
        sendAT("AT+HTTPTERM", 1000);
        delay(500);

        String payload = "{";
        payload += "\"stat\":\"" + status + "\",";
        payload += "\"timestamp\":" + String(timestamp) + ",";
        payload += "\"val\":" + String(peakVal) + ",";
        payload += "\"fdr\":\"" + String(FEEDER_NAME) + "\",";
        payload += "\"tf\":\"" + String(TRANSFORMER_NAME) + "\",";
        payload += "\"ccid\":\"" + simSerial + "\"";
        payload += "}";

        if (sendAT("AT+HTTPINIT", 2000).indexOf("OK") == -1) {
            // If HTTPINIT fails, reset GPRS bearer profile once and retry
            sendAT("AT+SAPBR=0,1", 2000);
            delay(1000);
            sendAT("AT+SAPBR=1,1", 4000);
            if (sendAT("AT+HTTPINIT", 2000).indexOf("OK") == -1) {
                return false;
            }
        }

        sendAT("AT+HTTPPARA=\"CID\",1", 1000);
        String url = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/power-tracker-gateway/";
        sendAT("AT+HTTPPARA=\"URL\",\"" + url + "\"", 1000);
        sendAT("AT+HTTPPARA=\"CONTENT\",\"application/json\"", 1000);

        String dataCmd = "AT+HTTPDATA=" + String(payload.length()) + ",3000";
        if (sendAT(dataCmd, 1500).indexOf("DOWNLOAD") == -1) {
            sendAT("AT+HTTPTERM", 500);
            return false;
        }

        gsmSerialPort.print(payload);
        delay(200);

        String actionResp = sendAT("AT+HTTPACTION=1", 10000);
        bool completed = false;

        if (actionResp.indexOf("+HTTPACTION:") != -1) {
            int firstComma = actionResp.indexOf(',');
            int secondComma = actionResp.indexOf(',', firstComma + 1);
            if (firstComma != -1 && secondComma != -1) {
                String responseCode = actionResp.substring(firstComma + 1, secondComma);
                if (responseCode.startsWith("2")) {
                    completed = true;
                }
            }
        }

        sendAT("AT+HTTPTERM", 1000);
        return completed;
    } catch (...) {
        logError("Exception in sendTelemetryHttpGPRS.");
        sendAT("AT+HTTPTERM", 500);
        return false;
    }
}
// Unified Transmission Dispatch: Tries WiFi first, then GSM
bool transmitEventNow(const String& status, uint32_t timestamp, uint16_t peakVal) {
    try {
        logInfo("Preparing to send event to server: status=" + status + ", timestamp=" + String(timestamp) + ", peak=" + String(peakVal));

        // 1. Try WiFi first (Fastest & Free)
        if (WiFi.status() == WL_CONNECTED) {
            if (sendTelemetryHttpWiFi(status, timestamp, peakVal)) {
                logInfo("[FastPath] Delivered via WiFi!");
                return true;
            }
        }

        // 2. Fallback to GSM GPRS
        if (sendTelemetryHttpGPRS(status, timestamp, peakVal)) {
            logInfo("[FastPath] Delivered via GSM GPRS!");
            return true;
        }

        return false;
    } catch (...) {
        logError("Exception in transmitEventNow.");
        return false;
    }
}

// ==========================================
// FIFO QUEUE & RETRY BACKOFF ENGINE
// ==========================================
void enqueueEvent(const String& status, uint32_t timestamp, uint16_t peakVal) {
    try {
        File cache = LittleFS.open(CACHE_FILE_PATH, "a");
        if (!cache) {
            logError("Failed to open LittleFS queue file.");
            return;
        }
        String lineRecord = status + "," + String(timestamp) + "," + String(peakVal) + "," + String(USER_LATITUDE) + "," + String(USER_LONGITUDE) + "\n";
        cache.print(lineRecord);
        cache.close();
        logInfo("Event persisted locally before network send: status=" + status + ", ts=" + String(timestamp) + ", peak=" + String(peakVal));
        // Enable retry backoff cycle
        isRetryActive = true;
        backoffIndex = 0;
        lastRetryAttemptMs = millis();
    } catch (...) {
        logError("Exception in enqueueEvent.");
    }
}

bool removeQueuedEvent(const String& status, uint32_t timestamp, uint16_t peakVal) {
    try {
        if (!LittleFS.exists(CACHE_FILE_PATH)) return true;

        File cache = LittleFS.open(CACHE_FILE_PATH, "r");
        if (!cache) return false;

        String tempPath = "/temp_queue_remove.json";
        File tempCache = LittleFS.open(tempPath, "w");
        if (!tempCache) {
            cache.close();
            return false;
        }

        bool removed = false;
        while (cache.available()) {
            String record = cache.readStringUntil('\n');
            record.trim();
            if (record.length() < 5) continue;

            int idx1 = record.indexOf(',');
            int idx2 = record.indexOf(',', idx1 + 1);
            int idx3 = record.indexOf(',', idx2 + 1);

            if (idx1 != -1 && idx2 != -1 && idx3 != -1) {
                String recStatus = record.substring(0, idx1);
                uint32_t recTimestamp = record.substring(idx1 + 1, idx2).toInt();
                uint16_t recPeak = record.substring(idx2 + 1, idx3).toInt();

                if (!removed && recStatus == status && recTimestamp == timestamp && recPeak == peakVal) {
                    removed = true;
                    continue;
                }
            }

            tempCache.println(record);
        }

        cache.close();
        tempCache.close();

        LittleFS.remove(CACHE_FILE_PATH);
        if (LittleFS.exists(tempPath)) {
            LittleFS.rename(tempPath, CACHE_FILE_PATH);
        }

        return true;
    } catch (...) {
        logError("Exception in removeQueuedEvent.");
        return false;
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

            if (idx1 != -1 && idx2 != -1 && idx3 != -1) {
                String status = record.substring(0, idx1);
                uint32_t timestamp = record.substring(idx1 + 1, idx2).toInt();
                uint16_t peakVal = record.substring(idx2 + 1, idx3).toInt();

                if (transmitEventNow(status, timestamp, peakVal)) {
                    logInfo("Queued event delivered successfully.");
                } else {
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

        return allDelivered;
    } catch (...) {
        logError("Exception in flushQueue.");
        return false;
    }
}

// ==========================================
// SETUP & MAIN LOOP
// ==========================================
void setup() {
    try {
        Serial.begin(115200);

        pinMode(GSM_RTS_PIN, OUTPUT);
        setGsmRts(true);
        gsmSerialPort.begin(4800);

        logInfo("Booting Feeder Power Monitor Node (" + String(FEEDER_NAME) + ")...");

        if (!LittleFS.begin()) {
            logError("LittleFS mount failed!");
        }

        rtc.init();
        isRtcAvailable = true;
        delay(1000);

        // Connect WiFi first
        isWifiConnected = initWiFi();
        if (isWifiConnected) {
            if (waitForNtpSync(10000)) syncRtcFromNtp();
        } else {
            isGsmConnected = initGSM();
            if (isGsmConnected) {
                fetchSIMIdentity();
                syncNetworkTime();
            }
        }

        // Capture initial boot state & persist before any send attempt
        lastPowerStatus = readPowerStatus(false);
        uint32_t initTs = getCurrentTimestamp();
        uint16_t initPeak = getFilteredAnalogPeak();
        String initialState = lastPowerStatus ? "on" : "off";
        
        logInfo("Initial Power Status -> " + String(lastPowerStatus ? "ON" : "OFF"));
        enqueueEvent(initialState, initTs, initPeak);

        if (!transmitEventNow(initialState, initTs, initPeak)) {
            logError("Initial transmission failed. Event remains queued for retry.");
        } else {
            removeQueuedEvent(initialState, initTs, initPeak);
        }
    } catch (...) {
        logError("Fatal exception in setup.");
    }
}

void loop() {
    try {
        // 1. Power status change detection & fast transmission
        bool previousState = lastPowerStatus;
        bool candidateState = readPowerStatus(previousState);
        if (candidateState != previousState) {
            delay(500); // Shorter debounce window for faster response (was 1200ms)
            bool confirmed = readPowerStatus(candidateState);
            if (confirmed == candidateState) {
                lastPowerStatus = confirmed;
                uint32_t eventTime = getCurrentTimestamp();
                uint16_t currentPeak = getFilteredAnalogPeak();
                
                String stateStr = lastPowerStatus ? "on" : "off";
                logInfo("Power state transition detected -> " + stateStr);

                // Persist first, then attempt immediate send. If send fails, it remains queued for retry.
                enqueueEvent(stateStr, eventTime, currentPeak);
                if (!transmitEventNow(stateStr, eventTime, currentPeak)) {
                    logError("Immediate transmission failed. Event remains queued for background retry.");
                } else {
                    removeQueuedEvent(stateStr, eventTime, currentPeak);
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

        // 3. FIFO Queue Retry & Exponential Backoff Engine (3s, 10s, 20s, 30s, 60s)
        if (isRetryActive) {
            if (millis() - lastRetryAttemptMs >= BACKOFF_INTERVALS_MS[backoffIndex]) {
                lastRetryAttemptMs = millis();
                logInfo("Backoff retry triggered (Level " + String(backoffIndex + 1) + "). Flushing queue...");

                if (flushQueue()) {
                    logInfo("All queued events delivered.");
                    isRetryActive = false;
                    backoffIndex = 0;
                } else {
                    if (backoffIndex < MAX_BACKOFF_INDEX) {
                        backoffIndex++;
                    }
                }
            }
        }

        delay(100);
        yield();
    } catch (...) {
        logError("Exception in main loop.");
    }
}