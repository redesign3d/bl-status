#include "provisioning_manager.h"

#include <WiFi.h>
#include <string.h>

#include "nvs_config_store.h"

ProvisioningManager::ProvisioningManager()
    : state_(ProvisioningState::BOOT),
      hasActiveConfig_(false),
      apIp_(192, 168, 4, 1),
      provisioningDeadlineMs_(0),
      rebootAtMs_(0),
      nextProvisionRetryMs_(0),
      lastWifiFailureCheckMs_(0),
      wifiFailureCount_(0),
      wifiFailureThreshold_(WIFI_FAILURE_THRESHOLD_BASE) {
  clearDeviceConfig(&activeConfig_);
  memset(apSsid_, 0, sizeof(apSsid_));
}

void ProvisioningManager::begin() {
  state_ = ProvisioningState::CHECK_CONFIG;
  if (RUN_VALIDATION_SELF_TEST_ON_BOOT) {
    runValidationSelfTest(Serial);
  }

  if (!initConfigStore()) {
    Serial.println("Config store unavailable; entering provisioning");
    state_ = ProvisioningState::ENTER_PROVISIONING;
    return;
  }

  if (loadConfigFromNvs()) {
    state_ = ProvisioningState::NORMAL_OPERATION;
    Serial.println("Provisioned config loaded from NVS");
    return;
  }

  Serial.println("No valid provisioned config found");
  state_ = ProvisioningState::ENTER_PROVISIONING;
}

void ProvisioningManager::loop(uint32_t nowMs) {
  if (state_ == ProvisioningState::ENTER_PROVISIONING) {
    if (nowMs < nextProvisionRetryMs_) {
      return;
    }
    if (startProvisioning(nowMs)) {
      state_ = ProvisioningState::PROVISIONING_ACTIVE;
      return;
    }
    nextProvisionRetryMs_ = nowMs + PROVISIONING_AP_RESTART_BACKOFF_MS;
    return;
  }

  if (state_ == ProvisioningState::PROVISIONING_ACTIVE) {
    dns_.loop();
    http_.loop(nowMs);

    if (nowMs > provisioningDeadlineMs_) {
      Serial.println("Provisioning window expired; cycling setup services");
      stopProvisioning();
      state_ = ProvisioningState::ENTER_PROVISIONING;
      nextProvisionRetryMs_ = nowMs + 1000;
    }
    return;
  }

  if (state_ == ProvisioningState::REBOOT_PENDING && nowMs >= rebootAtMs_) {
    Serial.println("Rebooting device");
    delay(100);
    ESP.restart();
  }
}

bool ProvisioningManager::isNormalOperation() const { return state_ == ProvisioningState::NORMAL_OPERATION; }

bool ProvisioningManager::isProvisioningActive() const {
  return state_ == ProvisioningState::ENTER_PROVISIONING || state_ == ProvisioningState::PROVISIONING_ACTIVE;
}

ProvisioningState ProvisioningManager::state() const { return state_; }

const DeviceConfig* ProvisioningManager::activeConfig() const { return hasActiveConfig_ ? &activeConfig_ : nullptr; }

const char* ProvisioningManager::provisioningSsid() const { return apSsid_; }

IPAddress ProvisioningManager::provisioningIp() const { return apIp_; }

void ProvisioningManager::notifyConnectivity(bool wifiConnected, uint32_t nowMs) {
  if (state_ != ProvisioningState::NORMAL_OPERATION) {
    return;
  }
  if (wifiConnected) {
    wifiFailureCount_ = 0;
    lastWifiFailureCheckMs_ = nowMs;
    wifiFailureThreshold_ = WIFI_FAILURE_THRESHOLD_BASE;
    return;
  }
  if ((nowMs - lastWifiFailureCheckMs_) < WIFI_FAILURE_CHECK_INTERVAL_MS) {
    return;
  }
  lastWifiFailureCheckMs_ = nowMs;
  wifiFailureCount_++;
  if (wifiFailureCount_ < wifiFailureThreshold_) {
    return;
  }

  Serial.println("Repeated WiFi failures detected; entering provisioning");
  wifiFailureCount_ = 0;
  if (wifiFailureThreshold_ < WIFI_FAILURE_THRESHOLD_MAX) {
    uint32_t doubled = static_cast<uint32_t>(wifiFailureThreshold_) * 2U;
    wifiFailureThreshold_ =
        (doubled > WIFI_FAILURE_THRESHOLD_MAX) ? WIFI_FAILURE_THRESHOLD_MAX : static_cast<uint16_t>(doubled);
  }
  state_ = ProvisioningState::ENTER_PROVISIONING;
  nextProvisionRetryMs_ = nowMs;
}

void ProvisioningManager::requestFactoryReset() {
  if (!clearProvisionedConfig()) {
    Serial.println("Factory reset request failed");
    return;
  }
  clearDeviceConfig(&activeConfig_);
  hasActiveConfig_ = false;
  stopProvisioning();
  state_ = ProvisioningState::ENTER_PROVISIONING;
  nextProvisionRetryMs_ = millis();
}

bool ProvisioningManager::loadConfigFromNvs() {
  DeviceConfig loaded{};
  if (!loadProvisionedConfig(&loaded)) {
    clearDeviceConfig(&loaded);
    return false;
  }
  activeConfig_ = loaded;
  hasActiveConfig_ = true;
  wifiFailureCount_ = 0;
  lastWifiFailureCheckMs_ = millis();
  return true;
}

bool ProvisioningManager::startProvisioning(uint32_t nowMs) {
  stopProvisioning();
  buildProvisioningSsid();

  if (!softAp_.begin(apSsid_)) {
    Serial.println("Failed to start setup SoftAP");
    return false;
  }
  apIp_ = softAp_.ip();

  if (!dns_.begin(apIp_, PROVISIONING_DNS_PORT)) {
    Serial.println("Failed to start captive DNS");
    stopProvisioning();
    return false;
  }

  if (!http_.begin(PROVISIONING_HTTP_PORT, apSsid_, apIp_)) {
    Serial.println("Failed to start captive HTTP");
    stopProvisioning();
    return false;
  }

  provisioningDeadlineMs_ = nowMs + PROVISIONING_AP_TIMEOUT_MS;

  Serial.println();
  Serial.println("=== Provisioning Mode ===");
  Serial.printf("Setup SSID: %s\n", apSsid_);
  Serial.printf("Setup URL: http://%s/\n", apIp_.toString().c_str());
  Serial.println("Open setup AP is active for onboarding.");
  Serial.println("=========================");
  Serial.println();
  return true;
}

void ProvisioningManager::stopProvisioning() {
  http_.stop();
  dns_.stop();
  softAp_.stop();
}

void ProvisioningManager::scheduleReboot(uint32_t nowMs) {
  state_ = ProvisioningState::REBOOT_PENDING;
  rebootAtMs_ = nowMs + PROVISIONING_REBOOT_DELAY_MS;
}

void ProvisioningManager::buildProvisioningSsid() {
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  snprintf(apSsid_, sizeof(apSsid_), "%s-SETUP-%02X%02X", PRODUCT_NAME, mac[4], mac[5]);
}
