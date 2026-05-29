/*
  Smart Hospital Automated Chemical Dosing System
  MAIN COMPONENT / CENTRAL HUB - MULTI EDGE NODE VERSION

  Important behavior:
  - One faulty edge node does NOT stop the full system.
  - Faulty node is skipped and blocked.
  - Other healthy nodes continue processing.
  - When faulty node publishes fault:false again, it is automatically recovered.
  - If recovered node is still below refill threshold, it is queued again.

  Supports nodes:
  - T01
  - T02
  - T03

  For extra edge nodes:
  - Copy the edge node Wokwi project.
  - Change NODE_ID in edge node code to T02 or T03.
  - Main component will process it automatically.

  Required libraries.txt:
  PubSubClient
  ArduinoJson
  LiquidCrystal I2C
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------------- WiFi / MQTT ----------------

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

const char* MQTT_HOST = "broker.hivemq.com";
const int MQTT_PORT = 8883;

const char* PROJECT_PREFIX = "shacd/thilokya/demo01";

String topicTelemetryAll;
String topicRequests;
String topicConfigRatio;
String topicMainStatus;

// ---------------- LCD ----------------

LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------------- Pins ----------------

#define WATER_SCALE_PIN      32
#define CHEM_SCALE_PIN       34

#define WATER_VALVE_PIN      14
#define CHEM_PUMP_PIN        19
#define CHEM_VALVE_PIN       13

#define ALARM_LED_PIN        27
#define BUZZER_PIN           4

#define ESTOP_PIN            23

#define LCD_SDA_PIN          21
#define LCD_SCL_PIN          22

#define BAR_DATA_PIN         25
#define BAR_CLOCK_PIN        26
#define BAR_LATCH_PIN        33

// ---------------- System Constants ----------------

const float WATER_TANK_CAPACITY_L = 2000.0;
const float CHEM_TANK_CAPACITY_ML = 10000.0;

const float EDGE_TANK_CAPACITY_ML = 750.0;
const float NORMAL_BATCH_ML = 600.0;
const float EDGE_TARGET_PERCENT = 95.0;
const float REFILL_REQUEST_PERCENT = 20.0;

const float WATER_FLOW_ML_PER_SEC = 90.0;
const float CHEM_FLOW_ML_PER_SEC = 6.0;

int ratioWater = 99;
int ratioChemical = 1;

const int MIN_RATIO_VALUE = 1;
const int MAX_RATIO_VALUE = 999;

const unsigned long EDGE_TELEMETRY_TIMEOUT_MS = 7000;
const unsigned long FILL_CHECK_START_DELAY_MS = 8000;
const unsigned long REFILL_TIMEOUT_MS = 120000;

const unsigned long VERIFY_FILL_TIMEOUT_MS = 6000;
const float MIN_ACCEPTABLE_GAIN_RATIO = 0.70;

const float WARNING_LEVEL_PERCENT = 15.0;
const float CRITICAL_LEVEL_PERCENT = 5.0;
const unsigned long BEEP_DURATION_MS = 180;

const unsigned long HEARTBEAT_INTERVAL_MS = 1500;
const unsigned long LCD_REFRESH_MS = 500;

// ---------------- Node Settings ----------------

const int MAX_NODES = 3;
String NODE_IDS[MAX_NODES] = { "T01", "T02", "T03" };

struct NodeInfo {
  String id;

  float levelPercent;
  float volumeML;
  float startVolumeML;
  float gainML;

  bool telemetryReceived;
  bool valveOpen;

  bool fault;
  bool blocked;
  bool inQueue;

  String faultMessage;

  unsigned long lastTelemetryMs;
};

NodeInfo nodes[MAX_NODES];

int activeNodeIndex = -1;

int refillQueue[MAX_NODES];
int queueCount = 0;

// ---------------- MQTT Objects ----------------

WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);

// ---------------- Main State ----------------

enum MainState {
  IDLE,
  QUEUED,
  PREFLIGHT,
  DOSING,
  VERIFY_FILL,
  COMPLETE,
  FAULT
};

MainState state = IDLE;

// ---------------- Runtime Variables ----------------

float waterPercent = 0.0;
float chemPercent = 0.0;
float waterAvailableL = 0.0;
float chemAvailableML = 0.0;

float requiredWaterML = 0.0;
float requiredChemicalML = 0.0;
float deliveredWaterML = 0.0;
float deliveredChemicalML = 0.0;
float deliveredTotalML = 0.0;

String activeNodeId = "NONE";
String faultMessage = "";

unsigned long stateStartMs = 0;
unsigned long lastDosingUpdateMs = 0;
unsigned long lastSerialMs = 0;
unsigned long lastHeartbeatMs = 0;
unsigned long lastReconnectAttemptMs = 0;
unsigned long lastLcdMs = 0;

bool buzzerBeepActive = false;
unsigned long buzzerBeepStartMs = 0;
int lastWarningBeepStep = 15;

// ---------------- Utility ----------------

float mapFloat(float x, float inMin, float inMax, float outMin, float outMax) {
  return (x - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

String stateName(MainState s) {
  switch (s) {
    case IDLE: return "IDLE";
    case QUEUED: return "QUEUED";
    case PREFLIGHT: return "PREFLIGHT";
    case DOSING: return "DOSING";
    case VERIFY_FILL: return "VERIFY_FILL";
    case COMPLETE: return "COMPLETE";
    case FAULT: return "FAULT";
    default: return "UNKNOWN";
  }
}

void setState(MainState newState) {
  state = newState;
  stateStartMs = millis();
}

void lcdLine(byte row, String text) {
  if (text.length() > 16) {
    text = text.substring(0, 16);
  }

  while (text.length() < 16) {
    text += " ";
  }

  lcd.setCursor(0, row);
  lcd.print(text);
}

void allActuatorsOff() {
  digitalWrite(WATER_VALVE_PIN, LOW);
  digitalWrite(CHEM_PUMP_PIN, LOW);
  digitalWrite(CHEM_VALVE_PIN, LOW);
}

String commandTopicForNode(String nodeId) {
  return String(PROJECT_PREFIX) + "/cmd/dispenser/" + nodeId + "/valve";
}

int findNodeIndex(String nodeId) {
  for (int i = 0; i < MAX_NODES; i++) {
    if (nodes[i].id == nodeId) {
      return i;
    }
  }

  return -1;
}

bool isNodeInQueue(int nodeIndex) {
  for (int i = 0; i < queueCount; i++) {
    if (refillQueue[i] == nodeIndex) {
      return true;
    }
  }

  return false;
}

void enqueueNode(int nodeIndex) {
  if (nodeIndex < 0 || nodeIndex >= MAX_NODES) return;
  if (queueCount >= MAX_NODES) return;
  if (nodes[nodeIndex].fault) return;
  if (nodes[nodeIndex].blocked) return;
  if (!nodes[nodeIndex].telemetryReceived) return;
  if (nodes[nodeIndex].levelPercent >= REFILL_REQUEST_PERCENT) return;
  if (isNodeInQueue(nodeIndex)) return;

  refillQueue[queueCount] = nodeIndex;
  queueCount++;
  nodes[nodeIndex].inQueue = true;

  Serial.print("Queued node ");
  Serial.println(nodes[nodeIndex].id);
}

int getNextHealthyQueuedNode() {
  while (queueCount > 0) {
    int nodeIndex = refillQueue[0];

    for (int i = 0; i < queueCount - 1; i++) {
      refillQueue[i] = refillQueue[i + 1];
    }

    queueCount--;
    nodes[nodeIndex].inQueue = false;

    if (!nodes[nodeIndex].fault &&
        !nodes[nodeIndex].blocked &&
        nodes[nodeIndex].telemetryReceived &&
        nodes[nodeIndex].levelPercent < REFILL_REQUEST_PERCENT) {
      return nodeIndex;
    }

    Serial.print("Skipped node ");
    Serial.print(nodes[nodeIndex].id);
    Serial.println(" because it is faulty/full/offline.");
  }

  return -1;
}

void publishValveCommand(String nodeId, String command) {
  if (!mqtt.connected()) return;

  String topic = commandTopicForNode(nodeId);
  mqtt.publish(topic.c_str(), command.c_str());

  Serial.print("MQTT publish -> ");
  Serial.print(topic);
  Serial.print(" : ");
  Serial.println(command);
}

void closeActiveNodeValve() {
  if (activeNodeIndex >= 0) {
    publishValveCommand(nodes[activeNodeIndex].id, "CLOSE");
  }
}

void publishMainStatus(String statusText) {
  if (!mqtt.connected()) return;

  StaticJsonDocument<768> doc;

  doc["state"] = stateName(state);
  doc["status"] = statusText;
  doc["active_node"] = activeNodeId;
  doc["water_percent"] = waterPercent;
  doc["chemical_percent"] = chemPercent;
  doc["fault"] = faultMessage;
  doc["queue_count"] = queueCount;
  doc["ms"] = millis();

  JsonArray nodeArray = doc.createNestedArray("nodes");

  for (int i = 0; i < MAX_NODES; i++) {
    JsonObject n = nodeArray.createNestedObject();
    n["id"] = nodes[i].id;
    n["level"] = nodes[i].levelPercent;
    n["volume_ml"] = nodes[i].volumeML;
    n["fault"] = nodes[i].fault;
    n["blocked"] = nodes[i].blocked;
    n["fault_message"] = nodes[i].faultMessage;
    n["telemetry"] = nodes[i].telemetryReceived;
  }

  char buffer[768];
  serializeJson(doc, buffer);

  mqtt.publish(topicMainStatus.c_str(), buffer, true);
}

// ---------------- LED Bar Graph ----------------

void writeShiftRegisters(byte sr1, byte sr2, byte sr3) {
  digitalWrite(BAR_LATCH_PIN, LOW);

  shiftOut(BAR_DATA_PIN, BAR_CLOCK_PIN, MSBFIRST, sr3);
  shiftOut(BAR_DATA_PIN, BAR_CLOCK_PIN, MSBFIRST, sr2);
  shiftOut(BAR_DATA_PIN, BAR_CLOCK_PIN, MSBFIRST, sr1);

  digitalWrite(BAR_LATCH_PIN, HIGH);
}

int segmentsFromPercent(float percent) {
  percent = constrain(percent, 0.0, 100.0);

  if (percent <= 0.5) return 0;

  int segments = ceil(percent / 10.0);
  return constrain(segments, 0, 10);
}

void updateLevelBarGraphs() {
  int waterSegments = segmentsFromPercent(waterPercent);
  int chemSegments = segmentsFromPercent(chemPercent);

  byte srWaterLow = 0;
  byte srMixed = 0;
  byte srChemHigh = 0;

  for (int i = 0; i < waterSegments; i++) {
    if (i < 8) {
      bitSet(srWaterLow, i);
    } else {
      bitSet(srMixed, i - 8);
    }
  }

  for (int i = 0; i < chemSegments; i++) {
    if (i < 6) {
      bitSet(srMixed, i + 2);
    } else {
      bitSet(srChemHigh, i - 6);
    }
  }

  writeShiftRegisters(srWaterLow, srMixed, srChemHigh);
}

// ---------------- Supply Monitoring ----------------

void updateSupplyLevels() {
  int rawWater = analogRead(WATER_SCALE_PIN);
  int rawChem = analogRead(CHEM_SCALE_PIN);

  waterPercent = mapFloat(rawWater, 0, 4095, 0.0, 100.0);
  chemPercent = mapFloat(rawChem, 0, 4095, 0.0, 100.0);

  waterPercent = constrain(waterPercent, 0.0, 100.0);
  chemPercent = constrain(chemPercent, 0.0, 100.0);

  waterAvailableL = (waterPercent / 100.0) * WATER_TANK_CAPACITY_L;
  chemAvailableML = (chemPercent / 100.0) * CHEM_TANK_CAPACITY_ML;
}

// ---------------- Alarm Logic ----------------

void startShortBeep() {
  buzzerBeepActive = true;
  buzzerBeepStartMs = millis();
  digitalWrite(BUZZER_PIN, HIGH);
}

void updateWarningAlarm() {
  if (state == FAULT) {
    digitalWrite(ALARM_LED_PIN, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
    return;
  }

  float lowestSupplyPercent = min(waterPercent, chemPercent);

  if (lowestSupplyPercent < WARNING_LEVEL_PERCENT) {
    digitalWrite(ALARM_LED_PIN, HIGH);
  } else {
    digitalWrite(ALARM_LED_PIN, LOW);
  }

  if (lowestSupplyPercent < CRITICAL_LEVEL_PERCENT) {
    digitalWrite(BUZZER_PIN, HIGH);
    buzzerBeepActive = false;
    return;
  }

  if (lowestSupplyPercent >= WARNING_LEVEL_PERCENT) {
    lastWarningBeepStep = 15;
    buzzerBeepActive = false;
    digitalWrite(BUZZER_PIN, LOW);
    return;
  }

  int currentStep = floor(lowestSupplyPercent);

  if (currentStep < lastWarningBeepStep) {
    lastWarningBeepStep = currentStep;
    startShortBeep();
  }

  if (buzzerBeepActive) {
    if (millis() - buzzerBeepStartMs >= BEEP_DURATION_MS) {
      buzzerBeepActive = false;
      digitalWrite(BUZZER_PIN, LOW);
    }
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }
}

// ---------------- Fault Handling ----------------

void enterSystemFault(String message) {
  faultMessage = message;

  allActuatorsOff();
  closeActiveNodeValve();

  digitalWrite(ALARM_LED_PIN, HIGH);
  digitalWrite(BUZZER_PIN, HIGH);

  setState(FAULT);

  Serial.println();
  Serial.println("========== SYSTEM FAULT ==========");
  Serial.println(faultMessage);
  Serial.println("Full system stopped because this fault affects all nodes.");
  Serial.println("==================================");
  Serial.println();

  publishMainStatus("SYSTEM_FAULT");
}

void abortActiveNode(String message) {
  if (activeNodeIndex < 0) return;

  Serial.println();
  Serial.println("========== NODE FAULT ==========");
  Serial.print("Node: ");
  Serial.println(nodes[activeNodeIndex].id);
  Serial.print("Reason: ");
  Serial.println(message);
  Serial.println("Only this node is blocked. System continues with others.");
  Serial.println("================================");
  Serial.println();

  allActuatorsOff();
  publishValveCommand(nodes[activeNodeIndex].id, "CLOSE");

  nodes[activeNodeIndex].fault = true;
  nodes[activeNodeIndex].blocked = true;
  nodes[activeNodeIndex].faultMessage = message;
  nodes[activeNodeIndex].inQueue = false;

  publishMainStatus("NODE_FAULT_" + nodes[activeNodeIndex].id);

  activeNodeIndex = -1;
  activeNodeId = "NONE";

  setState(IDLE);
}

// ---------------- Ratio / Dosing ----------------

bool validateRatio() {
  if (ratioWater < MIN_RATIO_VALUE || ratioChemical < MIN_RATIO_VALUE) return false;
  if (ratioWater > MAX_RATIO_VALUE || ratioChemical > MAX_RATIO_VALUE) return false;
  return true;
}

void calculateDose() {
  float totalRatio = ratioWater + ratioChemical;

  requiredWaterML = NORMAL_BATCH_ML * ratioWater / totalRatio;
  requiredChemicalML = NORMAL_BATCH_ML * ratioChemical / totalRatio;

  Serial.println();
  Serial.println("========== DOSING CALCULATION ==========");
  Serial.print("Node: ");
  Serial.println(activeNodeId);

  Serial.print("Backend ratio water:chemical = ");
  Serial.print(ratioWater);
  Serial.print(":");
  Serial.println(ratioChemical);

  Serial.print("Batch volume = ");
  Serial.print(NORMAL_BATCH_ML);
  Serial.println(" mL");

  Serial.print("Water required = ");
  Serial.print(requiredWaterML);
  Serial.println(" mL");

  Serial.print("Chemical required = ");
  Serial.print(requiredChemicalML);
  Serial.println(" mL");
  Serial.println("========================================");
  Serial.println();
}

void startDosing() {
  if (activeNodeIndex < 0) return;

  deliveredWaterML = 0.0;
  deliveredChemicalML = 0.0;
  deliveredTotalML = 0.0;

  nodes[activeNodeIndex].startVolumeML = nodes[activeNodeIndex].volumeML;
  nodes[activeNodeIndex].gainML = 0.0;

  lastDosingUpdateMs = millis();
  lastHeartbeatMs = 0;

  digitalWrite(WATER_VALVE_PIN, HIGH);
  digitalWrite(CHEM_VALVE_PIN, HIGH);
  digitalWrite(CHEM_PUMP_PIN, HIGH);

  publishValveCommand(activeNodeId, "OPEN");

  Serial.println("Dosing started.");
  Serial.print("Active node: ");
  Serial.println(activeNodeId);
  Serial.print("Edge starting volume = ");
  Serial.print(nodes[activeNodeIndex].startVolumeML);
  Serial.println(" mL");

  publishMainStatus("DOSING_STARTED");

  setState(DOSING);
}

void startVerifyFill() {
  allActuatorsOff();

  if (activeNodeIndex >= 0) {
    publishValveCommand(activeNodeId, "HEARTBEAT");
  }

  Serial.println("Delivery finished. Waiting for edge telemetry verification...");
  publishMainStatus("VERIFY_FILL");

  setState(VERIFY_FILL);
}

void updateVerifyFill() {
  if (activeNodeIndex < 0) {
    setState(IDLE);
    return;
  }

  unsigned long now = millis();
  NodeInfo &node = nodes[activeNodeIndex];

  if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = now;
    publishValveCommand(activeNodeId, "HEARTBEAT");
  }

  if (now - node.lastTelemetryMs > EDGE_TELEMETRY_TIMEOUT_MS) {
    abortActiveNode("No telemetry during verify");
    return;
  }

  if (node.fault) {
    abortActiveNode("Edge fault during verify: " + node.faultMessage);
    return;
  }

  node.gainML = node.volumeML - node.startVolumeML;
  if (node.gainML < 0) node.gainML = 0;

  float requiredGainML = deliveredTotalML * MIN_ACCEPTABLE_GAIN_RATIO;

  if (node.levelPercent >= EDGE_TARGET_PERCENT) {
    publishValveCommand(activeNodeId, "CLOSE");
    publishMainStatus("COMPLETE_TARGET_VERIFIED");

    Serial.println("Dosing complete: target level verified by edge telemetry.");

    setState(COMPLETE);
    return;
  }

  if (node.gainML >= requiredGainML) {
    publishValveCommand(activeNodeId, "CLOSE");
    publishMainStatus("COMPLETE_GAIN_VERIFIED");

    Serial.println("Dosing complete: edge gain verified.");

    setState(COMPLETE);
    return;
  }

  if (now - stateStartMs > VERIFY_FILL_TIMEOUT_MS) {
    String msg = "Gain low D:";
    msg += String(deliveredTotalML, 0);
    msg += " G:";
    msg += String(node.gainML, 0);

    abortActiveNode(msg);
    return;
  }
}

void updateDosing() {
  if (activeNodeIndex < 0) {
    setState(IDLE);
    return;
  }

  unsigned long now = millis();
  NodeInfo &node = nodes[activeNodeIndex];

  if (now - lastDosingUpdateMs >= 100) {
    float dt = (now - lastDosingUpdateMs) / 1000.0;
    lastDosingUpdateMs = now;

    if (deliveredWaterML < requiredWaterML) {
      digitalWrite(WATER_VALVE_PIN, HIGH);
      deliveredWaterML += WATER_FLOW_ML_PER_SEC * dt;

      if (deliveredWaterML > requiredWaterML) {
        deliveredWaterML = requiredWaterML;
      }
    } else {
      digitalWrite(WATER_VALVE_PIN, LOW);
    }

    if (deliveredChemicalML < requiredChemicalML) {
      digitalWrite(CHEM_VALVE_PIN, HIGH);
      digitalWrite(CHEM_PUMP_PIN, HIGH);

      deliveredChemicalML += CHEM_FLOW_ML_PER_SEC * dt;

      if (deliveredChemicalML > requiredChemicalML) {
        deliveredChemicalML = requiredChemicalML;
      }
    } else {
      digitalWrite(CHEM_VALVE_PIN, LOW);
      digitalWrite(CHEM_PUMP_PIN, LOW);
    }

    deliveredTotalML = deliveredWaterML + deliveredChemicalML;
  }

  if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = now;
    publishValveCommand(activeNodeId, "HEARTBEAT");
  }

  if (now - node.lastTelemetryMs > EDGE_TELEMETRY_TIMEOUT_MS) {
    abortActiveNode("Telemetry lost");
    return;
  }

  if (node.fault) {
    abortActiveNode("Edge fault: " + node.faultMessage);
    return;
  }

  node.gainML = node.volumeML - node.startVolumeML;
  if (node.gainML < 0) node.gainML = 0;

  if (now - stateStartMs > FILL_CHECK_START_DELAY_MS) {
    if (deliveredTotalML > 80.0 && node.gainML < 20.0) {
      abortActiveNode("Node not filling");
      return;
    }
  }

  if (node.levelPercent >= EDGE_TARGET_PERCENT) {
    allActuatorsOff();
    publishValveCommand(activeNodeId, "CLOSE");
    publishMainStatus("COMPLETE_TARGET");

    Serial.println("Dosing complete: edge tank reached target.");

    setState(COMPLETE);
    return;
  }

  if (deliveredWaterML >= requiredWaterML && deliveredChemicalML >= requiredChemicalML) {
    startVerifyFill();
    return;
  }

  if (now - stateStartMs > REFILL_TIMEOUT_MS) {
    abortActiveNode("Refill timeout");
    return;
  }
}

// ---------------- MQTT Receive ----------------

void handleTelemetryMessage(String topicStr, byte* payload, unsigned int length) {
  StaticJsonDocument<768> doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error) {
    Serial.print("Invalid telemetry JSON: ");
    Serial.println(error.c_str());
    return;
  }

  String receivedNode = "";

  if (doc.containsKey("node")) {
    receivedNode = doc["node"].as<String>();
  } else if (doc.containsKey("tank_id")) {
    receivedNode = doc["tank_id"].as<String>();
  } else {
    int lastSlash = topicStr.lastIndexOf('/');
    if (lastSlash >= 0) {
      receivedNode = topicStr.substring(lastSlash + 1);
    }
  }

  int nodeIndex = findNodeIndex(receivedNode);

  if (nodeIndex < 0) {
    Serial.print("Ignored unknown node telemetry: ");
    Serial.println(receivedNode);
    return;
  }

  NodeInfo &node = nodes[nodeIndex];

  node.telemetryReceived = true;
  node.lastTelemetryMs = millis();

  if (doc.containsKey("level_percent")) {
    node.levelPercent = doc["level_percent"].as<float>();
  }

  if (doc.containsKey("volume_ml")) {
    node.volumeML = doc["volume_ml"].as<float>();
  } else {
    node.volumeML = (node.levelPercent / 100.0) * EDGE_TANK_CAPACITY_ML;
  }

  if (doc.containsKey("valve_open")) {
    node.valveOpen = doc["valve_open"].as<bool>();
  } else if (doc.containsKey("valve")) {
    String valveText = doc["valve"].as<String>();
    valveText.toUpperCase();
    node.valveOpen = valveText == "OPEN";
  }

  bool incomingFault = false;
  if (doc.containsKey("fault")) {
    incomingFault = doc["fault"].as<bool>();
  }

  String incomingFaultMessage = "NONE";
  if (doc.containsKey("fault_message")) {
    incomingFaultMessage = doc["fault_message"].as<String>();
  }

  // Recovery detection:
  // If a node was previously faulty/blocked and now publishes fault:false,
  // automatically unblock it.
  if (!incomingFault && (node.fault || node.blocked)) {
    Serial.println();
    Serial.print("NODE RECOVERED: ");
    Serial.println(node.id);
    Serial.println("Node fault cleared. It can be processed again.");
    Serial.println();

    node.fault = false;
    node.blocked = false;
    node.faultMessage = "NONE";

    publishMainStatus("NODE_RECOVERED_" + node.id);
  }

  if (incomingFault) {
    node.fault = true;
    node.blocked = true;
    node.faultMessage = incomingFaultMessage;

    // If this is not the active node, just mark it faulty.
    // Do NOT stop full system.
    if (nodeIndex != activeNodeIndex) {
      Serial.print("Inactive node fault recorded: ");
      Serial.print(node.id);
      Serial.print(" - ");
      Serial.println(node.faultMessage);
    }
  }

  // Auto queue healthy low nodes.
  if (!node.fault &&
      !node.blocked &&
      node.telemetryReceived &&
      node.levelPercent < REFILL_REQUEST_PERCENT) {
    enqueueNode(nodeIndex);
  }

  Serial.println();
  Serial.println("EDGE TELEMETRY UPDATED");
  Serial.print("Node   : ");
  Serial.println(node.id);
  Serial.print("Level  : ");
  Serial.print(node.levelPercent);
  Serial.println("%");
  Serial.print("Volume : ");
  Serial.print(node.volumeML);
  Serial.println(" mL");
  Serial.print("Fault  : ");
  Serial.println(node.fault ? "YES" : "NO");
  Serial.println();
}

void handleRequestMessage(byte* payload, unsigned int length) {
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  String nodeId = "";

  if (!error) {
    if (doc.containsKey("node")) {
      nodeId = doc["node"].as<String>();
    } else if (doc.containsKey("tank_id")) {
      nodeId = doc["tank_id"].as<String>();
    }
  }

  if (nodeId == "") {
    String msg = "";
    for (unsigned int i = 0; i < length; i++) {
      msg += (char)payload[i];
    }

    for (int i = 0; i < MAX_NODES; i++) {
      if (msg.indexOf(nodes[i].id) >= 0) {
        nodeId = nodes[i].id;
        break;
      }
    }
  }

  int nodeIndex = findNodeIndex(nodeId);

  if (nodeIndex >= 0) {
    Serial.print("Refill request received from ");
    Serial.println(nodeId);
    enqueueNode(nodeIndex);
  }
}

void handleRatioConfig(byte* payload, unsigned int length) {
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error) {
    Serial.println("Invalid ratio config JSON");
    return;
  }

  if (doc.containsKey("water")) {
    ratioWater = doc["water"].as<int>();
  }

  if (doc.containsKey("chemical")) {
    ratioChemical = doc["chemical"].as<int>();
  }

  if (!validateRatio()) {
    Serial.println("Rejected invalid backend ratio. Reverting to 99:1.");
    ratioWater = 99;
    ratioChemical = 1;
    return;
  }

  Serial.print("Backend ratio updated: ");
  Serial.print(ratioWater);
  Serial.print(":");
  Serial.println(ratioChemical);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);

  Serial.println();
  Serial.print("MQTT RECEIVED TOPIC: ");
  Serial.println(topicStr);

  if (topicStr.indexOf("/telemetry/dispenser/") >= 0) {
    handleTelemetryMessage(topicStr, payload, length);
    return;
  }

  if (topicStr == topicRequests || topicStr.endsWith("/system/requests")) {
    handleRequestMessage(payload, length);
    return;
  }

  if (topicStr == topicConfigRatio || topicStr.endsWith("/config/dosing_ratio")) {
    handleRatioConfig(payload, length);
    return;
  }
}

// ---------------- WiFi / MQTT ----------------

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("Connecting to WiFi");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("WiFi connected. IP: ");
  Serial.println(WiFi.localIP());
}

void connectMQTT() {
  if (mqtt.connected()) return;

  unsigned long now = millis();
  if (now - lastReconnectAttemptMs < 3000) return;

  lastReconnectAttemptMs = now;

  String clientId = "main-component-";
  clientId += String((uint32_t)ESP.getEfuseMac(), HEX);
  clientId += "-";
  clientId += String(random(0xffff), HEX);

  Serial.print("Connecting to MQTTS as ");
  Serial.println(clientId);

  if (mqtt.connect(clientId.c_str())) {
    Serial.println("MQTTS connected");

    mqtt.subscribe(topicTelemetryAll.c_str());
    mqtt.subscribe(topicRequests.c_str());
    mqtt.subscribe(topicConfigRatio.c_str());

    publishMainStatus("ONLINE");

    Serial.println("Subscribed topics:");
    Serial.println(topicTelemetryAll);
    Serial.println(topicRequests);
    Serial.println(topicConfigRatio);
  } else {
    Serial.print("MQTTS failed, rc=");
    Serial.println(mqtt.state());
  }
}

// ---------------- LCD / Serial ----------------

int countFaultyNodes() {
  int count = 0;

  for (int i = 0; i < MAX_NODES; i++) {
    if (nodes[i].fault || nodes[i].blocked) {
      count++;
    }
  }

  return count;
}

void updateLCD() {
  if (millis() - lastLcdMs < LCD_REFRESH_MS) return;
  lastLcdMs = millis();

  if (state == FAULT) {
    lcdLine(0, "SYSTEM FAULT");
    lcdLine(1, faultMessage);
    return;
  }

  if (state == DOSING) {
    lcdLine(0, "Dosing:" + activeNodeId);
    if (activeNodeIndex >= 0) {
      lcdLine(1, "Gain:" + String(nodes[activeNodeIndex].gainML, 0) + "ml");
    }
    return;
  }

  if (state == VERIFY_FILL) {
    lcdLine(0, "Verify:" + activeNodeId);
    if (activeNodeIndex >= 0) {
      lcdLine(1, "Gain:" + String(nodes[activeNodeIndex].gainML, 0) + "ml");
    }
    return;
  }

  int faulty = countFaultyNodes();

  if (faulty > 0) {
    lcdLine(0, "Node Faults:" + String(faulty));
    lcdLine(1, "Q:" + String(queueCount) + " Sys Running");
    return;
  }

  lcdLine(0, "W:" + String(waterPercent, 0) + "% C:" + String(chemPercent, 0) + "%");
  lcdLine(1, "Queue:" + String(queueCount) + " IDLE");
}

void printStatus() {
  if (millis() - lastSerialMs < 1000) return;
  lastSerialMs = millis();

  Serial.print("State=");
  Serial.print(stateName(state));
  Serial.print(" | W=");
  Serial.print(waterPercent, 1);
  Serial.print("% | C=");
  Serial.print(chemPercent, 1);
  Serial.print("% | Active=");
  Serial.print(activeNodeId);
  Serial.print(" | Queue=");
  Serial.println(queueCount);

  for (int i = 0; i < MAX_NODES; i++) {
    Serial.print("  ");
    Serial.print(nodes[i].id);
    Serial.print(" Level=");
    Serial.print(nodes[i].levelPercent, 1);
    Serial.print("% Vol=");
    Serial.print(nodes[i].volumeML, 0);
    Serial.print(" Fault=");
    Serial.print(nodes[i].fault ? "YES" : "NO");
    Serial.print(" Blocked=");
    Serial.print(nodes[i].blocked ? "YES" : "NO");
    Serial.print(" Msg=");
    Serial.println(nodes[i].faultMessage);
  }
}

// ---------------- Setup / Loop ----------------

void setup() {
  Serial.begin(115200);

  topicTelemetryAll = String(PROJECT_PREFIX) + "/telemetry/dispenser/+";
  topicRequests = String(PROJECT_PREFIX) + "/system/requests";
  topicConfigRatio = String(PROJECT_PREFIX) + "/config/dosing_ratio";
  topicMainStatus = String(PROJECT_PREFIX) + "/status/main";

  for (int i = 0; i < MAX_NODES; i++) {
    nodes[i].id = NODE_IDS[i];
    nodes[i].levelPercent = -1;
    nodes[i].volumeML = -1;
    nodes[i].startVolumeML = -1;
    nodes[i].gainML = 0;
    nodes[i].telemetryReceived = false;
    nodes[i].valveOpen = false;
    nodes[i].fault = false;
    nodes[i].blocked = false;
    nodes[i].inQueue = false;
    nodes[i].faultMessage = "NONE";
    nodes[i].lastTelemetryMs = 0;
  }

  pinMode(WATER_VALVE_PIN, OUTPUT);
  pinMode(CHEM_PUMP_PIN, OUTPUT);
  pinMode(CHEM_VALVE_PIN, OUTPUT);

  pinMode(ALARM_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  pinMode(ESTOP_PIN, INPUT_PULLUP);

  pinMode(BAR_DATA_PIN, OUTPUT);
  pinMode(BAR_CLOCK_PIN, OUTPUT);
  pinMode(BAR_LATCH_PIN, OUTPUT);

  allActuatorsOff();
  digitalWrite(ALARM_LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  writeShiftRegisters(0, 0, 0);

  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  lcd.init();
  lcd.backlight();

  lcdLine(0, "Smart Dosing");
  lcdLine(1, "Main Starting");

  updateSupplyLevels();
  updateLevelBarGraphs();

  secureClient.setInsecure();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(60);
  mqtt.setSocketTimeout(20);
  mqtt.setBufferSize(1024);

  connectWiFi();

  Serial.println();
  Serial.println("Smart Hospital Chemical Dosing MAIN COMPONENT started.");
  Serial.println("Multi-node mode enabled.");
  Serial.println("One edge fault will not stop the whole system.");
  Serial.println();
}

void loop() {
  connectWiFi();
  connectMQTT();
  mqtt.loop();

  updateSupplyLevels();
  updateLevelBarGraphs();
  updateWarningAlarm();
  updateLCD();

  if (digitalRead(ESTOP_PIN) == LOW && state != FAULT) {
    enterSystemFault("Emergency stop");
  }

  switch (state) {
    case IDLE:
      allActuatorsOff();

      if (queueCount > 0) {
        int nextNode = getNextHealthyQueuedNode();

        if (nextNode >= 0) {
          activeNodeIndex = nextNode;
          activeNodeId = nodes[nextNode].id;
          setState(QUEUED);
        }
      }
      break;

    case QUEUED:
      Serial.println("Processing queued node: " + activeNodeId);
      setState(PREFLIGHT);
      break;

    case PREFLIGHT:
      if (activeNodeIndex < 0) {
        setState(IDLE);
        break;
      }

      if (!validateRatio()) {
        enterSystemFault("Invalid ratio");
        break;
      }

      if (millis() - nodes[activeNodeIndex].lastTelemetryMs > EDGE_TELEMETRY_TIMEOUT_MS) {
        abortActiveNode("No fresh telemetry");
        break;
      }

      if (nodes[activeNodeIndex].fault || nodes[activeNodeIndex].blocked) {
        abortActiveNode("Node already faulty");
        break;
      }

      calculateDose();

      if (waterAvailableL < requiredWaterML / 1000.0) {
        enterSystemFault("Low water");
        break;
      }

      if (chemAvailableML < requiredChemicalML) {
        enterSystemFault("Low chemical");
        break;
      }

      if (nodes[activeNodeIndex].levelPercent >= EDGE_TARGET_PERCENT) {
        Serial.println("Node already full. Skipping.");
        activeNodeIndex = -1;
        activeNodeId = "NONE";
        setState(IDLE);
        break;
      }

      startDosing();
      break;

    case DOSING:
      updateDosing();
      break;

    case VERIFY_FILL:
      updateVerifyFill();
      break;

    case COMPLETE:
      allActuatorsOff();

      if (activeNodeIndex >= 0) {
        publishValveCommand(activeNodeId, "CLOSE");
      }

      if (millis() - stateStartMs > 3000) {
        Serial.println("Node complete. Returning to IDLE for next queue item.");

        activeNodeIndex = -1;
        activeNodeId = "NONE";

        setState(IDLE);
      }
      break;

    case FAULT:
      allActuatorsOff();
      closeActiveNodeValve();
      digitalWrite(ALARM_LED_PIN, HIGH);
      digitalWrite(BUZZER_PIN, HIGH);
      break;
  }

  printStatus();
}