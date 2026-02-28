#include "provisioning_manager.h"

#include <WiFi.h>
#include <esp_random.h>
#include <string.h>

#include "nvs_config_store.h"

namespace {
char toHexNibble(uint8_t nibble) { return (nibble < 10) ? static_cast<char>('0' + nibble) : static_cast<char>('A' + (nibble - 10)); }
}  // namespace

ProvisioningManager::ProvisioningManager()
    : state_(ProvisioningState::BOOT),
      hasActiveConfig_(false),
      provisioningDeadlineMs_(0),
      rebootAtMs_(0),
      nextProvisionRetryMs_(0),
      lastWifiFailureCheckMs_(0),
      wifiFailureCount_(0),
      wifiFailureThreshold_(WIFI_FAILURE_THRESHOLD_BASE) {
  clearDeviceConfig(&activeConfig_);
  memset(apSsid_, 0, sizeof(apSsid_));
  memset(apPassphrase_, 0, sizeof(apPassphrase_));
  memset(pairingCode_, 0, sizeof(pairingCode_));
  memset(resetToken_, 0, sizeof(resetToken_));
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
    nextProvisionRetryMs_ = nowMs + 5000;
    return;
  }

  if (state_ == ProvisioningState::PROVISIONING_ACTIVE) {
    dns_.loop();
    http_.loop(nowMs);

    if (nowMs > provisioningDeadlineMs_) {
      Serial.println("Provisioning window expired; restarting provisioning mode");
      stopProvisioning();
      state_ = ProvisioningState::ENTER_PROVISIONING;
      nextProvisionRetryMs_ = nowMs + 1000;
      return;
    }

    if (http_.consumeResetRequest()) {
      Serial.println("Received authenticated factory reset request");
      if (!clearProvisionedConfig()) {
        Serial.println("Factory reset failed");
      } else {
        clearDeviceConfig(&activeConfig_);
        hasActiveConfig_ = false;
      }
      stopProvisioning();
      state_ = ProvisioningState::ENTER_PROVISIONING;
      nextProvisionRetryMs_ = nowMs + 1000;
      return;
    }

    DeviceConfig submitted{};
    if (http_.consumeProvisionRequest(&submitted)) {
      bool saveOk = saveProvisionedConfig(submitted);
      clearDeviceConfig(&submitted);
      if (!saveOk || !loadConfigFromNvs()) {
        Serial.println("Provisioning save failed");
        return;
      }
      Serial.println("Provisioning successful; scheduling reboot");
      stopProvisioning();
      scheduleReboot(nowMs);
      return;
    }
    return;
  }

  if (state_ == ProvisioningState::REBOOT_PENDING) {
    if (nowMs >= rebootAtMs_) {
      Serial.println("Rebooting device");
      delay(100);
      ESP.restart();
    }
  }
}

bool ProvisioningManager::isNormalOperation() const { return state_ == ProvisioningState::NORMAL_OPERATION; }

bool ProvisioningManager::isProvisioningActive() const {
  return state_ == ProvisioningState::ENTER_PROVISIONING || state_ == ProvisioningState::PROVISIONING_ACTIVE;
}

ProvisioningState ProvisioningManager::state() const { return state_; }

const DeviceConfig* ProvisioningManager::activeConfig() const { return hasActiveConfig_ ? &activeConfig_ : nullptr; }

const char* ProvisioningManager::provisioningSsid() const { return apSsid_; }

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
  if (nowMs - lastWifiFailureCheckMs_ < WIFI_FAILURE_CHECK_INTERVAL_MS) {
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
  generateApPassphrase();
  generatePairingCode();
  generateResetToken();

  WiFi.disconnect(true, true);
  delay(50);
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(apSsid_, apPassphrase_, 1, false, 4);
  if (!apOk) {
    Serial.println("Failed to start provisioning SoftAP");
    memset(apPassphrase_, 0, sizeof(apPassphrase_));
    memset(pairingCode_, 0, sizeof(pairingCode_));
    memset(resetToken_, 0, sizeof(resetToken_));
    return false;
  }

  IPAddress apIp = WiFi.softAPIP();
  if (!dns_.begin(apIp, PROVISIONING_DNS_PORT)) {
    Serial.println("Failed to start captive DNS");
    stopProvisioning();
    return false;
  }

  bool httpOk = http_.begin(PROVISIONING_HTTP_PORT, pairingCode_, nowMs + PROVISIONING_PAIRING_TTL_MS, resetToken_);
  if (!httpOk) {
    Serial.println("Failed to start captive HTTP");
    stopProvisioning();
    return false;
  }

  provisioningDeadlineMs_ = nowMs + PROVISIONING_AP_TIMEOUT_MS;

  Serial.println();
  Serial.println("=== Provisioning Mode ===");
  Serial.printf("AP SSID: %s\n", apSsid_);
  Serial.printf("AP Passphrase: %s\n", apPassphrase_);
  Serial.printf("Pairing Code: %s\n", pairingCode_);
  Serial.printf("Portal URL: http://%s/\n", apIp.toString().c_str());
  Serial.println("Credentials shown once; restart device to rotate.");
  Serial.println("=========================");
  Serial.println();

  // Keep AP passphrase in RAM only while provisioning is active.
  return true;
}

void ProvisioningManager::stopProvisioning() {
  http_.stop();
  dns_.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  memset(apPassphrase_, 0, sizeof(apPassphrase_));
  memset(pairingCode_, 0, sizeof(pairingCode_));
  memset(resetToken_, 0, sizeof(resetToken_));
}

void ProvisioningManager::scheduleReboot(uint32_t nowMs) {
  state_ = ProvisioningState::REBOOT_PENDING;
  rebootAtMs_ = nowMs + PROVISIONING_REBOOT_DELAY_MS;
}

void ProvisioningManager::buildProvisioningSsid() {
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  snprintf(apSsid_, sizeof(apSsid_), "BambuStatus-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

void ProvisioningManager::generateApPassphrase() {
  static const char alphabet[] =
      "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789!@#$%^&*";
  constexpr size_t alphabetLen = sizeof(alphabet) - 1;
  for (size_t i = 0; i < sizeof(apPassphrase_) - 1; ++i) {
    apPassphrase_[i] = alphabet[randomByte() % alphabetLen];
  }
  apPassphrase_[sizeof(apPassphrase_) - 1] = '\0';
}

void ProvisioningManager::generatePairingCode() {
  uint32_t code = static_cast<uint32_t>(esp_random() % 1000000);
  snprintf(pairingCode_, sizeof(pairingCode_), "%06lu", static_cast<unsigned long>(code));
}

void ProvisioningManager::generateResetToken() {
  for (size_t i = 0; i < sizeof(resetToken_) - 1; ++i) {
    uint8_t b = randomByte();
    resetToken_[i] = toHexNibble(b & 0x0F);
  }
  resetToken_[sizeof(resetToken_) - 1] = '\0';
}

uint8_t ProvisioningManager::randomByte() { return static_cast<uint8_t>(esp_random() & 0xFF); }
