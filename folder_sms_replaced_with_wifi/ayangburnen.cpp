#include <Arduino.h>
#include <LittleFS.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <SoftwareSerial.h>
#include <ArduinoJson.h>
#include <Ds1302.h>
#include <time.h>

// ==========================================
// HARDWARE PIN MAPPINGS (D1 Mini)
// ==========================================
#define SIM800_RX_PIN     D5  // Connect to SIM800 TX
#define SIM800_TX_PIN     D6  // Connect to SIM800 RX
#define DS1302_RST_PIN    D1  // RTC CE / RST
#define DS1302_DAT_PIN    D2  // RTC IO / DAT
#define DS1302_CLK_PIN    D3  // RTC SCLK / CLK
#define POWER_SENSE_PIN   D7  // Optional Digital input for Power State
#define SENSOR_ADC_PIN    A0  // Analog pin for voltage monitor

// ==========================================
// ADC SAMPLING & THRESHOLD CONSTANTS
// ==========================================
#define ADC_SAMPLE_WINDOW_MS          100  // Sample window covering 5 full 50Hz AC cycles
#define ADC_POWER_ON_THRESHOLD_HIGH   820  // Turn ON when peak ADC rises above baseline noise
#define ADC_POWER_ON_THRESHOLD_LOW    810  // Turn OFF when peak ADC falls back to baseline

// ==========================================
// NETWORK & SERVER CONFIGURATION
// ==========================================
const char* wifi_ssid = "V40 Lite";
const char* wifi_password = "@Ajibandele6";

const char* GATEWAY_URL = "http://24.144.119.35:8000/power-tracker-gateway/";
const char* APN = "web.gprs.mtnnigeria.net";
const char* TARGET_SMS_NUM = "2348108383472";

// ==========================================
// STORAGE & SYSTEM CONSTANTS
// ==========================================
#define QUEUE_FILE_PATH "/telemetry_queue.dat"
#define MAX_QUEUE_RECORDS 50
#define HTTP_TIMEOUT_MS 10000

// Epoch Validation Boundaries (2026-01-01 to 2035-01-01)
const uint32_t MIN_VALID_EPOCH = 1767225600UL;
const uint32_t MAX_VALID_EPOCH = 2051222400UL;

// Static Metadata Configuration
const char* FEEDER_NAME = "Ayangbunren";
const char* TRANSFORMER_NAME = "Baba Olomi DT";

// ==========================================
// GLOBAL HARDWARE OBJECTS
// ==========================================
SoftwareSerial gsmSerialPort(SIM800_RX_PIN, SIM800_TX_PIN);
Ds1302 rtc(DS1302_RST_PIN, DS1302_CLK_PIN, DS1302_DAT_PIN);

// ==========================================
// DATA STRUCTURES
// ==========================================
struct TelemetryRecord {
    char stat[4];       // "on" or "off"
    uint32_t timestamp; // Unsigned 32-bit epoch
    uint16_t val;       // Peak/Smoothed Sensor ADC reading
    char fdr[16];       // Feeder designation
    char tf[16];        // Transformer designation
    char ccid[24];      // SIM CCID
};

// ==========================================
// LOGGING UTILITY
// ==========================================
void logInfo(const String& msg) {
    Serial.printf("[LOG_INFO] %s\n", msg.c_str());
}

void logError(const String& msg) {
    Serial.printf("[LOG_ERROR] %s\n", msg.c_str());
}

// ==========================================
// ADC SAMPLING UTILITY (PEAK & AVERAGE)
// ==========================================
uint16_t getPeakADC() {
    uint16_t maxVal = 0;
    uint32_t sum = 0;
    uint16_t count = 0;
    uint32_t startMs = millis();

    while (millis() - startMs < ADC_SAMPLE_WINDOW_MS) {
        uint16_t sample = analogRead(SENSOR_ADC_PIN);
        if (sample > maxVal) {
            maxVal = sample;
        }
        sum += sample;
        count++;
        delay(2);
    }

    uint16_t avg = (count > 0) ? (uint16_t)(sum / count) : maxVal;
    logInfo("Sampled ADC -> Peak: " + String(maxVal) + " | Avg: " + String(avg));
    return maxVal; // Returning Peak for clearer AC ON detection
}

// ==========================================
// EPOCH VALIDATOR
// ==========================================
bool isValidEpoch(uint32_t epoch) {
    if (epoch < MIN_VALID_EPOCH || epoch > MAX_VALID_EPOCH) {
        logError("Invalid Epoch detected: " + String(epoch) + " (Outside expected range)");
        return false;
    }
    return true;
}

// ==========================================
// RTC TIME CONVERTER
// ==========================================
uint32_t getRTCEpoch() {
    Ds1302::DateTime dt;
    rtc.getDateTime(&dt);

    struct tm timeinfo;
    timeinfo.tm_year = (dt.year >= 2000) ? (dt.year - 1900) : (dt.year + 100);
    timeinfo.tm_mon  = dt.month - 1;
    timeinfo.tm_mday = dt.day;
    timeinfo.tm_hour = dt.hour;
    timeinfo.tm_min  = dt.minute;
    timeinfo.tm_sec  = dt.second;
    timeinfo.tm_isdst = 0;

    time_t epoch = mktime(&timeinfo);
    return (uint32_t)epoch;
}

// ==========================================
// SIM800 HARDWARE / AT CONTROLLER
// ==========================================
class SIM800Controller {
private:
    Stream& gsmSerial;
    char cachedCCID[24];

    String sendATCommand(const String& cmd, uint32_t timeoutMs = 2000) {
        while (gsmSerial.available()) gsmSerial.read(); // Clear input buffer
        gsmSerial.println(cmd);
        
        String response = "";
        uint32_t start = millis();
        while (millis() - start < timeoutMs) {
            while (gsmSerial.available()) {
                char c = gsmSerial.read();
                response += c;
            }
        }
        return response;
    }

public:
    SIM800Controller(Stream& serialPort) : gsmSerial(serialPort) {
        memset(cachedCCID, 0, sizeof(cachedCCID));
    }

    void initModule() {
        logInfo("Initializing SIM800 module...");
        sendATCommand("AT", 1000);
        sendATCommand("ATE1", 1000);
        sendATCommand("AT+CPIN?", 2000);
        fetchCCID();
    }

    void fetchCCID() {
        String resp = sendATCommand("AT+CCID", 2000);
        int idx = resp.indexOf("+CCID:");
        if (idx != -1) {
            String raw = resp.substring(idx + 7);
            raw.trim();
            raw.toCharArray(cachedCCID, sizeof(cachedCCID));
            logInfo("SIM CCID fetched: " + String(cachedCCID));
        } else {
            strncpy(cachedCCID, "UNKNOWN_CCID", sizeof(cachedCCID));
        }
    }

    const char* getCCID() const {
        return cachedCCID;
    }

    bool verifyAndActivateBearer() {
        logInfo("Checking GPRS bearer state (AT+SAPBR=2,1)...");
        String resp = sendATCommand("AT+SAPBR=2,1", 3000);
        
        if (resp.indexOf("+SAPBR: 1,1") != -1 && resp.indexOf("0.0.0.0") == -1) {
            logInfo("GPRS bearer is active and healthy.");
            return true;
        }

        logError("GPRS bearer context inactive. Re-initializing...");
        sendATCommand("AT+SAPBR=0,1", 2000);
        sendATCommand("AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\"", 1000);
        sendATCommand("AT+SAPBR=3,1,\"APN\",\"" + String(APN) + "\"", 1000);
        
        resp = sendATCommand("AT+SAPBR=1,1", 10000);
        if (resp.indexOf("OK") != -1) {
            logInfo("GPRS bearer context successfully established.");
            return true;
        } else {
            logError("Failed to activate GPRS bearer context.");
            return false;
        }
    }

    int sendHTTPPost(const String& jsonPayload) {
        if (!verifyAndActivateBearer()) {
            return 601;
        }

        sendATCommand("AT+HTTPTERM", 1000);
        sendATCommand("AT+HTTPINIT", 1000);
        sendATCommand("AT+HTTPPARA=\"CID\",1", 1000);
        sendATCommand("AT+HTTPPARA=\"URL\",\"" + String(GATEWAY_URL) + "\"", 1000);
        sendATCommand("AT+HTTPPARA=\"CONTENT\",\"application/json\"", 1000);

        String dataCmd = "AT+HTTPDATA=" + String(jsonPayload.length()) + ",10000";
        sendATCommand(dataCmd, 2000);
        sendATCommand(jsonPayload, 2000);

        String actionResp = sendATCommand("AT+HTTPACTION=1", 10000);
        
        int statusIdx = actionResp.indexOf("+HTTPACTION: 1,");
        if (statusIdx != -1) {
            String sub = actionResp.substring(statusIdx + 15);
            int commaIdx = sub.indexOf(',');
            int httpCode = sub.substring(0, commaIdx).toInt();
            sendATCommand("AT+HTTPTERM", 1000);
            return httpCode;
        }

        sendATCommand("AT+HTTPTERM", 1000);
        return 601;
    }

    bool sendSMS(const String& message) {
        logInfo("Attempting secondary SMS fallback delivery...");
        sendATCommand("AT+CMGF=1", 1000);
        String resp = sendATCommand("AT+CMGS=\"" + String(TARGET_SMS_NUM) + "\"", 2000);
        
        if (resp.indexOf(">") != -1) {
            gsmSerial.print(message);
            gsmSerial.write(0x1A); // Ctrl+Z
            
            uint32_t start = millis();
            while (millis() - start < 10000) {
                if (gsmSerial.find("+CMGS:")) {
                    logInfo("SMS token delivered successfully.");
                    return true;
                }
            }
        }
        logError("SMS gateway transmission timed out.");
        return false;
    }
};

SIM800Controller gsmModem(gsmSerialPort);

// ==========================================
// LITTLEFS BACKUP STORAGE QUEUE
// ==========================================
class TelemetryCacheQueue {
public:
    static bool init() {
        if (!LittleFS.begin()) {
            logError("Failed to mount LittleFS file system.");
            return false;
        }
        logInfo("LittleFS storage subsystem mounted.");
        return true;
    }

    static bool enqueueRecord(const TelemetryRecord& rec) {
        if (!isValidEpoch(rec.timestamp)) {
            logError("Caching rejected: Payload timestamp failover sanity check.");
            return false;
        }

        File f = LittleFS.open(QUEUE_FILE_PATH, "a");
        if (!f) {
            logError("Failed to open backup cache file for writing.");
            return false;
        }

        f.write((const uint8_t*)&rec, sizeof(TelemetryRecord));
        f.close();
        logInfo("Event payload committed to LittleFS backup cache.");
        return true;
    }

    static bool popRecord(TelemetryRecord& rec) {
        if (!LittleFS.exists(QUEUE_FILE_PATH)) return false;

        File f = LittleFS.open(QUEUE_FILE_PATH, "r");
        if (!f || f.size() < sizeof(TelemetryRecord)) {
            if (f) f.close();
            LittleFS.remove(QUEUE_FILE_PATH);
            return false;
        }

        size_t totalRecords = f.size() / sizeof(TelemetryRecord);
        f.read((uint8_t*)&rec, sizeof(TelemetryRecord));

        size_t remainingRecords = totalRecords - 1;
        uint8_t* buffer = nullptr;
        if (remainingRecords > 0) {
            buffer = new uint8_t[remainingRecords * sizeof(TelemetryRecord)];
            f.read(buffer, remainingRecords * sizeof(TelemetryRecord));
        }
        f.close();

        if (remainingRecords > 0 && buffer != nullptr) {
            File wf = LittleFS.open(QUEUE_FILE_PATH, "w");
            wf.write(buffer, remainingRecords * sizeof(TelemetryRecord));
            wf.close();
            delete[] buffer;
        } else {
            LittleFS.remove(QUEUE_FILE_PATH);
        }

        return true;
    }
};

// ==========================================
// TELEMETRY ROUTER & EXECUTION CONTROLLER
// ==========================================
class TelemetryRouter {
private:
    SIM800Controller& gsm;

    String serializePayload(const TelemetryRecord& rec) {
        StaticJsonDocument<256> doc;
        doc["stat"] = rec.stat;
        doc["timestamp"] = rec.timestamp;
        doc["val"] = rec.val;
        doc["fdr"] = rec.fdr;
        doc["tf"] = rec.tf;
        doc["ccid"] = rec.ccid;

        String output;
        serializeJson(doc, output);
        return output;
    }

    bool sendWiFiHTTPPost(const String& jsonPayload) {
        if (WiFi.status() != WL_CONNECTED) {
            logInfo("WiFi disconnected. Attempting fast reconnect to: " + String(wifi_ssid));
            WiFi.begin(wifi_ssid, wifi_password);
            uint32_t startMs = millis();
            while (WiFi.status() != WL_CONNECTED && (millis() - startMs < 5000)) {
                delay(100);
            }
        }

        if (WiFi.status() == WL_CONNECTED) {
            logInfo("Primary WiFi active. Sending HTTP POST...");
            WiFiClient client;
            HTTPClient http;

            if (http.begin(client, GATEWAY_URL)) {
                http.addHeader("Content-Type", "application/json");
                http.setTimeout(HTTP_TIMEOUT_MS);

                int httpCode = http.POST(jsonPayload);
                http.end();

                logInfo("WiFi HTTP response code: " + String(httpCode));
                if (httpCode == 200 || httpCode == 201) {
                    return true;
                }
            } else {
                logError("Failed to initialize WiFi HTTPClient connection.");
            }
        } else {
            logError("WiFi network unavailable.");
        }
        return false;
    }

    bool sendPayloadOverBestAvailable(const String& jsonPayload) {
        // STEP 1A: Primary Route - WiFi HTTP
        if (sendWiFiHTTPPost(jsonPayload)) {
            logInfo("Stage 1 HTTP delivery (WiFi) successful.");
            return true;
        }

        // STEP 1B: Secondary Route - SIM800 GPRS HTTP
        logError("WiFi route failed or unavailable. Falling back to GSM GPRS pipeline...");
        int gsmCode = gsm.sendHTTPPost(jsonPayload);
        if (gsmCode == 200 || gsmCode == 201) {
            logInfo("Stage 1 HTTP delivery (GSM GPRS) successful.");
            return true;
        }

        logError("GSM GPRS HTTP route failed (Error Code: " + String(gsmCode) + ").");
        return false;
    }

public:
    TelemetryRouter(SIM800Controller& gsmRef) : gsm(gsmRef) {}

    bool dispatch(const TelemetryRecord& rec) {
        if (!isValidEpoch(rec.timestamp)) {
            logError("Dispatch blocked: Record epoch corrupted.");
            return false;
        }

        String jsonPayload = serializePayload(rec);
        logInfo("Packaging JSON Telemetry payload: " + jsonPayload);

        // STAGE 1: HTTP Transmission (WiFi first, fallback to GPRS)
        if (sendPayloadOverBestAvailable(jsonPayload)) {
            flushCachedQueue();
            return true;
        }

        // STAGE 2: Secondary SMS Backup Route
        logError("Stage 1 (WiFi & GPRS) failed. Diverting to SMS backup channel...");
        if (gsm.sendSMS(jsonPayload)) {
            logInfo("Stage 2 SMS backup delivery successful.");
            return true;
        }

        // STAGE 3: Local Flash Persistence
        logError("Stage 1 & Stage 2 failed. Committing packet to LittleFS cache...");
        return TelemetryCacheQueue::enqueueRecord(rec);
    }

    void flushCachedQueue() {
        TelemetryRecord cachedRec;
        logInfo("Checking backup queue for cached records...");

        while (TelemetryCacheQueue::popRecord(cachedRec)) {
            logInfo("Flushing cached record for Timestamp=" + String(cachedRec.timestamp));
            String jsonPayload = serializePayload(cachedRec);

            if (!sendPayloadOverBestAvailable(jsonPayload)) {
                logError("Failed to flush record over best route. Re-enqueueing packet...");
                TelemetryCacheQueue::enqueueRecord(cachedRec);
                break;
            }
            logInfo("Purged cached record successfully: Time=" + String(cachedRec.timestamp));
        }
    }
};

TelemetryRouter router(gsmModem);

// ==========================================
// ARDUINO SKETCH ENTRY POINTS
// ==========================================
bool lastPowerIsOn = false;
bool isInitialRun = true;

void setup() {
    Serial.begin(115200);
    gsmSerialPort.begin(9600);
    delay(200);

    logInfo("Feeder tracker booting...");

    pinMode(POWER_SENSE_PIN, INPUT);
    rtc.init();

    TelemetryCacheQueue::init();

    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid, wifi_password);
    logInfo("Initiating connection to WiFi AP: " + String(wifi_ssid));

    gsmModem.initModule();

    logInfo("Hardware setup complete. Starting power monitor loop...");
}

void loop() {
    // Sample A0 over 100ms window to capture max AC peak
    uint16_t adcPeakVal = getPeakADC();
    uint8_t digitalSense = digitalRead(POWER_SENSE_PIN);

    // Apply Hysteresis using peak reading
    bool currentPowerIsOn;
    if (adcPeakVal >= ADC_POWER_ON_THRESHOLD_HIGH) {
        currentPowerIsOn = true;
    } else if (adcPeakVal <= ADC_POWER_ON_THRESHOLD_LOW) {
        currentPowerIsOn = false;
    } else {
        currentPowerIsOn = lastPowerIsOn;
    }

    // Optional digital sense override
    if (digitalSense == LOW && adcPeakVal < ADC_POWER_ON_THRESHOLD_HIGH) {
        currentPowerIsOn = true;
    }

    // Evaluate state transition
    if (isInitialRun || (currentPowerIsOn != lastPowerIsOn)) {
        const String previousState = lastPowerIsOn ? "ON" : "OFF";
        const String currentState = currentPowerIsOn ? "ON" : "OFF";
        logInfo("POWER_TRANSITION: prev=" + previousState + " -> " + currentState +
                " | Peak ADC=" + String(adcPeakVal) +
                " | HI=" + String(ADC_POWER_ON_THRESHOLD_HIGH) +
                " | LO=" + String(ADC_POWER_ON_THRESHOLD_LOW) +
                " | D7=" + String(digitalSense));

        lastPowerIsOn = currentPowerIsOn;
        isInitialRun = false;

        TelemetryRecord record;
        strncpy(record.stat, currentPowerIsOn ? "on" : "off", sizeof(record.stat));
        record.timestamp = getRTCEpoch();
        record.val = adcPeakVal;
        strncpy(record.fdr, FEEDER_NAME, sizeof(record.fdr));
        strncpy(record.tf, TRANSFORMER_NAME, sizeof(record.tf));
        strncpy(record.ccid, gsmModem.getCCID(), sizeof(record.ccid));

        logInfo("Dispatching state: " + String(record.stat) + " | ADC=" + String(record.val) + " | Epoch=" + String(record.timestamp));
        router.dispatch(record);
    }

    delay(200);
}