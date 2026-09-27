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
#define SIM800_RX_PIN     D5  
#define SIM800_TX_PIN     D6  
#define DS1302_RST_PIN    D1  
#define DS1302_DAT_PIN    D2  
#define DS1302_CLK_PIN    D3  
#define POWER_SENSE_PIN   D7  
#define SENSOR_ADC_PIN    A0  

// ==========================================
// ADC SAMPLING & THRESHOLD CONSTANTS
// ==========================================
#define ADC_SAMPLE_WINDOW_MS          200  // 200ms sample window (10 full 50Hz AC cycles)
#define VPP_POWER_ON_THRESHOLD_HIGH   22   // Vpp >= 22 indicates AC power present
#define VPP_POWER_ON_THRESHOLD_LOW    12   // Vpp <= 12 indicates AC power absent

// Sliding window: Filter over 7 windows (~1.4s total observation)
#define ROLLING_WINDOW_SIZE           7    
#define STATE_DEBOUNCE_COUNT          3    // Consecutive evaluation cycles needed for transition

// ==========================================
// NETWORK & SERVER CONFIGURATION
// ==========================================
const char* wifi_ssid = "V40 Lite";
const char* wifi_password = "@Ajibandele6";

const char* GATEWAY_URL = "http://24.144.119.35:8000/power-tracker-gateway/";
const char* APN = "9mobile";
const char* TARGET_SMS_NUM = "2348108383472";

// ==========================================
// STORAGE & SYSTEM CONSTANTS
// ==========================================
#define QUEUE_FILE_PATH "/telemetry_queue.dat"
#define MAX_QUEUE_RECORDS 50
#define HTTP_TIMEOUT_MS 10000

const uint32_t MIN_VALID_EPOCH = 1767225600UL;
const uint32_t MAX_VALID_EPOCH = 2051222400UL;

const char* FEEDER_NAME = "Erunwen";
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
    char stat[4];       
    uint32_t timestamp; 
    uint16_t val;       
    char fdr[16];       
    char tf[16];        
    char ccid[24];      
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
// ADC SAMPLING UTILITY
// ==========================================
uint16_t getVppADC() {
    uint16_t maxVal = 0;
    uint16_t minVal = 1023;
    uint32_t sampleCount = 0;
    uint32_t startMs = millis();

    try {
        while (millis() - startMs < ADC_SAMPLE_WINDOW_MS) {
            uint16_t sample = analogRead(SENSOR_ADC_PIN);
            if (sample > maxVal) maxVal = sample;
            if (sample < minVal) minVal = sample;
            sampleCount++;
            yield(); 
        }

        uint16_t vpp = (maxVal >= minVal) ? (maxVal - minVal) : 0;
        logInfo("Sampled ADC -> Max: " + String(maxVal) + " | Min: " + String(minVal) +
                " | Vpp: " + String(vpp) + " | Samples: " + String(sampleCount));
        
        return vpp;
    } catch (...) {
        logError("Exception during ADC sampling.");
        return 0;
    }
}

// ==========================================
// ROLLING PEAK FILTER (SOLVES ALIASING DROPS)
// ==========================================
class RollingVppFilter {
private:
    uint16_t history[ROLLING_WINDOW_SIZE];
    uint8_t index;

public:
    RollingVppFilter() : index(0) {
        memset(history, 0, sizeof(history));
    }

    void addSample(uint16_t vpp) {
        history[index] = vpp;
        index = (index + 1) % ROLLING_WINDOW_SIZE;
    }

    uint16_t getMaxVpp() const {
        uint16_t maxV = 0;
        for (uint8_t i = 0; i < ROLLING_WINDOW_SIZE; i++) {
            if (history[i] > maxV) maxV = history[i];
        }
        return maxV;
    }
};

RollingVppFilter vppFilter;

// ==========================================
// EPOCH VALIDATOR & RTC TIME CONVERTER
// ==========================================
bool isValidEpoch(uint32_t epoch) {
    if (epoch < MIN_VALID_EPOCH || epoch > MAX_VALID_EPOCH) {
        logError("Invalid Epoch detected: " + String(epoch));
        return false;
    }
    return true;
}

uint32_t getRTCEpoch() {
    try {
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
    } catch (...) {
        logError("Exception occurred while reading RTC epoch.");
        return 0;
    }
}

// ==========================================
// SIM800 CONTROLLER
// ==========================================
class SIM800Controller {
private:
    Stream& gsmSerial;
    char cachedCCID[24];

    String sendATCommand(const String& cmd, uint32_t timeoutMs = 2000) {
        while (gsmSerial.available()) gsmSerial.read();
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
        logInfo("Checking GPRS bearer state...");
        String resp = sendATCommand("AT+SAPBR=2,1", 3000);
        
        if (resp.indexOf("+SAPBR: 1,1") != -1 && resp.indexOf("0.0.0.0") == -1) {
            logInfo("GPRS bearer is active.");
            return true;
        }

        logError("GPRS bearer inactive. Re-initializing...");
        sendATCommand("AT+SAPBR=0,1", 2000);
        sendATCommand("AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\"", 1000);
        sendATCommand("AT+SAPBR=3,1,\"APN\",\"" + String(APN) + "\"", 1000);
        
        resp = sendATCommand("AT+SAPBR=1,1", 10000);
        return (resp.indexOf("OK") != -1);
    }

    int sendHTTPPost(const String& jsonPayload) {
        if (!verifyAndActivateBearer()) return 601;

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
        logInfo("Attempting SMS backup delivery...");
        sendATCommand("AT+CMGF=1", 1000);
        String resp = sendATCommand("AT+CMGS=\"" + String(TARGET_SMS_NUM) + "\"", 2000);
        
        if (resp.indexOf(">") != -1) {
            gsmSerial.print(message);
            gsmSerial.write(0x1A);
            
            uint32_t start = millis();
            while (millis() - start < 10000) {
                if (gsmSerial.find("+CMGS:")) {
                    logInfo("SMS delivered successfully.");
                    return true;
                }
            }
        }
        logError("SMS transmission timed out.");
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
            logError("Failed to mount LittleFS.");
            return false;
        }
        logInfo("LittleFS mounted.");
        return true;
    }

    static bool enqueueRecord(const TelemetryRecord& rec) {
        if (!isValidEpoch(rec.timestamp)) return false;

        File f = LittleFS.open(QUEUE_FILE_PATH, "a");
        if (!f) return false;

        f.write((const uint8_t*)&rec, sizeof(TelemetryRecord));
        f.close();
        logInfo("Event committed to LittleFS queue.");
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
            WiFi.begin(wifi_ssid, wifi_password);
            uint32_t startMs = millis();
            while (WiFi.status() != WL_CONNECTED && (millis() - startMs < 5000)) {
                delay(100);
            }
        }

        if (WiFi.status() == WL_CONNECTED) {
            WiFiClient client;
            HTTPClient http;

            if (http.begin(client, GATEWAY_URL)) {
                http.addHeader("Content-Type", "application/json");
                http.setTimeout(HTTP_TIMEOUT_MS);

                int httpCode = http.POST(jsonPayload);
                http.end();

                if (httpCode == 200 || httpCode == 201) return true;
            }
        }
        return false;
    }

    bool sendPayloadOverBestAvailable(const String& jsonPayload) {
        if (sendWiFiHTTPPost(jsonPayload)) return true;

        logError("WiFi failed. Falling back to GSM...");
        int gsmCode = gsm.sendHTTPPost(jsonPayload);
        return (gsmCode == 200 || gsmCode == 201);
    }

public:
    TelemetryRouter(SIM800Controller& gsmRef) : gsm(gsmRef) {}

    bool dispatch(const TelemetryRecord& rec) {
        if (!isValidEpoch(rec.timestamp)) return false;

        String jsonPayload = serializePayload(rec);
        logInfo("Payload: " + jsonPayload);

        if (sendPayloadOverBestAvailable(jsonPayload)) {
            flushCachedQueue();
            return true;
        }

        if (gsm.sendSMS(jsonPayload)) return true;

        return TelemetryCacheQueue::enqueueRecord(rec);
    }

    void flushCachedQueue() {
        TelemetryRecord cachedRec;
        while (TelemetryCacheQueue::popRecord(cachedRec)) {
            String jsonPayload = serializePayload(cachedRec);
            if (!sendPayloadOverBestAvailable(jsonPayload)) {
                TelemetryCacheQueue::enqueueRecord(cachedRec);
                break;
            }
        }
    }
};

TelemetryRouter router(gsmModem);

// ==========================================
// MAIN MONITORING LOOP
// ==========================================
bool confirmedPowerIsOn = false;
bool pendingPowerIsOn = false;
uint8_t debounceCounter = 0;
bool isInitialRun = true;

void setup() {
    Serial.begin(115200);
    gsmSerialPort.begin(9600);
    delay(200);

    logInfo("Feeder tracker booting...");

    pinMode(POWER_SENSE_PIN, INPUT_PULLUP);
    rtc.init();
    TelemetryCacheQueue::init();

    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid, wifi_password);

    gsmModem.initModule();
    logInfo("Setup complete. Starting monitor loop...");
}

void loop() {
    try {
        // Read raw Vpp for this 200ms sample window
        uint16_t instantVpp = getVppADC();

        // Push into rolling window filter
        vppFilter.addSample(instantVpp);

        // Retrieve the maximum peak-to-peak reading across the rolling observation window (~1.4s)
        uint16_t filteredVpp = vppFilter.getMaxVpp();

        // Hysteresis evaluation using the filtered peak value
        bool rawStateIsOn;
        if (filteredVpp >= VPP_POWER_ON_THRESHOLD_HIGH) {
            rawStateIsOn = true;
        } else if (filteredVpp <= VPP_POWER_ON_THRESHOLD_LOW) {
            rawStateIsOn = false;
        } else {
            rawStateIsOn = confirmedPowerIsOn;
        }

        // Software debouncing logic
        if (rawStateIsOn == pendingPowerIsOn) {
            debounceCounter++;
        } else {
            pendingPowerIsOn = rawStateIsOn;
            debounceCounter = 1;
        }

        // Trigger transition only when state changes reliably
        if (isInitialRun || (debounceCounter >= STATE_DEBOUNCE_COUNT && pendingPowerIsOn != confirmedPowerIsOn)) {
            const String previousState = confirmedPowerIsOn ? "ON" : "OFF";
            const String currentState = pendingPowerIsOn ? "ON" : "OFF";
            
            logInfo("POWER_TRANSITION: " + previousState + " -> " + currentState +
                    " | FilteredVpp=" + String(filteredVpp) +
                    " | InstantVpp=" + String(instantVpp) +
                    " | HI=" + String(VPP_POWER_ON_THRESHOLD_HIGH) +
                    " | LO=" + String(VPP_POWER_ON_THRESHOLD_LOW));

            confirmedPowerIsOn = pendingPowerIsOn;
            isInitialRun = false;

            TelemetryRecord record;
            strncpy(record.stat, confirmedPowerIsOn ? "on" : "off", sizeof(record.stat));
            record.timestamp = getRTCEpoch();
            record.val = filteredVpp;
            strncpy(record.fdr, FEEDER_NAME, sizeof(record.fdr));
            strncpy(record.tf, TRANSFORMER_NAME, sizeof(record.tf));
            strncpy(record.ccid, gsmModem.getCCID(), sizeof(record.ccid));

            router.dispatch(record);
        }
    } catch (...) {
        logError("Unhandled exception inside loop.");
    }

    delay(20);
}