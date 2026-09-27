/**
 * ============================================================
 * PEARL DT — Three-Phase Power Tracker Firmware (ESP32)
 * Calibrated against reference multimeter reading (168V)
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <HardwareSerial.h>
#include <LittleFS.h>
#include <time.h>

// ─── ADC Pin Definitions ────────────────────────────────────
const int ADC_PIN_RED    = 36; // VP
const int ADC_PIN_YELLOW = 39; // VN
const int ADC_PIN_BLUE   = 34; // D34

// ─── Phase Detection Thresholds ─────────────────────────────
const int THRESHOLD_ON  = 1800;
const int THRESHOLD_OFF = 800;

// ─── GSM UART (ESP32 HardwareSerial 2) ──────────────────────
const int GSM_RX_PIN  = 16; // GPIO16 ← GSM TXD
const int GSM_TX_PIN  = 17; // GPIO17 → GSM RXD
const int GSM_RTS_PIN = 4;  // GSM sleep/RTS control

HardwareSerial gsm(2); // UART2

// ─── WiFi Credentials ───────────────────────────────────────
const char* WIFI_SSID     = "V40 Lite";
const char* WIFI_PASSWORD = "@Ajibandele6";
const unsigned long WIFI_TIMEOUT_MS = 15000UL;

// ─── Server Configuration ───────────────────────────────────
const char* SERVER_IP      = "24.144.119.35";
const char* SERVER_PORT    = "8000";
const char* ENDPOINT       = "/power-tracker-gateway/";
const char* BACKUP_SMS_NUM = "2348108383472";

// ─── GSM APN ────────────────────────────────────────────────
const char* CURRENT_APN = "internet.ng.airtel.com";

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
bool wifi_connected   = false;
bool gsm_connected    = false;

String sim_serial = "UNKNOWN";
String sim_msisdn = "UNKNOWN";

uint32_t network_base_epoch  = 0;
unsigned long epoch_sync_ms  = 0;
unsigned long last_reset_handled = 0;

// ─── Three-Phase State ──────────────────────────────────────
struct PhaseReadings {
    int   peakR, peakY, peakB;
    float voltsR, voltsY, voltsB;
    bool  stateR, stateY, stateB;
};

// Last confirmed states
bool last_stateR = false;
bool last_stateY = false;
bool last_stateB = false;
bool first_run   = true;

// ─── Function Prototypes ────────────────────────────────────
PhaseReadings readPhaseSensors();
bool  initWiFi();
bool  initGSM();
void  fetchSIMIdentity();
void  syncNetworkTime();
uint32_t getCurrentTimestamp();
bool  sendHTTPWiFi(const PhaseReadings& p, uint32_t ts);
bool  sendHTTPGSM(const PhaseReadings& p, uint32_t ts);
bool  sendSMSBackup(const PhaseReadings& p, uint32_t ts);
void  cacheEventLocally(const PhaseReadings& p, uint32_t ts);
void  flushLocalCache();
void  processPowerEvent(const PhaseReadings& p);
void  monitorForReset();
void  setGsmRts(bool enabled);
String sendAT(String cmd, int timeout_ms);
String cleanResponse(String resp);
String buildJSONPayload(const PhaseReadings& p, uint32_t ts);
String buildSMSMessage(const PhaseReadings& p, uint32_t ts);

// ════════════════════════════════════════════════════════════
// SETUP
// ════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println(F("\n=== PEARL DT — Three-Phase Power Tracker Boot ==="));

    pinMode(ADC_PIN_RED,    INPUT);
    pinMode(ADC_PIN_YELLOW, INPUT);
    pinMode(ADC_PIN_BLUE,   INPUT);

    pinMode(GSM_RTS_PIN, OUTPUT);
    setGsmRts(true);

    if (!LittleFS.begin(true)) {
        Serial.println(F("[CRITICAL] LittleFS mount failed — cache unavailable."));
    } else {
        Serial.println(F("[INFO] LittleFS mounted OK."));
    }

    wifi_connected = initWiFi();

    if (wifi_connected) {
        configTime(3600, 0, "pool.ntp.org", "time.google.com");
        Serial.println(F("[TIME] NTP sync requested via WiFi."));
        delay(2000);
        flushLocalCache();
    } else {
        Serial.println(F("[WIFI] WiFi unavailable — attempting GSM GPRS pipeline."));
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
            flushLocalCache();
        } else {
            Serial.println(F("[GSM] GPRS unavailable — SMS-only mode active."));
        }
    }

    PhaseReadings init = readPhaseSensors();
    last_stateR = init.stateR;
    last_stateY = init.stateY;
    last_stateB = init.stateB;

    Serial.printf("[INIT] Phase States → R:%s Y:%s B:%s\n",
        init.stateR ? "ON" : "OFF",
        init.stateY ? "ON" : "OFF",
        init.stateB ? "ON" : "OFF");
    Serial.printf("[INIT] Voltages     → R:%.1fV Y:%.1fV B:%.1fV\n",
        init.voltsR, init.voltsY, init.voltsB);

    processPowerEvent(init);
    first_run = false;
}

// ════════════════════════════════════════════════════════════
// MAIN LOOP
// ════════════════════════════════════════════════════════════
void loop() {
    PhaseReadings current = readPhaseSensors();

    bool changed = (current.stateR != last_stateR) ||
                   (current.stateY != last_stateY) ||
                   (current.stateB != last_stateB);

    if (changed) {
        delay(1200);
        current = readPhaseSensors();
        bool still_changed = (current.stateR != last_stateR) ||
                              (current.stateY != last_stateY) ||
                              (current.stateB != last_stateB);

        if (still_changed) {
            Serial.printf("[EVENT] Phase state change! R:%s Y:%s B:%s\n",
                current.stateR ? "ON" : "OFF",
                current.stateY ? "ON" : "OFF",
                current.stateB ? "ON" : "OFF");

            last_stateR = current.stateR;
            last_stateY = current.stateY;
            last_stateB = current.stateB;

            processPowerEvent(current);
        }
    }

    if (!wifi_connected && WiFi.status() == WL_CONNECTED) {
        wifi_connected = true;
        Serial.println(F("[WIFI] WiFi reconnected."));
    } else if (wifi_connected && WiFi.status() != WL_CONNECTED) {
        wifi_connected = false;
        Serial.println(F("[WIFI] WiFi connection lost."));
    }

    if (!wifi_connected && !gsm_connected) {
        monitorForReset();
    }

    delay(300);
}

// ════════════════════════════════════════════════════════════
// PHASE SENSOR READING (Recalibrated for 168.0V Reference)
// ════════════════════════════════════════════════════════════
PhaseReadings readPhaseSensors() {
    PhaseReadings data = {0, 0, 0, 0.0f, 0.0f, 0.0f, false, false, false};

    try {
        unsigned long start = millis();
        while (millis() - start < 100UL) {
            int r = analogRead(ADC_PIN_RED);
            int y = analogRead(ADC_PIN_YELLOW);
            int b = analogRead(ADC_PIN_BLUE);
            if (r > data.peakR) data.peakR = r;
            if (y > data.peakY) data.peakY = y;
            if (b > data.peakB) data.peakB = b;
        }

        // Updated scaling coefficients based on multimeter ground-truth (168V)
        data.voltsR = (data.peakR > 380) ? (data.peakR - 380) * 0.05332f : 0.0f;
        data.voltsY = (data.peakY > 310) ? (data.peakY - 310) * 0.05230f : 0.0f;
        data.voltsB = (data.peakB > 305) ? (data.peakB - 305) * 0.05216f : 0.0f;

        data.stateR = last_stateR ? (data.peakR > THRESHOLD_OFF) : (data.peakR >= THRESHOLD_ON);
        data.stateY = last_stateY ? (data.peakY > THRESHOLD_OFF) : (data.peakY >= THRESHOLD_ON);
        data.stateB = last_stateB ? (data.peakB > THRESHOLD_OFF) : (data.peakB >= THRESHOLD_ON);

        Serial.printf("[TELEMETRY] R: %5.1fV (%4d) | Y: %5.1fV (%4d) | B: %5.1fV (%4d)\n",
                      data.voltsR, data.peakR,
                      data.voltsY, data.peakY,
                      data.voltsB, data.peakB);
    } 
    catch (const std::exception& e) {
        Serial.printf("[ERROR] Failed to read phase sensors: %s\n", e.what());
    }
    catch (...) {
        Serial.println(F("[ERROR] An unknown error occurred during phase sensor reading."));
    }

    return data;
}

// ════════════════════════════════════════════════════════════
// WORKFLOW ROUTER & TRANSMISSION HELPERS
// ════════════════════════════════════════════════════════════
void processPowerEvent(const PhaseReadings& p) {
    uint32_t ts = getCurrentTimestamp();
    Serial.printf("[WORKFLOW] Event at timestamp: %u\n", ts);
    Serial.printf("  R: %.1fV [%s]  Y: %.1fV [%s]  B: %.1fV [%s]\n",
        p.voltsR, p.stateR ? "ON":"OFF",
        p.voltsY, p.stateY ? "ON":"OFF",
        p.voltsB, p.stateB ? "ON":"OFF");

    if (wifi_connected || WiFi.status() == WL_CONNECTED) {
        if (sendHTTPWiFi(p, ts)) {
            Serial.println(F("[WORKFLOW] Route 1 (WiFi HTTP) succeeded."));
            flushLocalCache();
            return;
        }
        Serial.println(F("[WORKFLOW] Route 1 (WiFi HTTP) failed."));
    }

    if (!wifi_connected) {
        if (!gsm_connected) gsm_connected = initGSM();
        if (gsm_connected && sendHTTPGSM(p, ts)) {
            Serial.println(F("[WORKFLOW] Route 2 (GSM GPRS HTTP) succeeded."));
            flushLocalCache();
            return;
        }
        Serial.println(F("[WORKFLOW] Route 2 (GSM GPRS) failed — trying SMS."));

        if (sendSMSBackup(p, ts)) {
            Serial.println(F("[WORKFLOW] Route 3 (SMS) succeeded."));
            return;
        }
    }

    Serial.println(F("[WORKFLOW] All routes failed — caching locally."));
    cacheEventLocally(p, ts);
}

String buildJSONPayload(const PhaseReadings& p, uint32_t ts) {
    String j = "{";
    j += "\"dt\":\"" + String(DT_CODE) + "\",";
    j += "\"fdr\":\"" + String(FEEDER_NAME) + "\",";
    j += "\"tf\":\"" + String(TRANSFORMER) + "\",";
    j += "\"timestamp\":" + String(ts) + ",";
    j += "\"stat_r\":\"" + String(p.stateR ? "ON" : "OFF") + "\",";
    j += "\"volt_r\":" + String(p.voltsR, 1) + ",";
    j += "\"stat_y\":\"" + String(p.stateY ? "ON" : "OFF") + "\",";
    j += "\"volt_y\":" + String(p.voltsY, 1) + ",";
    j += "\"stat_b\":\"" + String(p.stateB ? "ON" : "OFF") + "\",";
    j += "\"volt_b\":" + String(p.voltsB, 1) + ",";
    String combined_stat = (p.stateR || p.stateY || p.stateB) ? "on" : "off";
    j += "\"stat\":\"" + combined_stat + "\",";
    j += "\"val\":" + String(max(p.peakR, max(p.peakY, p.peakB))) + ",";
    j += "\"ccid\":\"" + sim_serial + "\"";
    j += "}";
    return j;
}

String buildSMSMessage(const PhaseReadings& p, uint32_t ts) {
    String msg = "PEARL DT ALERT\n";
    msg += "Feeder: " + String(FEEDER_NAME) + "\n";
    msg += "Transformer: " + String(TRANSFORMER) + "\n";
    msg += "Red Phase:    " + String(p.stateR ? "ON" : "OFF") + " | " + String(p.voltsR, 1) + "V\n";
    msg += "Yellow Phase: " + String(p.stateY ? "ON" : "OFF") + " | " + String(p.voltsY, 1) + "V\n";
    msg += "Blue Phase:   " + String(p.stateB ? "ON" : "OFF") + " | " + String(p.voltsB, 1) + "V\n";
    msg += "Time: " + String(ts) + "\n";
    msg += "GPS: " + String(USER_LATITUDE) + "," + String(USER_LONGITUDE);
    return msg;
}

bool sendHTTPWiFi(const PhaseReadings& p, uint32_t ts) {
    if (WiFi.status() != WL_CONNECTED) return false;

    String url     = String("http://") + SERVER_IP + ":" + SERVER_PORT + ENDPOINT;
    String payload = buildJSONPayload(p, ts);

    Serial.printf("[WiFi-HTTP] POST → %s\n", url.c_str());
    Serial.printf("[WiFi-HTTP] Payload: %s\n", payload.c_str());

    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Connection", "close");
    http.setTimeout(10000);

    int code = http.POST(payload);
    http.end();

    Serial.printf("[WiFi-HTTP] Response code: %d\n", code);
    return (code >= 200 && code < 300);
}

bool sendHTTPGSM(const PhaseReadings& p, uint32_t ts) {
    if (!gsm_connected) {
        gsm_connected = initGSM();
        if (!gsm_connected) return false;
    }

    String payload = buildJSONPayload(p, ts);
    Serial.printf("[GSM-HTTP] Packaging payload: %s\n", payload.c_str());

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

    sendAT("AT+HTTPTERM", 1200);
    return completed;
}

bool sendSMSBackup(const PhaseReadings& p, uint32_t ts) {
    Serial.println(F("[SMS] Initiating SMS fallback..."));
    sendAT("AT+CMGF=1", 1500);

    String targetCmd = "AT+CMGS=\"" + String(BACKUP_SMS_NUM) + "\"";
    String response  = sendAT(targetCmd, 3000);

    if (response.indexOf(">") != -1) {
        String msg = buildSMSMessage(p, ts);
        gsm.print(msg);
        gsm.write(0x1A);

        unsigned long windowStart = millis();
        String statusBuf = "";
        while (millis() - windowStart < 15000UL) {
            if (gsm.available()) {
                statusBuf += (char)gsm.read();
                if (statusBuf.indexOf("+CMGS:") != -1) {
                    Serial.println(F("[SMS] Message delivered."));
                    return true;
                }
            }
            delay(1);
            yield();
        }
    }

    Serial.println(F("[SMS] SMS delivery timed out."));
    return false;
}

void cacheEventLocally(const PhaseReadings& p, uint32_t ts) {
    File cache = LittleFS.open(CACHE_FILE, "a");
    if (!cache) {
        Serial.println(F("[CACHE] Cannot open cache file."));
        return;
    }
    String record = String(ts) + "," +
                    (p.stateR ? "ON" : "OFF") + "," + String(p.voltsR, 1) + "," +
                    (p.stateY ? "ON" : "OFF") + "," + String(p.voltsY, 1) + "," +
                    (p.stateB ? "ON" : "OFF") + "," + String(p.voltsB, 1) + "\n";
    cache.print(record);
    cache.close();
    Serial.printf("[CACHE] Cached event: %s", record.c_str());
}

void flushLocalCache() {
    if (!LittleFS.exists(CACHE_FILE)) return;

    File cache = LittleFS.open(CACHE_FILE, "r");
    if (!cache) return;

    Serial.println(F("[FLUSH] Flushing cached events..."));
    String tempPath = "/pearl_temp.json";
    File tmp = LittleFS.open(tempPath, "w");
    if (!tmp) { cache.close(); return; }

    while (cache.available()) {
        String record = cache.readStringUntil('\n');
        if (record.length() < 5) continue;

        int c1 = record.indexOf(',');
        int c2 = record.indexOf(',', c1 + 1);
        int c3 = record.indexOf(',', c2 + 1);
        int c4 = record.indexOf(',', c5 = record.indexOf(',', c3 + 1));
        int c5 = record.indexOf(',', c4 + 1);
        int c6 = record.indexOf(',', c5 + 1);

        if (c1 < 0 || c2 < 0 || c3 < 0 || c4 < 0 || c5 < 0 || c6 < 0) continue;

        uint32_t ts_cached = record.substring(0, c1).toInt();
        PhaseReadings p_cached = {};
        p_cached.stateR  = (record.substring(c1 + 1, c2) == "ON");
        p_cached.voltsR  = record.substring(c2 + 1, c3).toFloat();
        p_cached.stateY  = (record.substring(c3 + 1, c4) == "ON");
        p_cached.voltsY  = record.substring(c4 + 1, c5).toFloat();
        p_cached.stateB  = (record.substring(c5 + 1, c6) == "ON");
        p_cached.voltsB  = record.substring(c6 + 1).toFloat();

        bool sent = false;
        if (wifi_connected || WiFi.status() == WL_CONNECTED)
            sent = sendHTTPWiFi(p_cached, ts_cached);
        if (!sent && gsm_connected)
            sent = sendHTTPGSM(p_cached, ts_cached);

        if (!sent) tmp.println(record);
        else Serial.printf("[FLUSH] Sent cached record ts=%u\n", ts_cached);
    }

    cache.close();
    tmp.close();
    LittleFS.remove(CACHE_FILE);
    LittleFS.rename(tempPath, CACHE_FILE);
}

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
            flushLocalCache();
        }
    }
}

uint32_t getCurrentTimestamp() {
    if (wifi_connected || WiFi.status() == WL_CONNECTED) {
        time_t now;
        time(&now);
        if (now > 1700000000UL) return (uint32_t)now;
    }
    if (network_base_epoch > 0) {
        unsigned long offset = (millis() - epoch_sync_ms) / 1000UL;
        return network_base_epoch + (uint32_t)offset;
    }
    return (uint32_t)(millis() / 1000UL);
}

void syncNetworkTime() {
    Serial.println(F("[TIME] Querying GSM network time (AT+CCLK?)..."));
    String resp = sendAT("AT+CCLK?", 2000);
    int idx = resp.indexOf("+CCLK: \"");
    if (idx == -1) {
        Serial.println(F("[TIME] NITZ unavailable."));
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

    if (year < 2024 || month < 1 || day < 1) return;

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
    epoch_sync_ms      = millis();
    Serial.printf("[TIME] NITZ epoch synced: %u\n", network_base_epoch);
}

// ════════════════════════════════════════════════════════════
// AT COMMAND & HARDWARE UTILITIES
// ════════════════════════════════════════════════════════════
void fetchSIMIdentity() {
    String ccidResp = sendAT("AT+CCID", 1500);
    int idx = ccidResp.indexOf("+CCID:");
    if (idx != -1) {
        sim_serial = cleanResponse(ccidResp.substring(idx + 6));
    } else {
        String iccidResp = sendAT("AT+CGSN", 1500);
        sim_serial = cleanResponse(iccidResp);
    }

    String numResp = sendAT("AT+CNUM", 1500);
    int numIdx = numResp.indexOf("+CNUM:");
    if (numIdx != -1) {
        sim_msisdn = cleanResponse(numResp.substring(numIdx + 6));
    }
    Serial.printf("[SIM] ICCID: %s | MSISDN: %s\n", sim_serial.c_str(), sim_msisdn.c_str());
}

void setGsmRts(bool enabled) {
    digitalWrite(GSM_RTS_PIN, enabled ? LOW : HIGH);
}

String sendAT(String cmd, int timeout_ms) {
    while (gsm.available()) gsm.read();
    gsm.println(cmd);

    String resp = "";
    unsigned long start = millis();
    while (millis() - start < (unsigned long)timeout_ms) {
        while (gsm.available()) {
            char c = gsm.read();
            resp += c;
        }
        yield();
    }
    return resp;
}

String cleanResponse(String resp) {
    resp.replace("\r", "");
    resp.replace("\n", " ");
    resp.replace("OK", "");
    resp.trim();
    return resp;
}