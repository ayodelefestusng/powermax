#include <Arduino.h>
#include <SoftwareSerial.h>

// --- GSM modem pins for Wemos D1 Mini ---
const int GSM_RX_PIN = D5; // SIM TX -> Wemos RX
const int GSM_TX_PIN = D6; // SIM RX -> Wemos TX
const int GSM_RTS_PIN = D8; // RTS / sleep control pin

SoftwareSerial gsm(GSM_RX_PIN, GSM_TX_PIN);

const char* CURRENT_APN = "9mobile";
const char* TEST_URL = "http://24.144.119.35:8000/utility/";

bool is_gsm_connected = false;

void setGsmRts(bool enabled) {
  digitalWrite(GSM_RTS_PIN, enabled ? LOW : HIGH);
  Serial.printf("[LOG] RTS set to %s\n", enabled ? "LOW (Enabled)" : "HIGH (Disabled)");
}

void clearGSMBuffer() {
  int clearCount = 0;
  while (gsm.available() && clearCount < 256) {
    gsm.read();
    clearCount++;
    yield();
  }
}

String sendAT(String cmd, int timeout) {
  String response = "";
  clearGSMBuffer();
  
  Serial.print(F("-> AT: "));
  Serial.println(cmd);
  gsm.println(cmd);

  unsigned long start = millis();
  while (millis() - start < (unsigned long)timeout) {
    while (gsm.available()) {
      char c = (char)gsm.read();
      response += c;
      start = millis();
    }
    yield();
    ESP.wdtFeed();
  }

  Serial.print(F("<- RESP: "));
  Serial.println(response);
  return response;
}

void sendATAsync(String cmd) {
  clearGSMBuffer();
  Serial.print(F("-> AT (Async): "));
  Serial.println(cmd);
  gsm.println(cmd);
}

// Improved URC reader that waits for the full status line including newline
String waitForHTTPAction(int timeout) {
  String response = "";
  unsigned long start = millis();
  
  while (millis() - start < (unsigned long)timeout) {
    while (gsm.available()) {
      char c = (char)gsm.read();
      response += c;
      start = millis(); 
    }
    // Only break once +HTTPACTION: is followed by a newline, ensuring complete payload
    if (response.indexOf("+HTTPACTION:") != -1 && response.indexOf("\n", response.indexOf("+HTTPACTION:")) != -1) {
      break;
    }
    yield();
    ESP.wdtFeed();
  }
  
  Serial.print(F("<- URC CAPTURED: "));
  Serial.println(response);
  return response;
}

bool initGSM() {
  Serial.println(F("[GSM] Initializing modem for HTTP connection..."));
  setGsmRts(true);

  sendAT("AT", 500);
  sendAT("ATE1", 1000);

  String cpin = sendAT("AT+CPIN?", 2000);
  if (cpin.indexOf("READY") == -1) {
    Serial.println(F("[GSM-ERROR] SIM card not ready or missing."));
    return false;
  }

  String creg = sendAT("AT+CREG?", 2000);
  if (creg.indexOf(",1") == -1 && creg.indexOf(",5") == -1) {
    Serial.println(F("[GSM-ERROR] Network registration failed."));
    return false;
  }

  sendAT("AT+CIPSHUT", 3000);
  sendAT("AT+CGATT=1", 3000);
  sendAT("AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\"", 2000);
  sendAT("AT+SAPBR=3,1,\"APN\",\"" + String(CURRENT_APN) + "\"", 2000);
  sendAT("AT+SAPBR=0,1", 2000);
  delay(500);
  sendAT("AT+SAPBR=1,1", 5000);

  String sapbrCheck = sendAT("AT+SAPBR=2,1", 3000);
  if (sapbrCheck.indexOf("0.0.0.0") != -1 || sapbrCheck.indexOf("ERROR") != -1) {
    Serial.println(F("[GSM-ERROR] GPRS bearer initialization failed."));
    return false;
  }

  Serial.println(F("[GSM] GPRS network connection established."));
  return true;
}

void callUtilityEndpoint() {
  if (!is_gsm_connected) {
    is_gsm_connected = initGSM();
    if (!is_gsm_connected) {
      Serial.println(F("[HTTP-ERROR] Cannot proceed. GSM initialization failed."));
      return;
    }
  }

  Serial.print(F("[HTTP] Executing GET request to: "));
  Serial.println(TEST_URL);

  sendAT("AT+HTTPTERM", 1000);
  delay(150);

  String initResp = sendAT("AT+HTTPINIT", 1500);
  if (initResp.indexOf("ERROR") != -1) {
    Serial.println(F("[HTTP-ERROR] Modem HTTP service startup failed."));
    return;
  }

  sendAT("AT+HTTPPARA=\"CID\",1", 1200);
  sendAT("AT+HTTPPARA=\"URL\",\"" + String(TEST_URL) + "\"", 1500);

  sendATAsync("AT+HTTPACTION=0");
  String actionResp = waitForHTTPAction(25000);

  if (actionResp.indexOf(",200,") != -1) {
    Serial.println(F("[HTTP-SUCCESS] Endpoint reachable (HTTP 200 OK)."));
    String body = sendAT("AT+HTTPREAD", 5000);
    Serial.println(F("[HTTP] Payload Response Body:"));
    Serial.println(body);
  } else if (actionResp.indexOf(",601,") != -1) {
    Serial.println(F("[HTTP-ERROR] 601 Network Error: Target host unreachable or port 8000 blocked by carrier."));
  } else if (actionResp.indexOf("+HTTPACTION:") != -1) {
    Serial.println(F("[HTTP-ERROR] Server responded with non-200 status code."));
  } else {
    Serial.println(F("[HTTP-ERROR] Request timed out or serial data corrupted."));
  }

  sendAT("AT+HTTPTERM", 1200);
}

void setup() {
  Serial.begin(115200);
  pinMode(GSM_RTS_PIN, OUTPUT);
  setGsmRts(true);
  gsm.begin(4800);
  delay(2000);

  Serial.println(F("\n=== SIM HTTP Utility Confirmation Test ==="));
  callUtilityEndpoint();
}

void loop() {
  delay(30000);
  callUtilityEndpoint();
}