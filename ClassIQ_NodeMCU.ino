#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <MFRC522.h>
#include <DHT.h>

// =====================================================
// 1. PIN CONFIGURATION (ESP8266 / NodeMCU)
// =====================================================
#define I2C_SDA_PIN      D2   // GPIO 4
#define I2C_SCL_PIN      D1   // GPIO 5

#define RFID_SS_PIN      D8   // GPIO 15
#define RFID_RST_PIN     D3   // GPIO 0

#define DHT_PIN          D4   // GPIO 2
#define DHT_TYPE         DHT11
#define SENSOR_GAS_PIN   A0   // Analog Input ADC0

#define BUZZER_PIN       D0   // GPIO 16

// =====================================================
// 2. NETWORK, FIREBASE & GOOGLE SHEETS CONFIGURATION
// =====================================================
const char* WIFI_SSID = "infiNET (5GHz)";
const char* WIFI_PASS = "11223344";

const char* FIREBASE_HOST     = "https://environment-318e8-default-rtdb.asia-southeast1.firebasedatabase.app";
const char* GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbwBjVUsUlfz3h-XzHxdFDJKYZiYrWg_TyooqTFwL6EVKY--zvL-5_eXNiHs_5WnH1q4/exec";

// =====================================================
// 3. THRESHOLD CRITERIA
// =====================================================
const float TEMP_THRESHOLD = 35.0; // In °C
const float HUM_THRESHOLD  = 80.0; // In %
const int   GAS_THRESHOLD  = 500;  // ADC value (0 - 1023)

// Peripherals Instances
LiquidCrystal_I2C lcd(0x24, 16, 2);
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
DHT dht(DHT_PIN, DHT_TYPE);

// Non-blocking Timing Variables
unsigned long lastSensorUpload    = 0;
unsigned long lastDisplayCycle    = 0;
unsigned long messageDisplayTimer = 0;
unsigned long messageHoldDuration = 6000; 

const unsigned long UPLOAD_INTERVAL  = 6000; // 6 seconds
const unsigned long DISPLAY_INTERVAL = 2000; // 2 seconds

int displayState = 0; // 0: Temp, 1: Humidity, 2: Gas
bool isShowingMessage = false;
bool hasThresholdAlert = false;

float curTemp = 0.0;
float curHum  = 0.0;
int   curGas  = 0;

// Function Prototypes
void connectWiFi();
void readAndUploadSensors();
void cycleSensorDisplay();
void handleRFIDScan();
void triggerBuzzerBeep(int durationMs);
void updateAlertState();

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);

  // Initialize Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Initialize I2C LCD
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  
  lcd.setCursor(0, 0);
  lcd.print("ClassIQ System  ");
  lcd.setCursor(0, 1);
  lcd.print("Connecting WiFi ");

  // Initialize Hardware Modules
  SPI.begin();
  rfid.PCD_Init();
  dht.begin();

  connectWiFi();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("ClassIQ System  ");
  lcd.setCursor(0, 1);
  lcd.print("System Ready!   ");
  delay(1200);
  lcd.clear();
}

// =====================================================
// MAIN LOOP
// =====================================================
void loop() {
  unsigned long currentMillis = millis();

  // 1. Every 6 Seconds: Read & Upload Sensor Data to Firebase
  if (currentMillis - lastSensorUpload >= UPLOAD_INTERVAL) {
    lastSensorUpload = currentMillis;
    readAndUploadSensors();
  }

  // 2. Continuous Safety Threshold & Constant Beep Check
  updateAlertState();

  // 3. Check RFID Reader for New Card Taps
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    handleRFIDScan();
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }

  // 4. Clear RFID Welcome or OTP Messages after hold duration ends
  if (isShowingMessage && (millis() - messageDisplayTimer >= messageHoldDuration)) {
    isShowingMessage = false;
    lcd.clear();
  }

  // 5. Normal Mode: Cycle LCD Sensor Values Every 2 Seconds
  if (!isShowingMessage && (currentMillis - lastDisplayCycle >= DISPLAY_INTERVAL)) {
    lastDisplayCycle = currentMillis;
    cycleSensorDisplay();
  }
}

// =====================================================
// WIFI CONNECTION HANDLER
// =====================================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi Connecting...");
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

// =====================================================
// SENSOR READINGS & FIREBASE UPLOAD
// =====================================================
void readAndUploadSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  int g   = analogRead(SENSOR_GAS_PIN);

  if (!isnan(t)) curTemp = t;
  if (!isnan(h)) curHum  = h;
  curGas = g;

  Serial.printf("Sensors -> Temp: %.1fC | Hum: %.1f%% | Gas: %d\n", curTemp, curHum, curGas);

  // Firebase Realtime DB Upload
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    String firebaseUrl = String(FIREBASE_HOST) + "/environment.json";
    
    http.begin(client, firebaseUrl);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<200> doc;
    doc["temperature"] = curTemp;
    doc["humidity"]    = curHum;
    doc["gas_level"]   = curGas;
    doc["timestamp"]   = millis();

    String reqBody;
    serializeJson(doc, reqBody);

    int httpCode = http.PUT(reqBody);
    Serial.printf("Firebase Realtime DB Updated! Status: %d\n", httpCode);

    http.end();
  }
}

// =====================================================
// CONTINUOUS ALERT & CONSTANT BEEP HANDLER FOR ANY COMBO
// =====================================================
void updateAlertState() {
  bool isTempHigh = (curTemp > TEMP_THRESHOLD);
  bool isHumHigh  = (curHum > HUM_THRESHOLD);
  bool isGasHigh  = (curGas > GAS_THRESHOLD);

  if (isTempHigh || isHumHigh || isGasHigh) {
    hasThresholdAlert = true;
    
    // Constant Continuous Beep Alarm
    digitalWrite(BUZZER_PIN, HIGH);

    // Dynamic Alert Text Formulation for Combos
    String alertMsg = "HIGH: ";
    if (isTempHigh && isHumHigh && isGasHigh) alertMsg += "ALL SENSORS!";
    else if (isTempHigh && isGasHigh) alertMsg += "TEMP+GAS!   ";
    else if (isTempHigh && isHumHigh) alertMsg += "TEMP+HUM!   ";
    else if (isHumHigh && isGasHigh)  alertMsg += "HUM+GAS!    ";
    else if (isTempHigh)              alertMsg += "TEMP LEVEL! ";
    else if (isHumHigh)               alertMsg += "HUMIDITY!   ";
    else if (isGasHigh)               alertMsg += "GAS LEVEL!  ";

    // Update Alert on Display if not showing RFID scan message
    if (!isShowingMessage) {
      lcd.setCursor(0, 0);
      lcd.print("!! ALERT SENSOR !!");
      lcd.setCursor(0, 1);
      
      while (alertMsg.length() < 16) alertMsg += " ";
      lcd.print(alertMsg.substring(0, 16));
    }
  } else {
    if (hasThresholdAlert) {
      hasThresholdAlert = false;
      digitalWrite(BUZZER_PIN, LOW);
      lcd.clear();
    }
  }
}

// =====================================================
// LCD DISPLAY CYCLING (Clean Fixed Formatting)
// =====================================================
void cycleSensorDisplay() {
  if (hasThresholdAlert) return;

  lcd.setCursor(0, 0);
  lcd.print("ClassIQ Monitor ");

  lcd.setCursor(0, 1);
  switch (displayState) {
    case 0: {
      String line = "Temp : " + String(curTemp, 1) + " C     ";
      lcd.print(line.substring(0, 16));
      displayState = 1;
      break;
    }
    case 1: {
      String line = "Humid: " + String(curHum, 1) + " %     ";
      lcd.print(line.substring(0, 16));
      displayState = 2;
      break;
    }
    case 2: {
      String line = "Gas  : " + String(curGas) + " PPM     ";
      lcd.print(line.substring(0, 16));
      displayState = 0;
      break;
    }
  }
}

// =====================================================
// RFID SCAN, GOOGLE SHEETS QUERY & OTP RESPONSE HANDLER
// =====================================================
void handleRFIDScan() {
  triggerBuzzerBeep(150);

  String cardUID = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) cardUID += "0";
    cardUID += String(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) cardUID += " ";
  }
  cardUID.toUpperCase();
  Serial.println("Scanned RFID: " + cardUID);

  // Set message flag BEFORE starting HTTP request
  isShowingMessage = true;
  messageDisplayTimer = millis();
  messageHoldDuration = 10000; // Allow enough time for network response
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("RFID Detected   ");
  lcd.setCursor(0, 1);
  lcd.print("Verifying...    ");

  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    String encodedUID = cardUID;
    encodedUID.replace(" ", "%20");

    String requestUrl = String(GOOGLE_SCRIPT_URL) + "?action=scan&uid=" + encodedUID;
    
    http.begin(client, requestUrl);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    int httpCode = http.GET();

    if (httpCode > 0) {
      String response = http.getString();
      Serial.println("Google Sheets Response: " + response);

      StaticJsonDocument<300> resDoc;
      DeserializationError error = deserializeJson(resDoc, response);

      if (!error) {
        bool registered = resDoc["registered"] | false;

        lcd.clear();
        if (registered) {
          // 1. REGISTERED USER: Show Welcome Screen (6 Seconds Hold)
          const char* studentName = resDoc["name"] | "Student";
          
          lcd.setCursor(0, 0);
          lcd.print("Welcome!        ");
          
          char line1[17];
          snprintf(line1, sizeof(line1), "%-16s", studentName);
          lcd.setCursor(0, 1);
          lcd.print(line1);

          // Start hold timer AFTER updating display
          messageDisplayTimer = millis();
          messageHoldDuration = 6000; // 6 Seconds Hold
        } else {
          // 2. UNREGISTERED USER: Show OTP Screen (8 Seconds Hold)
          const char* otp = resDoc["otp"] | "0000";
          
          lcd.setCursor(0, 0);
          lcd.print("Register Name   ");
          
          char line1[17];
          snprintf(line1, sizeof(line1), "OTP: %-11s", otp);
          lcd.setCursor(0, 1);
          lcd.print(line1);

          // Start hold timer AFTER updating display
          messageDisplayTimer = millis();
          messageHoldDuration = 8000; // 8 Seconds Hold
        }
      } else {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("JSON Parse Err  ");
        messageDisplayTimer = millis();
        messageHoldDuration = 3000;
      }
    } else {
      Serial.printf("Google Script HTTP Error: %s\n", http.errorToString(httpCode).c_str());
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Server Timeout! ");
      messageDisplayTimer = millis();
      messageHoldDuration = 3000;
    }
    http.end();
  }
}

// =====================================================
// BUZZER SHORT BEEP
// =====================================================
void triggerBuzzerBeep(int durationMs) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(durationMs);
  digitalWrite(BUZZER_PIN, LOW);
}