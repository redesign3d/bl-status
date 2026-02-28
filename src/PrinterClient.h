#pragma once

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "PrinterStatus.h"
#include "RuntimeConfig.h"
#include "config.h"

class PrinterClient {
 public:
  PrinterClient();

  void begin(PrinterStatus* sharedStatus, const DeviceConfig* runtimeConfig);
  void loop();
  void stop();

  bool isWifiConnected() const;
  bool isMqttConnected();

 private:
  void ensureWifi();
  void ensureMqtt();
  void handleMessage(char* topic, uint8_t* payload, unsigned int length);
  void applyStatusFromJson(JsonVariantConst printNode);

  static void handleMqttWrapper(char* topic, uint8_t* payload, unsigned int length);

  WiFiClientSecure wifiClient_;
  PubSubClient mqttClient_;
  PrinterStatus* status_;
  const DeviceConfig* config_;
  unsigned long lastWifiAttemptMs_;
  unsigned long lastMqttAttemptMs_;
  bool wifiReportedConnected_;
  bool mqttReportedConnected_;
  String subscribeTopic_;
};
