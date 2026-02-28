#include <Arduino.h>

#include "Display.h"
#include "LedController.h"
#include "PrinterClient.h"
#include "PrinterStatus.h"
#include "config.h"
#include "provisioning_manager.h"

namespace {
PrinterStatus gStatus;
PrinterClient gClient;
ProvisioningManager gProvisioning;
bool gClientStarted = false;
bool gDisplayReady = false;
uint32_t lastDisplayMs = 0;
constexpr uint32_t DISPLAY_INTERVAL_MS = 150;  // ~6-7 FPS

void resetStatusFields() {
  gStatus.isPrinting = false;
  gStatus.hasError = false;
  gStatus.progress = 0.0f;
  gStatus.nozzleTemp = NAN;
  gStatus.nozzleTarget = NAN;
  gStatus.bedTemp = NAN;
  gStatus.bedTarget = NAN;
  gStatus.hasLayerInfo = false;
  gStatus.layerNum = -1;
  gStatus.totalLayerNum = -1;
  gStatus.lastUpdateMs = 0;
  gStatus.hasProgressData = false;
  gStatus.lastPrintActiveMs = 0;
  gStatus.mcRemainingTimeSeconds = -1;
  gStatus.wifiSignalDbm = 0;
  gStatus.printErrorCode = 0;
  gStatus.mcPrintStage = -1;
  gStatus.stgCur = -1;
  gStatus.printerLightOn = false;
}

void markDisconnected() {
  if (gStatus.statusText != "Disconnected") {
    Serial.println("Printer status stale; marking as disconnected");
  }
  gStatus.statusText = "Disconnected";
  gStatus.gcodeState = "UNKNOWN";
  gStatus.mqttConnected = false;
  gStatus.wifiConnected = true;
  resetStatusFields();
}

void markWifiConnecting() {
  if (gStatus.statusText != "WiFi") {
    Serial.println("Waiting for WiFi connection...");
  }
  gStatus.statusText = "WiFi";
  gStatus.gcodeState = "WIFI";
  gStatus.wifiConnected = false;
  gStatus.mqttConnected = false;
  resetStatusFields();
}
}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Booting Bambu status monitor...");

  initLeds();
  gDisplayReady = initDisplay();
  if (!gDisplayReady) {
    Serial.println("Display init failed; continuing without OLED output");
  }

  gProvisioning.begin();
  if (gProvisioning.isNormalOperation() && gProvisioning.activeConfig()) {
    gClient.begin(&gStatus, gProvisioning.activeConfig());
    gClientStarted = true;
  }
}

void loop() {
  uint32_t now = millis();

  gProvisioning.loop(now);

  if (gProvisioning.isProvisioningActive()) {
    if (gClientStarted) {
      gClient.stop();
      gClientStarted = false;
    }

    gStatus.statusText = "Provision";
    gStatus.gcodeState = "PROVISION";
    gStatus.hasError = false;
    gStatus.wifiConnected = true;
    gStatus.mqttConnected = false;
    gStatus.progress = 0.0f;
    gStatus.hasProgressData = false;
    gStatus.isPrinting = false;

    updateLeds(gStatus, now);
    if (gDisplayReady && (now - lastDisplayMs >= DISPLAY_INTERVAL_MS)) {
      lastDisplayMs = now;
      drawProvisioningScreen(gProvisioning.provisioningSsid());
    }
    return;
  }

  if (!gClientStarted && gProvisioning.isNormalOperation() && gProvisioning.activeConfig()) {
    gClient.begin(&gStatus, gProvisioning.activeConfig());
    gClientStarted = true;
  }

  if (!gClientStarted) {
    return;
  }

  gClient.loop();

  bool wifiOk = gClient.isWifiConnected();
  bool mqttOk = wifiOk && gClient.isMqttConnected();

  gProvisioning.notifyConnectivity(wifiOk, now);
  if (gProvisioning.isProvisioningActive()) {
    return;
  }

  gStatus.wifiConnected = wifiOk;
  gStatus.mqttConnected = mqttOk;

  bool dataTooOld = (gStatus.lastUpdateMs == 0) || ((now - gStatus.lastUpdateMs) > STATUS_STALE_TIMEOUT_MS);

  if (!wifiOk) {
    markWifiConnecting();
  } else if (!mqttOk && dataTooOld) {
    // Only clear state if we have neither MQTT nor fresh data
    markDisconnected();
  }

  updateLeds(gStatus, now);

  if (gDisplayReady && (now - lastDisplayMs >= DISPLAY_INTERVAL_MS)) {
    lastDisplayMs = now;
    drawStatus(gStatus);
  }
}
