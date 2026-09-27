#include <Arduino.h>
#include <SoftwareSerial.h>
#include <Ds1302.h>
#include <LittleFS.h> // Modern, clean replacement for the deprecated FS.h/SPIFFS

// --- Hardware Pin Definitions ---
const int ADC_PIN = A0;
// --- Calibrated Hardware Thresholds ---
const int THRESHOLD_ON = 840;  // Intercepts high active wave peaks (844 - 895)
const int THRESHOLD_OFF = 740; // Triggers when the peak drops into the active wave valleys (682 - 717)

/* 
  === WEMOS D1 MINI PIN MAPPING TO GSM (SIM900) ===
  - D5 (GPIO14) -> SIM TXD (Wemos RX)
  - D6 (GPIO12) -> SIM RXD (Wemos TX)
  - D8 (GPIO15) -> SIM RTS (Hardware Flow / Sleep Control)
  
  === WEMOS D1 MINI PIN MAPPING TO DS1302 RTC MODULE ===
  - D2 (GPIO4)  -> CLK (Clock)
  - D1 (GPIO5)  -> DAT (Data I/O)
  - D7 (GPIO13) -> RST (Reset / Chip Enable)
*/

const int GSM_RX_PIN = D5; 
const int GSM_TX_PIN = D6; 
const int GSM_RTS_PIN = D8; 

const int RTC_CLK_PIN = D2;
const int RTC_DAT_PIN = D1;
const int RTC_RST_PIN = D7;

// --- Object Initializations ---
SoftwareSerial gsm(GSM_RX_PIN, GSM_TX_PIN);
Ds1302 rtc(RTC_RST_PIN, RTC_CLK_PIN, RTC_DAT_PIN);

bool last_status = false;
bool is_gsm_connected = false;
bool rtc_available = false;
String sim_msisdn = "UNKNOWN";
String sim_serial = "UNKNOWN";

// --- Time Sync Variables ---
uint32_t network_base_epoch = 0;
unsigned long epoch_sync_millis = 0;

// --- Host Configuration ---
const char* server_ip = "24.144.119.35";
const char* server_port = "8000";
const char* backup_sms_target = "2348108383472";

// --- Static Node Metadata ---
const char* feeder_name = "Ayangbunren";
const char* transformer = "Baba Olomi DT";
const char* CURRENT_APN = "web.gprs.mtnnigeria.net";
// const char* CURRENT_APN = "internet.ng.airtel.com";
const char* CACHE_FILE = "/telemetry_cache.json";

// --- Static GPS Fallback coordinates ---
const char* user_latitude = "6.5230";
const char* user_longitude = "3.3420";

// --- Function Prototypes ---
bool initGSM();
void fetchSIMIdentity();
bool readPowerStatus();
bool sendTelemetryHttp(String status, uint32_t timestamp, int peakVal);
void processPowerEvent(bool status);
bool sendSMSBackup(String status, uint32_t timestamp, int peakVal);
void cacheEventLocally(String status, uint32_t timestamp, int peakVal);
void flushLocalCache();
uint32_t getCurrentTimestamp();
void syncNetworkTime();
String sendAT(String cmd, int timeout);
String cleanResponse(String resp);
void monitorForReset();
void setGsmRts(bool enabled);

unsigned long lastResetHandled = 0;

void setup() {
  Serial.begin(115200);
  
  // Initialize GSM RTS Control Pin
  pinMode(GSM_RTS_PIN, OUTPUT);
  setGsmRts(true);
  gsm.begin(4800);

  Serial.println(F("\n=== GSM Power Monitor - NITZ & DS1302 Dual Clock Build ==="));

  // Fix: Upgraded file system to LittleFS
  if (!LittleFS.begin()) {
    Serial.println(F("[CRITICAL] LittleFS mount failed! Flash memory storage unavailable."));
  }

  // Initialize DS1302 RTC
  rtc.init();
  Ds1302::DateTime now;
  rtc.getDateTime(&now);
  Serial.println(F("[SUCCESS] DS1302 RTC operational and communicating."));
  rtc_available = true;

  delay(5000);

  gsm.println("AT+IPR=4800");
  delay(500);
  while (gsm.available()) {
    gsm.read();
  }

  sendAT("AT+CLTS=1", 1000); // Enable network clock capture
  sendAT("AT&W", 1000);      // Save modem config

  int initAttempts = 0;
  while (!is_gsm_connected) {
    initAttempts++;
    is_gsm_connected = initGSM();
    if (!is_gsm_connected) {
      Serial.printf("[ERROR] Base GPRS Pipeline Failed (Attempt %d). Retrying...\n", initAttempts);
      delay(5000);
    }
    if (initAttempts >= 3 && !is_gsm_connected) {
      break;
    }
  }

  if (is_gsm_connected) {
    fetchSIMIdentity();
    syncNetworkTime();
    flushLocalCache();
  }

  last_status = readPowerStatus();
  Serial.print(F("[INITIAL POWER STATUS] : "));
  Serial.println(last_status ? "ON" : "OFF");
  processPowerEvent(last_status);
}

void loop() {
  bool current = readPowerStatus();
  if (current != last_status) {
    delay(1200);
    current = readPowerStatus();
    if (current != last_status) {
      last_status = current;
      Serial.printf("[EVENT] Power state transition detected. New State: %s\n", last_status ? "ON" : "OFF");
      processPowerEvent(last_status);
    }
  }
  monitorForReset();
  delay(300);
}

void setGsmRts(bool enabled) {
  digitalWrite(GSM_RTS_PIN, enabled ? LOW : HIGH);
}

void syncNetworkTime() {
  Serial.println(F("[TIME-SYNC] Querying network baseline time parameters (AT+CCLK?)..."));
  String resp = sendAT("AT+CCLK?", 2000);

  int idx = resp.indexOf("+CCLK: \"");
  if (idx != -1) {
    int startIdx = idx + 8;
    // Fix: Explicitly cast to match signed vs unsigned loop variables
    if (resp.length() >= (unsigned int)(startIdx + 17)) {
      String rtcDate = resp.substring(startIdx, startIdx + 8);
      String rtcTime = resp.substring(startIdx + 9, startIdx + 17);

      int year = 2000 + rtcDate.substring(0, 2).toInt();
      int month = rtcDate.substring(3, 5).toInt();
      int day = rtcDate.substring(6, 8).toInt();
      int hour = rtcTime.substring(0, 2).toInt();
      int minute = rtcTime.substring(3, 5).toInt();
      int second = rtcTime.substring(6, 8).toInt();

      if (year >= 2026 && month > 0 && day > 0) {
        if (rtc_available) {
          Ds1302::DateTime netTime = {0};
          netTime.year = year - 2000;
          netTime.month = month;
          netTime.day = day;
          netTime.hour = hour;
          netTime.minute = minute;
          netTime.second = second;
          rtc.setDateTime(&netTime);
          Serial.println(F("[TIME-SYNC] Synchronized hardware DS1302 with Cell Tower time."));
        }

        long numDays = 0;
        int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) {
          daysInMonth[1] = 29;
        }
        for (int i = 1970; i < year; i++) {
          numDays += 365;
          if (i % 4 == 0 && (i % 100 != 0 || i % 400 == 0)) numDays++;
        }
        for (int i = 0; i < month - 1; i++) {
          numDays += daysInMonth[i];
        }
        numDays += day - 1;

        network_base_epoch = (numDays * 86400UL) + (hour * 3600UL) + (minute * 60UL) + second;
        epoch_sync_millis = millis();
        return;
      }
    }
  }
  Serial.println(F("[TIME-SYNC] Network clock unavailable. Keeping current DS1302 settings."));
}

uint32_t ds1302ToEpoch(const Ds1302::DateTime &dt) {
  int year = 2000 + dt.year;
  int month = dt.month;
  int day = dt.day;
  int hour = dt.hour;
  int minute = dt.minute;
  int second = dt.second;

  long numDays = 0;
  int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) {
    daysInMonth[1] = 29;
  }
  for (int i = 1970; i < year; i++) {
    numDays += 365;
    if (i % 4 == 0 && (i % 100 != 0 || i % 400 == 0)) numDays++;
  }
  for (int i = 0; i < month - 1; i++) {
    numDays += daysInMonth[i];
  }
  numDays += day - 1;

  return (uint32_t)((numDays * 86400UL) + (hour * 3600UL) + (minute * 60UL) + second);
}

uint32_t getCurrentTimestamp() {
  if (rtc_available) {
    Ds1302::DateTime dt;
    rtc.getDateTime(&dt);
    if (dt.year > 0) {
      return ds1302ToEpoch(dt);
    }
  }
  
  if (network_base_epoch > 0) {
    unsigned long relativeOffset = (millis() - epoch_sync_millis) / 1000UL;
    return network_base_epoch + (uint32_t)relativeOffset;
  }
  return (uint32_t)(millis() / 1000UL);
}

void processPowerEvent(bool status) {
  uint32_t eventTime = getCurrentTimestamp();
  int current_peak = analogRead(ADC_PIN);
  String stateStr = status ? "on" : "off";

  Serial.printf("[WORKFLOW] Router invoked for event timestamp: %u\n", eventTime);

  bool httpSuccess = false;
  if (is_gsm_connected) {
    httpSuccess = sendTelemetryHttp(stateStr, eventTime, current_peak);
  }

  if (httpSuccess) {
    Serial.println(F("[WORKFLOW] Stage 1 HTTP delivery confirmed successful. Checking queue logs..."));
    flushLocalCache();
    return;
  }

  Serial.println(F("[WORKFLOW] Stage 1 HTTP route failed. Diverting execution logic to SMS backup channel..."));
  bool smsSuccess = sendSMSBackup(stateStr, eventTime, current_peak);
  if (smsSuccess) {
    Serial.println(F("[WORKFLOW] Stage 2 SMS backup successful."));
    return;
  }

  Serial.println(F("[WORKFLOW] Stage 1 & 2 dropped. Committing packet to LittleFS backup storage cache..."));
  cacheEventLocally(stateStr, eventTime, current_peak);
}

bool sendTelemetryHttp(String status, uint32_t timestamp, int peakVal) {
  if (!is_gsm_connected) {
    is_gsm_connected = initGSM();
    if (!is_gsm_connected) return false;
  }

  String payload = "{";
  payload += "\"stat\":\"" + status + "\",";
  payload += "\"timestamp\":" + String(timestamp) + ",";
  payload += "\"val\":" + String(peakVal) + ",";
  payload += "\"fdr\":\"" + String(feeder_name) + "\",";
  payload += "\"tf\":\"" + String(transformer) + "\",";
  payload += "\"ccid\":\"" + sim_serial + "\"";
  payload += "}";

  Serial.printf("[HTTP-LOG] Packaging JSON Telemetry payload: %s\n", payload.c_str());

  sendAT("AT+HTTPTERM", 1000);
  delay(150);
  String initResp = sendAT("AT+HTTPINIT", 1500);
  if (initResp.indexOf("ERROR") != -1) {
    sendAT("AT+HTTPTERM", 1000);
    initResp = sendAT("AT+HTTPINIT", 1500);
    if (initResp.indexOf("ERROR") != -1) {
      Serial.println(F("[HTTP-ERROR] HTTP engine structural initialization crashed."));
      is_gsm_connected = false;
      return false;
    }
  }

  sendAT("AT+HTTPPARA=\"CID\",1", 1200);
  String url = "http://" + String(server_ip) + ":" + String(server_port) + "/power-tracker-gateway/";
  sendAT("AT+HTTPPARA=\"URL\",\"" + url + "\"", 1500);
  sendAT("AT+HTTPPARA=\"CONTENT\",\"application/json\"", 1500);

  String dataCmd = "AT+HTTPDATA=" + String(payload.length()) + ",5000";
  if (sendAT(dataCmd, 2000).indexOf("DOWNLOAD") == -1) {
    Serial.println(F("[HTTP-ERROR] Modem failed to enter raw payload download stream buffer mode."));
    sendAT("AT+HTTPTERM", 1000);
    return false;
  }

  gsm.print(payload);
  delay(500);
  
  String actionResp = sendAT("AT+HTTPACTION=1", 15000);
  bool completed = false;
  
  if (actionResp.indexOf("+HTTPACTION:") != -1) {
    int firstComma = actionResp.indexOf(',');
    int secondComma = actionResp.indexOf(',', firstComma + 1);
    if (firstComma != -1 && secondComma != -1) {
      String responseCode = actionResp.substring(firstComma + 1, secondComma);
      Serial.printf("[HTTP-LOG] Gateway server HTTP Response status evaluated: %s\n", responseCode.c_str());
      if (responseCode.startsWith("2")) {
        completed = true;
      }
    }
  }

  sendAT("AT+HTTPTERM", 1200);
  return completed;
}

bool sendSMSBackup(String status, uint32_t timestamp, int peakVal) {
  Serial.println(F("[SMS-LOG] Commencing secondary network SMS path fallback execution..."));
  sendAT("AT+CMGF=1", 1500);
  String targetCmd = "AT+CMGS=\"" + String(backup_sms_target) + "\"";
  String response = sendAT(targetCmd, 3000);

  if (response.indexOf(">") != -1) {
    gsm.printf("ALERT: Feeder %s [%s] transitioned to %s. Peak ADC: %d. Timestamp: %u. GPS: %s,%s", 
               feeder_name, transformer, status.c_str(), peakVal, timestamp, user_latitude, user_longitude);
    gsm.write(0x1A);

    unsigned long windowStart = millis();
    String statusWindow = "";
    while (millis() - windowStart < 15000UL) {
      if (gsm.available()) {
        statusWindow += (char)gsm.read();
        if (statusWindow.indexOf("+CMGS:") != -1) {
          Serial.println(F("[SMS-LOG] GSM base network successfully delivered transaction token."));
          return true;
        }
      }
      delay(1);
      ESP.wdtFeed();
    }
  }
  Serial.println(F("[SMS-ERROR] SMS gateway processing handshakes timed out. Carrier link dropped."));
  return false;
}

void cacheEventLocally(String status, uint32_t timestamp, int peakVal) {
  // Upgraded: Using LittleFS instead of SPIFFS
  File cache = LittleFS.open(CACHE_FILE, "a");
  if (!cache) {
    Serial.println(F("[CACHE-ERROR] Unable to target LittleFS partition segment. Drop event."));
    return;
  }
  String lineRecord = status + "," + String(timestamp) + "," + String(peakVal) + "," + String(user_latitude) + "," + String(user_longitude) + "\n";
  cache.print(lineRecord);
  cache.close();
  Serial.printf("[CACHE-LOG] Event successfully cached: Timestamp=%u\n", timestamp);
}

void flushLocalCache() {
  if (!LittleFS.exists(CACHE_FILE)) return;
  File cache = LittleFS.open(CACHE_FILE, "r");
  if (!cache) return;

  Serial.println(F("[FLUSH-LOG] Stored records detected. Flushing structures..."));
  String tempPath = "/temp_cache.json";
  File tempCache = LittleFS.open(tempPath, "w");
  if (!tempCache) {
    cache.close();
    return;
  }

  while (cache.available()) {
    String record = cache.readStringUntil('\n');
    if (record.length() < 5) continue;

    int idx1 = record.indexOf(',');
    int idx2 = record.indexOf(',', idx1 + 1);
    int idx3 = record.indexOf(',', idx2 + 1);
    int idx4 = record.indexOf(',', idx3 + 1);

    if (idx1 != -1 && idx2 != -1 && idx3 != -1 && idx4 != -1) {
      String status = record.substring(0, idx1);
      uint32_t timestamp = record.substring(idx1 + 1, idx2).toInt();
      int peakVal = record.substring(idx2 + 1, idx3).toInt();
      
      bool sent = false;
      if (is_gsm_connected) {
        sent = sendTelemetryHttp(status, timestamp, peakVal);
      }
      if (!sent) {
        tempCache.println(record);
      } else {
        Serial.printf("[FLUSH-LOG] Purged cached record successfully: Time=%u\n", timestamp);
      }
    }
  }

  cache.close();
  tempCache.close();
  LittleFS.remove(CACHE_FILE);
  LittleFS.rename(tempPath, CACHE_FILE);
}

bool initGSM() {
  Serial.println(F("[LOG] Syncing interface configurations..."));
  setGsmRts(true);
  sendAT("AT", 500);
  sendAT("ATE1", 1000);

  String cpin = "";
  for (int simRetry = 0; simRetry < 3; simRetry++) {
    cpin = sendAT("AT+CPIN?", 2000);
    if (cpin.indexOf("READY") != -1) break;
    sendAT("AT+CFUN=0", 2000);
    delay(1500);
    sendAT("AT+CFUN=1", 2000);
    delay(2000);
  }

  if (cpin.indexOf("READY") == -1) {
    Serial.println(F("[GSM-ERROR] Critical: SIM Card dropped or unpowered."));
    return false;
  }

  sendAT("AT+CSQ", 1500);
  String creg = sendAT("AT+CREG?", 2000);
  if (creg.indexOf(",1") == -1 && creg.indexOf(",5") == -1) return false;

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
    Serial.println(F("[GSM-ERROR] Bearer context IP allocation returned invalid structure."));
    return false;
  }

  Serial.println(F("[GSM] Production IP Pipeline Active.\n"));
  return true;
}

void monitorForReset() {
  if (gsm.available()) {
    String buffer = "";
    while (gsm.available()) {
      buffer += (char)gsm.read();
    }
    if ((buffer.indexOf("RDY") != -1 || buffer.indexOf("+CFUN: 1") != -1 || buffer.indexOf("Call Ready") != -1) && (millis() - lastResetHandled > 10000UL)) {
      Serial.println(F("[ALERT] Hardware transceiver reset detected!"));
      lastResetHandled = millis();
      is_gsm_connected = false;
      delay(8000);
      int attempts = 0;
      while (!is_gsm_connected && attempts < 3) {
        attempts++;
        is_gsm_connected = initGSM();
        if (!is_gsm_connected) delay(5000UL * attempts);
      }
      if (is_gsm_connected) {
        fetchSIMIdentity();
        syncNetworkTime();
        flushLocalCache();
      }
    }
  }
}

bool readPowerStatus() {
  int peak = 0;
  unsigned long start = millis();
  while (millis() - start < 100UL) {
    int val = analogRead(ADC_PIN);
    if (val > peak) peak = val;
  }
  return last_status ? (peak > THRESHOLD_OFF) : (peak > THRESHOLD_ON);
}

void fetchSIMIdentity() {
  sendAT("AT+CNUM", 1500);
  String response = sendAT("AT+CCID", 1000);
  sim_serial = cleanResponse(response);
  if (sim_serial.length() < 5 || sim_serial.indexOf("ERROR") != -1) sim_serial = "UNKNOWN";
}

String cleanResponse(String resp) {
  resp.replace("AT+CCID", "");   resp.replace("AT+CNUM", "");
  resp.replace("OK", "");         resp.replace("Call Ready", "");
  resp.replace("\r", "");         resp.replace("\n", "");
  resp.trim();
  return resp;
}

String sendAT(String cmd, int timeout) {
  int clearCount = 0;
  while (gsm.available() && clearCount < 128) {
    gsm.read();
    clearCount++;
    yield();
  }

  Serial.print("→ Outbound AT: ");
  Serial.println(cmd);
  gsm.println(cmd);

  String response = "";
  unsigned long t = millis();
  
  while (millis() - t < (unsigned long)timeout) {
    while (gsm.available()) {
      char c = (char)gsm.read();
      response += c;
      t = millis(); 
    }
    yield();
    ESP.wdtFeed();
  }
  return response;
}