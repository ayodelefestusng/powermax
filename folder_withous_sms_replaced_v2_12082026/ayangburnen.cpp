#include <Arduino.h>
#include <Ds1302.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <SoftwareSerial.h>
#include <WiFiClient.h>

// ==========================================
// HARDWARE PIN MAPPINGS (ESP8266 / D1 MINI)
// ==========================================
#define SENSOR_ADC_PIN A0

// GSM SIM900/SIM800 Pins
#define GSM_RX_PIN D5  // ESP RX  <-- SIM TX
#define GSM_TX_PIN D6  // ESP TX  --> SIM RX
#define GSM_RTS_PIN D8 // SIM RTS Control

// DS1302 RTC Pins
#define RTC_CLK_PIN D2 // Clock
#define RTC_DAT_PIN D1 // Data
#define RTC_RST_PIN D7 // Reset / Chip Enable

// ==========================================
// CALIBRATED HARDWARE THRESHOLDS & FILTERING
// ==========================================
const int ADC_THRESHOLD =
    890; // Threshold above idle noise floor (~2.87V) to trip ON
const int ADC_OFF_BASE =
    860; // Upper boundary of DC baseline noise (~2.77V) to trip OFF
const int CONSECUTIVE_SAMPLES = 3; // Debounce counter for signal stability

#define AC_SAMPLE_WINDOW_MS 100 // 100ms window captures 5 full 50Hz AC cycles

// ==========================================
// NETWORK & SERVER CONFIGURATION
// ==========================================
const int LOCAL_TIME_OFFSET_SECONDS =
    3600; // UTC+1 (Nigeria / West Africa Time)

const char *SERVER_IP = "24.144.119.35";
const char *SERVER_PORT = "8000";
const char *CURRENT_APN = "web.gprs.mtnnigeria.net";

// WiFi Credentials
const char *wifi_ssid = "V40 Lite";
const char *wifi_password = "@Ajibandele61";
const unsigned long WIFI_TIMEOUT_MS = 15000UL;

// Static Node Metadata
const char *FEEDER_NAME = "Ayangbunren";
const char *TRANSFORMER_NAME = "Oremolu DT";
const char *USER_LATITUDE = "6.5230";
const char *USER_LONGITUDE = "3.3420";

// ==========================================
// STORAGE CONSTANTS
// ==========================================
#define CACHE_FILE_PATH "/telemetry_cache.json"

// ==========================================
// GLOBAL HARDWARE OBJECTS
// ==========================================
SoftwareSerial gsmSerialPort(GSM_RX_PIN, GSM_TX_PIN);
Ds1302 rtc(RTC_RST_PIN, RTC_CLK_PIN, RTC_DAT_PIN);

// Global State
bool lastPowerStatus = false;
bool isWifiConnected = false;
bool isGsmConnected = false;
bool isRtcAvailable = false;
String simSerial = "UNKNOWN";

uint32_t networkBaseEpoch = 0;
unsigned long epochSyncMillis = 0;
unsigned long lastResetHandled = 0;

// Debounce Tracking
int consecutiveSampleCount = 0;
bool pendingTargetStatus = false;

// ==========================================
// LOGGING UTILITIES
// ==========================================
void logInfo(const String &msg) {
  Serial.printf("[LOG_INFO] %s\n", msg.c_str());
}

void logError(const String &msg) {
  Serial.printf("[LOG_ERROR] %s\n", msg.c_str());
}

bool isRtcDateTimeInvalid(const Ds1302::DateTime &dt) {
  return dt.year == 0 && dt.month == 0 && dt.day == 0 && dt.hour == 0 &&
         dt.minute == 0 && dt.second == 0;
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

  struct tm *timeInfo = localtime(&now);
  if (timeInfo == nullptr) {
    return false;
  }

  Ds1302::DateTime rtcNow = {0};
  rtcNow.year = timeInfo->tm_year % 100;
  rtcNow.month = timeInfo->tm_mon + 1;
  rtcNow.day = timeInfo->tm_mday;
  rtcNow.hour = timeInfo->tm_hour;
  rtcNow.minute = timeInfo->tm_min;
  rtcNow.second = timeInfo->tm_sec;
  rtcNow.dow = timeInfo->tm_wday + 1;

  rtc.setDateTime(&rtcNow);
  logInfo("RTC synchronized from NTP time: " + String(rtcNow.year) + "/" +
          String(rtcNow.month) + "/" + String(rtcNow.day) + " " +
          String(rtcNow.hour) + ":" + String(rtcNow.minute) + ":" +
          String(rtcNow.second));
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

  Serial.printf("[RTC] 20%02d-%02d-%02d %02d:%02d:%02d\n", dt.year, dt.month,
                dt.day, dt.hour, dt.minute, dt.second);
}

// ==========================================
// GSM RTS & AT COMMUNICATION ENGINE
// ==========================================
void setGsmRts(bool enabled) {
  digitalWrite(GSM_RTS_PIN, enabled ? LOW : HIGH);
}

String sendAT(const String &cmd, uint32_t timeoutMs) {
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
        t = millis(); // Refresh timeout on incoming byte
      }
      yield();
      ESP.wdtFeed();
    }

    String trimmedResp = response;
    trimmedResp.trim();
    if (trimmedResp.length() > 0) {
      logInfo("Inbound AT Response <- " + trimmedResp);
    } else {
      logInfo("Inbound AT Response <- [NO RESPONSE / TIMEOUT] (GSM module may "
              "be unavailable)");
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
  logInfo("Connecting to WiFi SSID: " + String(wifi_ssid));
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         (millis() - start) < WIFI_TIMEOUT_MS) {
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
      if (cpin.indexOf("READY") != -1)
        break;
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
    if (sapbrCheck.indexOf("0.0.0.0") != -1 ||
        sapbrCheck.indexOf("ERROR") != -1) {
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
    if (i % 4 == 0 && (i % 100 != 0 || i % 400 == 0))
      numDays++;
  }
  for (uint8_t i = 0; i < month - 1; i++) {
    numDays += daysInMonth[i];
  }
  numDays += day - 1;

  return (uint32_t)((numDays * 86400UL) + (hour * 3600UL) + (minute * 60UL) +
                    second);
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

        uint16_t year = 2000 + rtcDate.substring(0, 2).toInt();
        uint8_t month = rtcDate.substring(3, 5).toInt();
        uint8_t day = rtcDate.substring(6, 8).toInt();
        uint8_t hour = rtcTime.substring(0, 2).toInt();
        uint8_t minute = rtcTime.substring(3, 5).toInt();
        uint8_t second = rtcTime.substring(6, 8).toInt();

        if (year >= 2026 && month > 0 && day > 0) {
          if (isRtcAvailable) {
            Ds1302::DateTime netTime = {0};
            netTime.year = year - 2000;
            netTime.month = month;
            netTime.day = day;
            netTime.hour = hour;
            netTime.minute = minute;
            netTime.second = second;
            rtc.setDateTime(&netTime);
            logInfo("Synchronized hardware DS1302 with Cell Tower time.");
          }

          uint32_t numDays = 0;
          uint8_t daysInMonth[] = {31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31};
          if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) {
            daysInMonth[1] = 29;
          }
          for (uint16_t i = 1970; i < year; i++) {
            numDays += 365;
            if (i % 4 == 0 && (i % 100 != 0 || i % 400 == 0))
              numDays++;
          }
          for (uint8_t i = 0; i < month - 1; i++) {
            numDays += daysInMonth[i];
          }
          numDays += day - 1;

          networkBaseEpoch =
              (numDays * 86400UL) + (hour * 3600UL) + (minute * 60UL) + second;
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

// Continuous 100ms peak evaluation over 5 full 50Hz AC cycles
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

bool readPowerStatus() {
  try {
    uint16_t peakVal = getFilteredAnalogPeak();

    // Standard hysteresis evaluation
    if (peakVal >= ADC_THRESHOLD) {
      return true;
    } else if (peakVal <= ADC_OFF_BASE) {
      return false;
    }

    return lastPowerStatus; // Retain current state inside hysteresis margin
  } catch (...) {
    logError("Exception reading power analog status.");
    return lastPowerStatus;
  }
}

void cacheEventLocally(const String &status, uint32_t timestamp,
                       uint16_t peakVal) {
  try {
    File cache = LittleFS.open(CACHE_FILE_PATH, "a");
    if (!cache) {
      logError("Unable to target LittleFS partition. Dropping record.");
      return;
    }
    String lineRecord = status + "," + String(timestamp) + "," +
                        String(peakVal) + "," + String(USER_LATITUDE) + "," +
                        String(USER_LONGITUDE) + "\n";
    cache.print(lineRecord);
    cache.close();
    logInfo("Event successfully cached locally: Timestamp=" +
            String(timestamp));
  } catch (...) {
    logError("Exception committing payload to local cache.");
  }
}

// ==========================================
// DISPATCH PIPELINES: WIFI & GPRS HTTP
// ==========================================
bool sendTelemetryHttpWiFi(const String &status, uint32_t timestamp,
                           uint16_t peakVal) {
  if (WiFi.status() != WL_CONNECTED)
    return false;

  try {
    String payload = "{";
    payload += "\"stat\":\"" + status + "\",";
    payload += "\"timestamp\":" + String(timestamp) + ",";
    payload += "\"val\":" + String(peakVal) + ",";
    payload += "\"fdr\":\"" + String(FEEDER_NAME) + "\",";
    payload += "\"tf\":\"" + String(TRANSFORMER_NAME) + "\",";
    payload += "\"ccid\":\"" + simSerial + "\"";
    payload += "}";

    String url = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) +
                 "/power-tracker-gateway/";
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

bool sendTelemetryHttpGPRS(const String &status, uint32_t timestamp,
                           uint16_t peakVal) {
  try {
    if (!isGsmConnected) {
      isGsmConnected = initGSM();
      if (!isGsmConnected)
        return false;
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
    String url = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) +
                 "/power-tracker-gateway/";
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

void flushLocalCache() {
  try {
    if (!LittleFS.exists(CACHE_FILE_PATH))
      return;
    File cache = LittleFS.open(CACHE_FILE_PATH, "r");
    if (!cache)
      return;

    logInfo(
        "Stored cache records detected. Processing and flushing pipeline...");
    String tempPath = "/temp_cache.json";
    File tempCache = LittleFS.open(tempPath, "w");
    if (!tempCache) {
      cache.close();
      return;
    }

    while (cache.available()) {
      String record = cache.readStringUntil('\n');
      if (record.length() < 5)
        continue;

      int idx1 = record.indexOf(',');
      int idx2 = record.indexOf(',', idx1 + 1);
      int idx3 = record.indexOf(',', idx2 + 1);
      int idx4 = record.indexOf(',', idx3 + 1);

      if (idx1 != -1 && idx2 != -1 && idx3 != -1 && idx4 != -1) {
        String status = record.substring(0, idx1);
        uint32_t timestamp = record.substring(idx1 + 1, idx2).toInt();
        uint16_t peakVal = record.substring(idx2 + 1, idx3).toInt();

        // Drop corrupted timestamps or re-calibrate status against latest
        // thresholds
        if (timestamp == 2147483647UL) {
          logInfo("Dropping invalid/corrupted cache record.");
          continue;
        }

        if (peakVal >= ADC_THRESHOLD) {
          status = "on";
        } else if (peakVal <= ADC_OFF_BASE) {
          status = "off";
        }

        bool sent = false;
        if (isWifiConnected || WiFi.status() == WL_CONNECTED) {
          sent = sendTelemetryHttpWiFi(status, timestamp, peakVal);
        }
        if (!sent) {
          if (isGsmConnected) {
            sent = sendTelemetryHttpGPRS(status, timestamp, peakVal);
          }
        }
        if (!sent) {
          tempCache.println(record);
        } else {
          logInfo("Purged cached record successfully: Time=" +
                  String(timestamp));
        }
      }
    }

    cache.close();
    tempCache.close();
    LittleFS.remove(CACHE_FILE_PATH);
    LittleFS.rename(tempPath, CACHE_FILE_PATH);
  } catch (...) {
    logError("Exception encountered during local cache flush.");
  }
}

void processPowerEvent(bool status) {
  try {
    uint32_t eventTime = getCurrentTimestamp();
    uint16_t currentPeak = getFilteredAnalogPeak();
    String stateStr = status ? "on" : "off";

    logInfo("Router invoked for event timestamp: " + String(eventTime));

    // 1. Try WiFi HTTP first
    if (isWifiConnected || WiFi.status() == WL_CONNECTED) {
      if (sendTelemetryHttpWiFi(stateStr, eventTime, currentPeak)) {
        logInfo("Stage 1 (WiFi HTTP) delivery confirmed successful.");
        flushLocalCache();
        return;
      }
      logError("Stage 1 (WiFi HTTP) failed. Falling back to Stage 2 (GPRS)...");
    }

    // 2. Try GSM GPRS HTTP second
    if (!isGsmConnected) {
      isGsmConnected = initGSM();
    }
    if (isGsmConnected) {
      if (sendTelemetryHttpGPRS(stateStr, eventTime, currentPeak)) {
        logInfo("Stage 2 (GSM GPRS HTTP) delivery confirmed successful.");
        flushLocalCache();
        return;
      }
      logError("Stage 2 (GSM GPRS) failed.");
    }

    // 3. Fallback to local LittleFS Cache
    logError("All live delivery routes failed. Committing packet to LittleFS "
             "backup storage...");
    cacheEventLocally(stateStr, eventTime, currentPeak);
  } catch (...) {
    logError("Exception executing processPowerEvent workflow.");
  }
}

void monitorForReset() {
  try {
    if (gsmSerialPort.available()) {
      String buffer = "";
      while (gsmSerialPort.available()) {
        buffer += (char)gsmSerialPort.read();
      }
      if ((buffer.indexOf("RDY") != -1 || buffer.indexOf("+CFUN: 1") != -1 ||
           buffer.indexOf("Call Ready") != -1) &&
          (millis() - lastResetHandled > 10000UL)) {
        logError("Hardware transceiver reset detected!");
        lastResetHandled = millis();
        isGsmConnected = false;
        delay(8000);
        uint8_t attempts = 0;
        while (!isGsmConnected && attempts < 3) {
          attempts++;
          isGsmConnected = initGSM();
          if (!isGsmConnected)
            delay(5000UL * attempts);
        }
        if (isGsmConnected) {
          fetchSIMIdentity();
          syncNetworkTime();
          flushLocalCache();
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

    logInfo("Booting Feeder Power Monitor Node...");

    if (!LittleFS.begin()) {
      logError("LittleFS mount failed! Backup local storage unavailable.");
    }

    rtc.init();
    Ds1302::DateTime now;
    rtc.getDateTime(&now);
    logInfo("DS1302 RTC initialized.");
    isRtcAvailable = true;

    delay(1000);

    gsmSerialPort.println("AT+IPR=4800");
    delay(500);
    while (gsmSerialPort.available()) {
      gsmSerialPort.read();
    }

    sendAT("AT+CLTS=1", 1000);
    sendAT("AT&W", 1000);

    // Stage 1: Try WiFi connection
    isWifiConnected = initWiFi();

    if (isWifiConnected) {
      configTime(LOCAL_TIME_OFFSET_SECONDS, 0, "pool.ntp.org",
                 "time.google.com");
      logInfo("NTP time sync requested via WiFi for UTC+1 local time.");
      if (waitForNtpSync(15000)) {
        syncRtcFromNtp();
      } else {
        logError("NTP time sync did not complete within timeout.");
      }
      flushLocalCache();
    } else {
      logInfo("WiFi unavailable — attempting GSM GPRS pipeline...");
      uint8_t initAttempts = 0;
      while (!isGsmConnected) {
        initAttempts++;
        isGsmConnected = initGSM();
        if (!isGsmConnected) {
          logError("Base GPRS Pipeline Failed (Attempt " +
                   String(initAttempts) + "). Retrying...");
          delay(5000);
        }
        if (initAttempts >= 3 && !isGsmConnected)
          break;
      }

      if (isGsmConnected) {
        fetchSIMIdentity();
        syncNetworkTime();
        flushLocalCache();
      }
    }

    lastPowerStatus = readPowerStatus();
    pendingTargetStatus = lastPowerStatus;
    logInfo("Initial Power Status -> " +
            String(lastPowerStatus ? "ON" : "OFF"));
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

    bool currentReading = readPowerStatus();

    if (currentReading != lastPowerStatus) {
      if (currentReading == pendingTargetStatus) {
        consecutiveSampleCount++;
      } else {
        pendingTargetStatus = currentReading;
        consecutiveSampleCount = 1;
      }

      if (consecutiveSampleCount >= CONSECUTIVE_SAMPLES) {
        lastPowerStatus = currentReading;
        consecutiveSampleCount = 0;
        logInfo(
            "Power state transition confirmed (" + String(CONSECUTIVE_SAMPLES) +
            " samples). New State: " + String(lastPowerStatus ? "ON" : "OFF"));
        processPowerEvent(lastPowerStatus);
      }
    } else {
      consecutiveSampleCount = 0;
      pendingTargetStatus = lastPowerStatus;
    }

    // Monitor WiFi connection state transitions
    if (!isWifiConnected && WiFi.status() == WL_CONNECTED) {
      isWifiConnected = true;
      logInfo("WiFi reconnected.");
    } else if (isWifiConnected && WiFi.status() != WL_CONNECTED) {
      isWifiConnected = false;
      logError("WiFi connection lost.");
    }

    if (!isWifiConnected) {
      monitorForReset();
    }
    delay(100);
  } catch (...) {
    logError("Exception inside main system loop.");
  }
}