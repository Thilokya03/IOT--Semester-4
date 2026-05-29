/*
  Smart Hospital Automated Chemical Dosing System
  DISPENSING TANK EDGE NODE - T01

  Fixed:
  - 1 kg empty tank = 0%
  - 2 kg full tank = 100%
  - HX711_NOT_READY is no longer treated as an immediate fault
  - Uses last valid reading if HX711 is temporarily not ready
  - Yellow LED = tank missing
  - Red LED = real fault only
  - LED bar graph shows dispensing tank level
  - Publishes MQTTS telemetry compatible with main component
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "HX711.h"

// ---------------- WiFi / MQTT ----------------

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

const char* MQTT_HOST = "broker.hivemq.com";
const int MQTT_PORT = 8883;

const char* PROJECT_PREFIX = "shacd/thilokya/demo01";
const char* NODE_ID = "T03";

String topicTelemetry;
String topicRequest;
String topicValveCommand;
String topicNodeStatus;

// ---------------- Pins ----------------

#define HX711_DT_PIN        12
#define HX711_SCK_PIN       15

#define VALVE_PIN           26

#define YELLOW_FAULT_LED    18
#define RED_FAULT_LED       19

#define BAR_DATA_PIN        27
#define BAR_CLOCK_PIN       25
#define BAR_LATCH_PIN       33

// ---------------- Tank Settings ----------------

const float TANK_CAPACITY_ML = 750.0;

const float EMPTY_TANK_WEIGHT_KG = 1.0;
const float FULL_TANK_WEIGHT_KG  = 2.0;

const float TANK_MISSING_LIMIT_KG = 0.70;

const float REFILL_THRESHOLD_PERCENT = 20.0;
const float TARGET_LEVEL_PERCENT = 95.0;

// ---------------- Timing ----------------

const unsigned long SENSOR_READ_INTERVAL_MS = 250;
const unsigned long TELEMETRY_INTERVAL_MS = 1000;
const unsigned long REQUEST_INTERVAL_MS = 5000;

const unsigned long COMMAND_HEARTBEAT_TIMEOUT_MS = 5000;
const unsigned long VALVE_MAX_OPEN_TIME_MS = 120000;

// HX711 is allowed to be not-ready temporarily.
// Only fault if there is no valid read for this long.
const unsigned long HX711_TIMEOUT_MS = 5000;

// ---------------- Objects ----------------

HX711 scale;
WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);

// ---------------- Runtime Variables ----------------

float measuredWeightKg = 1.0;
float liquidWeightKg = 0.0;
float volumeML = 0.0;
float levelPercent = 0.0;

bool valveOpen = false;

bool tankMissingFault = false;
bool otherFault = false;
String faultMessage = "NONE";

unsigned long lastSensorReadMs = 0;
unsigned long lastGoodHx711ReadMs = 0;
unsigned long lastTelemetryMs = 0;
unsigned long lastRequestMs = 0;
unsigned long lastCommandMs = 0;
unsigned long valveOpenedMs = 0;
unsigned long lastReconnectAttemptMs = 0;

// ---------------- Utility ----------------

float mapFloat(float x, float inMin, float inMax, float outMin, float outMax) {
  return (x - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

void setValve(bool openValve) {
  valveOpen = openValve;
  digitalWrite(VALVE_PIN, valveOpen ? HIGH : LOW);

  if (valveOpen) {
    valveOpenedMs = millis();
  }

  Serial.print("Valve ");
  Serial.println(valveOpen ? "OPENED" : "CLOSED");
}

void closeValveFailSafe() {
  if (valveOpen) {
    setValve(false);
  } else {
    digitalWrite(VALVE_PIN, LOW);
    valveOpen = false;
  }
}

void updateFaultLEDs() {
  digitalWrite(YELLOW_FAULT_LED, tankMissingFault ? HIGH : LOW);
  digitalWrite(RED_FAULT_LED, otherFault ? HIGH : LOW);

  if (tankMissingFault) {
    faultMessage = "TANK_MISSING";
  } else if (!otherFault) {
    faultMessage = "NONE";
  }
}

void setTankMissingFault(bool active) {
  tankMissingFault = active;

  if (tankMissingFault) {
    closeValveFailSafe();
  }

  updateFaultLEDs();
}

void setOtherFault(bool active, String msg) {
  otherFault = active;

  if (otherFault) {
    faultMessage = msg;
    closeValveFailSafe();
  }

  updateFaultLEDs();
}

// ---------------- LED Bar Graph ----------------

void writeBarShiftRegisters(byte srLow, byte srHigh) {
  digitalWrite(BAR_LATCH_PIN, LOW);
  shiftOut(BAR_DATA_PIN, BAR_CLOCK_PIN, MSBFIRST, srHigh);
  shiftOut(BAR_DATA_PIN, BAR_CLOCK_PIN, MSBFIRST, srLow);
  digitalWrite(BAR_LATCH_PIN, HIGH);
}

int segmentsFromPercent(float percent) {
  percent = constrain(percent, 0.0, 100.0);

  if (percent <= 0.5) return 0;

  int segments = ceil(percent / 10.0);
  return constrain(segments, 0, 10);
}

void updateTankBarGraph() {
  int segments = segmentsFromPercent(levelPercent);

  byte srLow = 0;
  byte srHigh = 0;

  for (int i = 0; i < segments; i++) {
    if (i < 8) {
      bitSet(srLow, i);
    } else {
      bitSet(srHigh, i - 8);
    }
  }

  writeBarShiftRegisters(srLow, srHigh);
}

// ---------------- Level Calculation ----------------

void calculateLevelFromWeight(float kg) {
  measuredWeightKg = kg;

  if (measuredWeightKg < 0) {
    measuredWeightKg = 0;
  }

  if (measuredWeightKg < TANK_MISSING_LIMIT_KG) {
    liquidWeightKg = 0;
    volumeML = 0;
    levelPercent = 0;

    setTankMissingFault(true);
    updateTankBarGraph();
    return;
  }

  setTankMissingFault(false);

  liquidWeightKg = measuredWeightKg - EMPTY_TANK_WEIGHT_KG;

  if (liquidWeightKg < 0) {
    liquidWeightKg = 0;
  }

  float fullLiquidWeightKg = FULL_TANK_WEIGHT_KG - EMPTY_TANK_WEIGHT_KG;

  levelPercent = (liquidWeightKg / fullLiquidWeightKg) * 100.0;
  levelPercent = constrain(levelPercent, 0.0, 100.0);

  volumeML = (levelPercent / 100.0) * TANK_CAPACITY_ML;
  volumeML = constrain(volumeML, 0.0, TANK_CAPACITY_ML);

  updateTankBarGraph();
}

void readTankLevel() {
  unsigned long now = millis();

  if (now - lastSensorReadMs < SENSOR_READ_INTERVAL_MS) {
    return;
  }

  lastSensorReadMs = now;

  if (!scale.is_ready()) {
    // Important fix:
    // HX711 not-ready is normal between conversions.
    // Do not immediately set fault.
    if (now - lastGoodHx711ReadMs > HX711_TIMEOUT_MS) {
      setOtherFault(true, "HX711_TIMEOUT");
    }

    return;
  }

  long raw = scale.read();

  /*
    In Wokwi, this scale value maps the HX711 simulated weight near kg.
    If your serial value does not match the Wokwi slider, adjust this number.
  */
  measuredWeightKg = scale.get_units(3);

  lastGoodHx711ReadMs = now;

  // Clear HX711 timeout fault when reading becomes valid again.
  if (faultMessage == "HX711_TIMEOUT") {
    setOtherFault(false, "NONE");
  }

  calculateLevelFromWeight(measuredWeightKg);

  Serial.print("Raw=");
  Serial.print(raw);
  Serial.print(" | Measured=");
  Serial.print(measuredWeightKg, 3);
  Serial.print(" kg | Liquid=");
  Serial.print(liquidWeightKg, 3);
  Serial.print(" kg | Level=");
  Serial.print(levelPercent, 1);
  Serial.print("% | Volume=");
  Serial.print(volumeML, 0);
  Serial.println(" mL");
}

// ---------------- MQTT Publish ----------------

void publishTelemetry() {
  if (!mqtt.connected()) return;

  StaticJsonDocument<512> doc;

  doc["tank_id"] = NODE_ID;
  doc["node"] = NODE_ID;
  doc["level_percent"] = levelPercent;
  doc["volume_ml"] = volumeML;

  doc["measured_weight_kg"] = measuredWeightKg;
  doc["empty_tank_weight_kg"] = EMPTY_TANK_WEIGHT_KG;
  doc["full_tank_weight_kg"] = FULL_TANK_WEIGHT_KG;
  doc["liquid_weight_kg"] = liquidWeightKg;

  doc["valve"] = valveOpen ? "OPEN" : "CLOSED";
  doc["valve_open"] = valveOpen;

  doc["fault"] = tankMissingFault || otherFault;
  doc["fault_message"] = faultMessage;
  doc["missing_tank"] = tankMissingFault;

  doc["rssi"] = WiFi.RSSI();
  doc["ms"] = millis();

  char buffer[512];
  serializeJson(doc, buffer);

  mqtt.publish(topicTelemetry.c_str(), buffer, false);

  Serial.print("Telemetry -> ");
  Serial.println(buffer);
}

void publishRefillRequest() {
  if (!mqtt.connected()) return;
  if (tankMissingFault || otherFault) return;

  StaticJsonDocument<256> doc;

  doc["tank_id"] = NODE_ID;
  doc["node"] = NODE_ID;
  doc["request"] = "REFILL";
  doc["level_percent"] = levelPercent;
  doc["volume_ml"] = volumeML;
  doc["ms"] = millis();

  char buffer[256];
  serializeJson(doc, buffer);

  mqtt.publish(topicRequest.c_str(), buffer, false);

  Serial.print("Refill request -> ");
  Serial.println(buffer);
}

void publishNodeStatus(const String& statusText) {
  if (!mqtt.connected()) return;

  StaticJsonDocument<256> doc;

  doc["tank_id"] = NODE_ID;
  doc["node"] = NODE_ID;
  doc["status"] = statusText;
  doc["fault"] = tankMissingFault || otherFault;
  doc["fault_message"] = faultMessage;
  doc["ms"] = millis();

  char buffer[256];
  serializeJson(doc, buffer);

  mqtt.publish(topicNodeStatus.c_str(), buffer, true);
}

// ---------------- MQTT Receive ----------------

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";

  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.print("Command received [");
  Serial.print(topic);
  Serial.print("] ");
  Serial.println(message);

  message.trim();
  message.toUpperCase();

  if (message == "OPEN") {
    lastCommandMs = millis();

    if (tankMissingFault) {
      setOtherFault(true, "OPEN_REJECTED_TANK_MISSING");
      closeValveFailSafe();
      return;
    }

    if (otherFault) {
      closeValveFailSafe();
      return;
    }

    setValve(true);
  }
  else if (message == "CLOSE") {
    lastCommandMs = millis();
    setValve(false);
  }
  else if (message == "HEARTBEAT") {
    lastCommandMs = millis();
  }
}

// ---------------- WiFi / MQTT ----------------

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("Connecting WiFi");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("WiFi connected. IP=");
  Serial.println(WiFi.localIP());
}

void connectMQTT() {
  if (mqtt.connected()) return;

  unsigned long now = millis();

  if (now - lastReconnectAttemptMs < 3000) return;

  lastReconnectAttemptMs = now;

  String clientId = "edge-node-";
  clientId += NODE_ID;
  clientId += "-";
  clientId += String((uint32_t)ESP.getEfuseMac(), HEX);

  Serial.print("Connecting to MQTTS... ");

  bool connected = mqtt.connect(
    clientId.c_str(),
    topicNodeStatus.c_str(),
    1,
    true,
    "OFFLINE"
  );

  if (connected) {
    Serial.println("connected");

    mqtt.subscribe(topicValveCommand.c_str());
    publishNodeStatus("ONLINE");

    Serial.print("Subscribed: ");
    Serial.println(topicValveCommand);
  } else {
    Serial.print("failed, rc=");
    Serial.println(mqtt.state());
  }
}

// ---------------- Safety ----------------

void updateLocalSafety() {
  unsigned long now = millis();

  if (valveOpen && (now - lastCommandMs > COMMAND_HEARTBEAT_TIMEOUT_MS)) {
    setOtherFault(true, "HEARTBEAT_LOST");
    closeValveFailSafe();
  }

  if (valveOpen && (now - valveOpenedMs > VALVE_MAX_OPEN_TIME_MS)) {
    setOtherFault(true, "VALVE_TIMEOUT");
    closeValveFailSafe();
  }

  if (valveOpen && levelPercent >= TARGET_LEVEL_PERCENT) {
    closeValveFailSafe();
    publishNodeStatus("TARGET_REACHED");
  }

  if (tankMissingFault && valveOpen) {
    closeValveFailSafe();
  }
}

// ---------------- Setup / Loop ----------------

void setup() {
  Serial.begin(115200);

  topicTelemetry = String(PROJECT_PREFIX) + "/telemetry/dispenser/" + NODE_ID;
  topicRequest = String(PROJECT_PREFIX) + "/system/requests";
  topicValveCommand = String(PROJECT_PREFIX) + "/cmd/dispenser/" + NODE_ID + "/valve";
  topicNodeStatus = String(PROJECT_PREFIX) + "/status/dispenser/" + NODE_ID;

  pinMode(VALVE_PIN, OUTPUT);
  pinMode(YELLOW_FAULT_LED, OUTPUT);
  pinMode(RED_FAULT_LED, OUTPUT);

  pinMode(BAR_DATA_PIN, OUTPUT);
  pinMode(BAR_CLOCK_PIN, OUTPUT);
  pinMode(BAR_LATCH_PIN, OUTPUT);

  digitalWrite(VALVE_PIN, LOW);
  digitalWrite(YELLOW_FAULT_LED, LOW);
  digitalWrite(RED_FAULT_LED, LOW);
  writeBarShiftRegisters(0, 0);

  scale.begin(HX711_DT_PIN, HX711_SCK_PIN);

  /*
    Do not use scale.tare().
    We need actual weight:
    1 kg = empty tank
    2 kg = full tank
  */
  scale.set_scale(420.0);

  lastGoodHx711ReadMs = millis();

  secureClient.setInsecure();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(60);
  mqtt.setSocketTimeout(20);
  mqtt.setBufferSize(1024);

  connectWiFi();

  Serial.println();
  Serial.println("Dispensing Tank Edge Node T01 started.");
  Serial.println("Fixed level mapping:");
  Serial.println("1 kg = empty tank = 0%");
  Serial.println("2 kg = full tank = 100%");
  Serial.println();
}

void loop() {
  connectWiFi();
  connectMQTT();
  mqtt.loop();

  readTankLevel();
  updateLocalSafety();

  unsigned long now = millis();

  if (now - lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryMs = now;
    publishTelemetry();
  }

  if (!tankMissingFault && !otherFault && levelPercent < REFILL_THRESHOLD_PERCENT) {
    if (now - lastRequestMs >= REQUEST_INTERVAL_MS) {
      lastRequestMs = now;
      publishRefillRequest();
    }
  }

  delay(10);
}