#include "PrinterClient.h"

#include <esp_system.h>
#include <string.h>
#include <stdlib.h>

namespace {
PrinterClient* g_instance = nullptr;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 5000;
constexpr uint32_t MQTT_RETRY_INTERVAL_MS = 5000;
}

PrinterClient::PrinterClient()
    : mqttClient_(wifiClient_),
      status_(nullptr),
      config_(nullptr),
      lastWifiAttemptMs_(0),
      lastMqttAttemptMs_(0),
      wifiReportedConnected_(false),
      mqttReportedConnected_(false) {}

void PrinterClient::begin(PrinterStatus* sharedStatus, const DeviceConfig* runtimeConfig) {
  status_ = sharedStatus;
  config_ = runtimeConfig;
  g_instance = this;

  if (!config_) {
    Serial.println("PrinterClient begin called without runtime config");
    return;
  }

  subscribeTopic_ = String(BAMBU_REPORT_TOPIC_PREFIX) + config_->printerSerial + BAMBU_REPORT_TOPIC_SUFFIX;

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname("bambu-status");

  // Configure TLS from provisioned config.
  if (config_->tlsInsecure) {
    wifiClient_.setInsecure();  // Skip verification to mirror mosquitto --insecure (host uses IP)
    Serial.println("TLS set to insecure mode (no certificate validation)");
  } else if (strlen(BAMBU_PRINTER_ROOT_CA) > 10) {
    wifiClient_.setCACert(BAMBU_PRINTER_ROOT_CA);
    Serial.println("Using provided printer root CA for TLS validation");
  } else {
    wifiClient_.setInsecure();
    Serial.println("No printer root CA found; falling back to insecure TLS");
  }
  wifiClient_.setHandshakeTimeout(15);
  wifiClient_.setNoDelay(true);

  mqttClient_.setServer(config_->printerHost, config_->printerPort);
  mqttClient_.setCallback(PrinterClient::handleMqttWrapper);
  mqttClient_.setKeepAlive(30);
  mqttClient_.setBufferSize(4096);
}

void PrinterClient::loop() {
  if (!config_) {
    return;
  }
  ensureWifi();
  ensureMqtt();

  if (mqttClient_.connected()) {
    mqttClient_.loop();
  }
}

void PrinterClient::stop() {
  if (mqttClient_.connected()) {
    mqttClient_.disconnect();
  }
  WiFi.disconnect(true, true);
  mqttReportedConnected_ = false;
  wifiReportedConnected_ = false;
}

bool PrinterClient::isWifiConnected() const { return WiFi.status() == WL_CONNECTED; }

bool PrinterClient::isMqttConnected() { return mqttClient_.connected(); }

void PrinterClient::ensureWifi() {
  unsigned long now = millis();
  wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    if (!wifiReportedConnected_) {
      Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
      wifiReportedConnected_ = true;
    }
    return;
  }

  if (wifiReportedConnected_) {
    Serial.println("WiFi disconnected, attempting reconnect...");
    wifiReportedConnected_ = false;
  }

  if (now - lastWifiAttemptMs_ < WIFI_RETRY_INTERVAL_MS) {
    return;
  }

  lastWifiAttemptMs_ = now;
  Serial.println("Connecting to provisioned WiFi network...");
  WiFi.begin(config_->wifiSsid, config_->wifiPassword);
}

void PrinterClient::ensureMqtt() {
  unsigned long now = millis();

  if (!isWifiConnected()) {
    if (mqttClient_.connected()) {
      mqttClient_.disconnect();
      Serial.println("MQTT disconnected because WiFi is down");
    }
    mqttReportedConnected_ = false;
    return;
  }

  if (mqttClient_.connected()) {
    if (!mqttReportedConnected_) {
      Serial.println("MQTT connected to printer broker");
      if (mqttClient_.subscribe(subscribeTopic_.c_str())) {
        Serial.printf("Subscribed to %s\n", subscribeTopic_.c_str());
      } else {
        Serial.println("Failed to subscribe to printer topic");
      }
      mqttReportedConnected_ = true;
    }
    return;
  }

  if (mqttReportedConnected_) {
    Serial.println("MQTT connection lost, retrying...");
    mqttReportedConnected_ = false;
  }

  if (now - lastMqttAttemptMs_ < MQTT_RETRY_INTERVAL_MS) {
    return;
  }

  lastMqttAttemptMs_ = now;
  String clientId = String("bambu-status-") + String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFF), HEX);
  if (!mqttClient_.connect(clientId.c_str(), config_->mqttUsername, config_->accessCode)) {
    Serial.printf("MQTT connect failed, state=%d\n", mqttClient_.state());
    return;
  }

  Serial.println("MQTT handshake complete");
  mqttReportedConnected_ = false;  // Force subscription in connected block
}

void PrinterClient::handleMqttWrapper(char* topic, uint8_t* payload, unsigned int length) {
  if (g_instance) {
    g_instance->handleMessage(topic, payload, length);
  }
}

void PrinterClient::handleMessage(char* topic, uint8_t* payload, unsigned int length) {
  (void)topic;
  if (!status_) {
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print("Failed to parse MQTT payload: ");
    Serial.println(err.c_str());
    return;
  }

  JsonVariantConst printNode = doc["print"];
  if (printNode.isNull()) {
    Serial.println("MQTT payload missing 'print' node");
    return;
  }

  applyStatusFromJson(printNode);
}

void PrinterClient::applyStatusFromJson(JsonVariantConst printNode) {
  if (!status_) {
    return;
  }

  PrinterStatus& s = *status_;
  s.lastUpdateMs = millis();
  uint32_t nowMs = s.lastUpdateMs;

  bool hasState = printNode["gcode_state"].is<const char*>();
  String rawState = hasState ? String(printNode["gcode_state"].as<const char*>()) : s.gcodeState;
  rawState.toUpperCase();
  if (hasState) {
    s.gcodeState = rawState;
    if (rawState == "PREPARE") {
      s.hasProgressData = false;
      s.progress = 0.0f;
    } else if (rawState == "FINISH" || rawState == "IDLE") {
      s.hasProgressData = false;
    }
  }

  if (printNode["mc_percent"].is<float>()) {
    float progress = printNode["mc_percent"].as<float>();
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 100.0f) progress = 100.0f;
    s.progress = progress;
    if (!s.hasProgressData) {
      Serial.println("First progress update received");
    }
    s.hasProgressData = true;
  }

  if (printNode["mc_remaining_time"].is<int>()) {
    s.mcRemainingTimeSeconds = printNode["mc_remaining_time"].as<int>();
  }

  if (printNode["wifi_signal"].is<const char*>()) {
    const char* sig = printNode["wifi_signal"];
    int val = atoi(sig);  // expects "-52dBm"
    s.wifiSignalDbm = val;
  }

  auto readTemp = [](JsonVariantConst node) -> float {
    if (node.isNull()) {
      return NAN;
    }
    return node.as<float>();
  };

  if (printNode["nozzle_temper"].is<float>()) {
    s.nozzleTemp = readTemp(printNode["nozzle_temper"]);
  }
  if (printNode["nozzle_target_temper"].is<float>()) {
    s.nozzleTarget = readTemp(printNode["nozzle_target_temper"]);
  }
  if (printNode["bed_temper"].is<float>()) {
    s.bedTemp = readTemp(printNode["bed_temper"]);
  }
  if (printNode["bed_target_temper"].is<float>()) {
    s.bedTarget = readTemp(printNode["bed_target_temper"]);
  }

  if (printNode["layer_num"].is<int>() && printNode["total_layer_num"].is<int>()) {
    s.layerNum = printNode["layer_num"].as<int32_t>();
    s.totalLayerNum = printNode["total_layer_num"].as<int32_t>();
    s.hasLayerInfo = true;
  } else {
    s.layerNum = -1;
    s.totalLayerNum = -1;
    s.hasLayerInfo = false;
  }

  bool hasPrintErrorField = printNode["print_error"].is<int32_t>();
  int32_t printErrorCode = hasPrintErrorField ? printNode["print_error"].as<int32_t>() : 0;
  s.printErrorCode = printErrorCode;
  if (printNode["mc_print_stage"].is<const char*>()) {
    s.mcPrintStage = atoi(printNode["mc_print_stage"].as<const char*>());
  } else if (printNode["mc_print_stage"].is<int>()) {
    s.mcPrintStage = printNode["mc_print_stage"].as<int>();
  }
  if (printNode["stg_cur"].is<int>()) {
    s.stgCur = printNode["stg_cur"].as<int>();
  }
  if (ENABLE_LED_SYNC && printNode["lights_report"].is<JsonArrayConst>()) {
    JsonArrayConst lights = printNode["lights_report"].as<JsonArrayConst>();
    bool chamberOn = false;
    for (JsonVariantConst light : lights) {
      const char* node = light["node"] | "";
      const char* mode = light["mode"] | "";
      if (strcmp(node, "chamber_light") == 0) {
        chamberOn = strcmp(mode, "off") != 0;
        if (s.printerLightOn != chamberOn) {
          Serial.printf("Printer light state changed: %s -> %s\n", s.printerLightOn ? "on" : "off",
                        chamberOn ? "on" : "off");
        }
        break;
      }
    }
    s.printerLightOn = chamberOn;
  }

  bool isPrinting = rawState == "RUNNING" || rawState == "BUSY";
  bool isPrepping = rawState == "PREPARE";
  bool isPaused = rawState == "PAUSE";
  bool isIdle = rawState == "IDLE";
  bool isFinished = rawState == "FINISH";
  bool isError = rawState == "FAILED" || rawState == "STOP" || rawState == "ERROR" ||
                 (hasPrintErrorField && printErrorCode != 0);

  bool previousError = s.hasError;
  s.isPrinting = isPrinting;

  bool hasExplicitErrorUpdate = hasState || hasPrintErrorField;
  if (isError) {
    s.hasError = true;
  } else if (hasExplicitErrorUpdate) {
    // Only clear error when we explicitly see a non-error state and (if present) a cleared code.
    bool nonErrorState = hasState && !(rawState == "FAILED" || rawState == "STOP" || rawState == "ERROR");
    bool errorCodeCleared = hasPrintErrorField && printErrorCode == 0;
    if (nonErrorState && (!hasPrintErrorField || errorCodeCleared)) {
      s.hasError = false;
    }
  }

  if (isPrinting || isPrepping) {
    s.lastPrintActiveMs = nowMs;
  }
  bool recentPrint =
      s.lastPrintActiveMs != 0 && (nowMs - s.lastPrintActiveMs) <= FINISH_RECENT_WINDOW_MS;

  String friendly = s.statusText;
  bool shouldUpdateFriendly = hasState || hasPrintErrorField;

  if (s.hasError) {
    friendly = "Error";
  } else if (hasState) {
    if (isPrinting) {
      friendly = "Printing";
    } else if (isPrepping && !s.hasProgressData) {
      friendly = "Prepare";
    } else if (isPaused) {
      friendly = "Paused";
    } else if (isFinished) {
      friendly = recentPrint ? "Finished" : "Idle";
    } else if (isIdle) {
      friendly = "Idle";
    } else {
      friendly = rawState.length() ? rawState : "Unknown";
    }
  } else if (hasPrintErrorField && !s.hasError && previousError) {
    // Error cleared without a state update; fall back to idle
    friendly = "Idle";
  }

  if (shouldUpdateFriendly && s.statusText != friendly) {
    Serial.printf("Printer status changed: %s -> %s\n", s.statusText.c_str(), friendly.c_str());
    s.statusText = friendly;
  } else if (shouldUpdateFriendly) {
    s.statusText = friendly;
  }
}
