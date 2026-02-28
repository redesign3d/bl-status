#include "provisioning_manager.h"

#include <WiFi.h>
#include <esp_random.h>
#include <string.h>

#include "LogRedaction.h"
#include "admin_auth_store.h"
#include "device_identity.h"
#include "nvs_config_store.h"

namespace {
char toHexNibble(uint8_t nibble) { return (nibble < 10) ? static_cast<char>('0' + nibble) : static_cast<char>('A' + (nibble - 10)); }
}  // namespace

ProvisioningManager::ProvisioningManager()
    : state_(ProvisioningState::BOOT),
      hasActiveConfig_(false),
      provisioningCompletePending_(false),
      apIp_(192, 168, 4, 1),
      provisioningDeadlineMs_(0),
      rebootAtMs_(0),
      nextProvisionRetryMs_(0),
      adminAnnouncementUntilMs_(0),
      lastWifiFailureCheckMs_(0),
      wifiFailureCount_(0),
      wifiFailureThreshold_(WIFI_FAILURE_THRESHOLD_BASE) {
  clearDeviceConfig(&activeConfig_);
  clearDeviceConfig(&draftConfig_);
  memset(apSsid_, 0, sizeof(apSsid_));
  memset(resetToken_, 0, sizeof(resetToken_));
  memset(pendingAdminUser_, 0, sizeof(pendingAdminUser_));
  memset(pendingAdminPass_, 0, sizeof(pendingAdminPass_));
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
    AdminCredentials credentials{};
    bool generated = false;
    if (ensureLocalAdminCredentials(&credentials, &generated) && generated) {
      announceGeneratedAdminCredentials(credentials, "Generated for existing provisioned device");
    }
    clearAdminCredentialsStruct(&credentials);
    state_ = ProvisioningState::NORMAL_OPERATION;
    Serial.println("Provisioned config loaded from NVS");
    return;
  }

  Serial.println("No valid provisioned config found");
  state_ = ProvisioningState::ENTER_PROVISIONING;
}

void ProvisioningManager::loop(uint32_t nowMs) {
  if (adminAnnouncementUntilMs_ != 0 && nowMs >= adminAnnouncementUntilMs_) {
    clearPendingAdminCredentials();
  }

  if (state_ == ProvisioningState::NORMAL_OPERATION) {
    localPortal_.loop(nowMs);
  }

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
    improv_.loop(nowMs);

    if (provisioningCompletePending_) {
      provisioningCompletePending_ = false;
      Serial.println("Provisioning successful; scheduling reboot");
      stopProvisioning();
      scheduleReboot(nowMs);
      return;
    }

    if (nowMs > provisioningDeadlineMs_) {
      Serial.println("Provisioning window expired; cycling setup services");
      stopProvisioning();
      state_ = ProvisioningState::ENTER_PROVISIONING;
      nextProvisionRetryMs_ = nowMs + 1000;
    }
    return;
  }

  if (state_ == ProvisioningState::REBOOT_PENDING) {
    stopRuntimeServices();
    stopProvisioning();
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

bool ProvisioningManager::isRebootPending() const { return state_ == ProvisioningState::REBOOT_PENDING; }

ProvisioningState ProvisioningManager::state() const { return state_; }

const DeviceConfig* ProvisioningManager::activeConfig() const { return hasActiveConfig_ ? &activeConfig_ : nullptr; }

const char* ProvisioningManager::provisioningSsid() const { return apSsid_; }

IPAddress ProvisioningManager::provisioningIp() const { return apIp_; }

bool ProvisioningManager::shouldShowAdminPassword(uint32_t nowMs) const {
  return pendingAdminPass_[0] != '\0' && adminAnnouncementUntilMs_ != 0 && nowMs < adminAnnouncementUntilMs_;
}

const char* ProvisioningManager::adminUsernameForDisplay() const {
  return pendingAdminUser_[0] != '\0' ? pendingAdminUser_ : nullptr;
}

const char* ProvisioningManager::adminPasswordForDisplay() const {
  return pendingAdminPass_[0] != '\0' ? pendingAdminPass_ : nullptr;
}

void ProvisioningManager::notifyConnectivity(bool wifiConnected, uint32_t nowMs) {
  if (state_ != ProvisioningState::NORMAL_OPERATION) {
    stopRuntimeServices();
    return;
  }
  if (wifiConnected) {
    wifiFailureCount_ = 0;
    lastWifiFailureCheckMs_ = nowMs;
    wifiFailureThreshold_ = WIFI_FAILURE_THRESHOLD_BASE;
    updateRuntimeServices(true);
    return;
  }
  stopRuntimeServices();
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
  char message[96];
  resetProvisioningConfig(message, sizeof(message));
  clearPendingAdminCredentials();
  stopRuntimeServices();
  stopProvisioning();
  state_ = ProvisioningState::ENTER_PROVISIONING;
  nextProvisionRetryMs_ = millis();
}

bool ProvisioningManager::applySubmittedConfig(const DeviceConfig& config, char* message, size_t messageLen) {
  if (message && messageLen > 0) {
    message[0] = '\0';
  }

  AdminCredentials credentials{};
  bool generated = false;
  if (!ensureLocalAdminCredentials(&credentials, &generated)) {
    if (message && messageLen > 0) {
      strlcpy(message, "Admin credential initialization failed.", messageLen);
    }
    return false;
  }

  if (!saveProvisionedConfig(config)) {
    if (generated) {
      clearAdminCredentials();
    }
    clearAdminCredentialsStruct(&credentials);
    if (message && messageLen > 0) {
      strlcpy(message, "Configuration save failed.", messageLen);
    }
    return false;
  }

  if (!loadConfigFromNvs()) {
    if (generated) {
      clearAdminCredentials();
    }
    clearAdminCredentialsStruct(&credentials);
    if (message && messageLen > 0) {
      strlcpy(message, "Configuration verify failed.", messageLen);
    }
    return false;
  }

  if (generated) {
    announceGeneratedAdminCredentials(credentials, "Generated during provisioning");
  }
  clearAdminCredentialsStruct(&credentials);
  provisioningCompletePending_ = true;
  if (message && messageLen > 0) {
    strlcpy(message, "Saved. Rebooting now.", messageLen);
  }
  return true;
}

bool ProvisioningManager::resetProvisioningConfig(char* message, size_t messageLen) {
  if (message && messageLen > 0) {
    message[0] = '\0';
  }

  if (!clearProvisionedConfig()) {
    if (message && messageLen > 0) {
      strlcpy(message, "Configuration erase failed.", messageLen);
    }
    return false;
  }
  clearAdminCredentials();

  clearDeviceConfig(&activeConfig_);
  clearDeviceConfig(&draftConfig_);
  clearPendingAdminCredentials();
  hasActiveConfig_ = false;
  refreshDraftInPortal();
  if (message && messageLen > 0) {
    strlcpy(message, "Configuration erased. Device remains in setup mode.", messageLen);
  }
  return true;
}

bool ProvisioningManager::handleImprovWifiSettings(const char* ssid, const char* password, char* url, size_t urlLen,
                                                   char* message, size_t messageLen) {
  if (!ssid || !password || !url || urlLen == 0) {
    if (message && messageLen > 0) {
      strlcpy(message, "Invalid Improv payload.", messageLen);
    }
    return false;
  }

  const size_t ssidLen = strlen(ssid);
  const size_t passwordLen = strlen(password);
  if (ssidLen == 0 || ssidLen > WIFI_SSID_MAX_LEN || passwordLen > WIFI_PASSWORD_MAX_LEN || !isPrintableAscii(ssid) ||
      !isPrintableAscii(password) || (passwordLen > 0 && passwordLen < 8)) {
    if (message && messageLen > 0) {
      strlcpy(message, "Wi-Fi credentials rejected.", messageLen);
    }
    return false;
  }

  Serial.printf("Improv Wi-Fi request received for SSID %s\n", redactForLog(ssid).c_str());

  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  delay(100);
  if (passwordLen == 0) {
    WiFi.begin(ssid);
  } else {
    WiFi.begin(ssid, password);
  }

  wl_status_t status = WiFi.status();
  for (uint8_t attempt = 0; attempt < IMPROV_WIFI_CONNECT_ATTEMPTS && status != WL_CONNECTED; ++attempt) {
    delay(IMPROV_WIFI_CONNECT_RETRY_MS);
    status = WiFi.status();
  }

  if (status != WL_CONNECTED) {
    WiFi.disconnect(false, false);
    if (message && messageLen > 0) {
      strlcpy(message, "Unable to connect to Wi-Fi.", messageLen);
    }
    return false;
  }

  strlcpy(draftConfig_.wifiSsid, ssid, sizeof(draftConfig_.wifiSsid));
  strlcpy(draftConfig_.wifiPassword, password, sizeof(draftConfig_.wifiPassword));
  refreshDraftInPortal();
  provisioningDeadlineMs_ = millis() + PROVISIONING_AP_TIMEOUT_MS;

  const IPAddress localIp = WiFi.localIP();
  snprintf(url, urlLen, "http://%u.%u.%u.%u/", localIp[0], localIp[1], localIp[2], localIp[3]);

  ConfigValidationResult validation = validateDeviceConfig(draftConfig_);
  if (!validation.ok) {
    if (message && messageLen > 0) {
      strlcpy(message, "Wi-Fi connected. Finish setup at the local URL.", messageLen);
    }
    return true;
  }

  char saveMessage[96];
  memset(saveMessage, 0, sizeof(saveMessage));
  if (!applySubmittedConfig(draftConfig_, saveMessage, sizeof(saveMessage))) {
    if (message && messageLen > 0) {
      strlcpy(message, saveMessage[0] != '\0' ? saveMessage : "Configuration save failed.", messageLen);
    }
    return false;
  }

  if (message && messageLen > 0) {
    strlcpy(message, "Wi-Fi connected and configuration saved.", messageLen);
  }
  return true;
}

bool ProvisioningManager::saveRuntimeConfig(const DeviceConfig& config, char* message, size_t messageLen) {
  if (message && messageLen > 0) {
    message[0] = '\0';
  }

  AdminCredentials credentials{};
  bool generated = false;
  if (!ensureLocalAdminCredentials(&credentials, &generated)) {
    if (message && messageLen > 0) {
      strlcpy(message, "Admin credential initialization failed.", messageLen);
    }
    return false;
  }

  if (!saveProvisionedConfig(config) || !loadConfigFromNvs()) {
    if (generated) {
      clearAdminCredentials();
    }
    clearAdminCredentialsStruct(&credentials);
    if (message && messageLen > 0) {
      strlcpy(message, "Configuration save failed.", messageLen);
    }
    return false;
  }

  if (generated) {
    announceGeneratedAdminCredentials(credentials, "Generated during LAN portal save");
  }
  clearAdminCredentialsStruct(&credentials);
  scheduleReboot(millis());
  if (message && messageLen > 0) {
    strlcpy(message, "Saved. Rebooting now.", messageLen);
  }
  return true;
}

bool ProvisioningManager::requestRuntimeReboot(char* message, size_t messageLen) {
  scheduleReboot(millis());
  if (message && messageLen > 0) {
    strlcpy(message, "Device rebooting.", messageLen);
  }
  return true;
}

bool ProvisioningManager::requestFactoryResetAndReboot(char* message, size_t messageLen) {
  if (!clearProvisionedConfig() || !clearAdminCredentials()) {
    if (message && messageLen > 0) {
      strlcpy(message, "Factory reset failed.", messageLen);
    }
    return false;
  }

  clearPendingAdminCredentials();
  scheduleReboot(millis());
  if (message && messageLen > 0) {
    strlcpy(message, "Configuration cleared. Rebooting into setup mode.", messageLen);
  }
  return true;
}

bool ProvisioningManager::loadConfigFromNvs() {
  DeviceConfig loaded{};
  if (!loadProvisionedConfig(&loaded)) {
    clearDeviceConfig(&loaded);
    return false;
  }
  activeConfig_ = loaded;
  draftConfig_ = loaded;
  hasActiveConfig_ = true;
  wifiFailureCount_ = 0;
  lastWifiFailureCheckMs_ = millis();
  return true;
}

bool ProvisioningManager::startProvisioning(uint32_t nowMs) {
  stopRuntimeServices();
  stopProvisioning();
  buildProvisioningSsid();
  generateResetToken();
  provisioningCompletePending_ = false;

  if (hasActiveConfig_) {
    draftConfig_ = activeConfig_;
  } else {
    clearDeviceConfig(&draftConfig_);
    draftConfig_.printerPort = DEFAULT_MQTT_TLS_PORT;
    strlcpy(draftConfig_.mqttUsername, DEFAULT_MQTT_USERNAME, sizeof(draftConfig_.mqttUsername));
    draftConfig_.tlsInsecure = DEFAULT_TLS_INSECURE;
  }

  if (!softAp_.begin(apSsid_)) {
    Serial.println("Failed to start setup SoftAP");
    memset(resetToken_, 0, sizeof(resetToken_));
    return false;
  }
  apIp_ = softAp_.ip();

  if (!dns_.begin(apIp_, PROVISIONING_DNS_PORT)) {
    Serial.println("Failed to start captive DNS");
    stopProvisioning();
    return false;
  }

  if (!http_.begin(PROVISIONING_HTTP_PORT, apSsid_, apIp_, resetToken_, this)) {
    Serial.println("Failed to start captive HTTP");
    stopProvisioning();
    return false;
  }
  refreshDraftInPortal();
  improv_.begin(Serial, this);

  provisioningDeadlineMs_ = nowMs + PROVISIONING_AP_TIMEOUT_MS;

  Serial.println();
  Serial.println("=== Provisioning Mode ===");
  Serial.printf("Setup SSID: %s\n", apSsid_);
  Serial.printf("Setup URL: http://%s/\n", apIp_.toString().c_str());
  Serial.println("Open setup AP and Improv serial are active for onboarding.");
  Serial.println("=========================");
  Serial.println();
  return true;
}

void ProvisioningManager::stopProvisioning() {
  improv_.stop();
  http_.stop();
  dns_.stop();
  softAp_.stop();
  memset(resetToken_, 0, sizeof(resetToken_));
}

void ProvisioningManager::scheduleReboot(uint32_t nowMs) {
  state_ = ProvisioningState::REBOOT_PENDING;
  const uint32_t defaultRebootAt = nowMs + PROVISIONING_REBOOT_DELAY_MS;
  rebootAtMs_ = defaultRebootAt;
  if (adminAnnouncementUntilMs_ > rebootAtMs_) {
    rebootAtMs_ = adminAnnouncementUntilMs_;
  }
}

void ProvisioningManager::buildProvisioningSsid() {
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  snprintf(apSsid_, sizeof(apSsid_), "%s-SETUP-%02X%02X", PRODUCT_NAME, mac[4], mac[5]);
}

void ProvisioningManager::generateResetToken() {
  for (size_t i = 0; i < sizeof(resetToken_) - 1; ++i) {
    resetToken_[i] = toHexNibble(randomByte() & 0x0F);
  }
  resetToken_[sizeof(resetToken_) - 1] = '\0';
}

uint8_t ProvisioningManager::randomByte() { return static_cast<uint8_t>(esp_random() & 0xFF); }

bool ProvisioningManager::isPrintableAscii(const char* value) const {
  if (!value) {
    return false;
  }
  for (size_t i = 0; value[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c < 0x20 || c > 0x7E) {
      return false;
    }
  }
  return true;
}

void ProvisioningManager::refreshDraftInPortal() { http_.setDraftConfig(&draftConfig_); }

bool ProvisioningManager::ensureLocalAdminCredentials(AdminCredentials* credentials, bool* generated) {
  return ensureAdminCredentials(credentials, generated);
}

void ProvisioningManager::setPendingAdminCredentials(const AdminCredentials& credentials, uint32_t nowMs) {
  strlcpy(pendingAdminUser_, credentials.username, sizeof(pendingAdminUser_));
  strlcpy(pendingAdminPass_, credentials.password, sizeof(pendingAdminPass_));
  adminAnnouncementUntilMs_ = nowMs + ADMIN_PASSWORD_ANNOUNCE_MS;
}

void ProvisioningManager::clearPendingAdminCredentials() {
  memset(pendingAdminUser_, 0, sizeof(pendingAdminUser_));
  memset(pendingAdminPass_, 0, sizeof(pendingAdminPass_));
  adminAnnouncementUntilMs_ = 0;
}

void ProvisioningManager::announceGeneratedAdminCredentials(const AdminCredentials& credentials, const char* reason) {
  setPendingAdminCredentials(credentials, millis());
  Serial.println();
  Serial.println("=== Local Portal Credentials ===");
  if (reason && reason[0] != '\0') {
    Serial.println(reason);
  }
  Serial.printf("URL: http://%s.local/\n", getHostname().c_str());
  Serial.printf("Admin user: %s\n", credentials.username);
  Serial.printf("Admin password: %s\n", credentials.password);
  Serial.println("===============================");
  Serial.println();
}

void ProvisioningManager::updateRuntimeServices(bool wifiConnected) {
  if (!wifiConnected || !hasActiveConfig_ || WiFi.status() != WL_CONNECTED || WiFi.localIP()[0] == 0) {
    stopRuntimeServices();
    return;
  }

  if (!localPortal_.isRunning()) {
    if (!localPortal_.begin(LOCAL_CONFIG_PORTAL_PORT, this)) {
      Serial.println("Failed to start local config portal");
      stopRuntimeServices();
      return;
    }
  }

  mdns_.updateMdns(localPortal_.isRunning(), getHostname().c_str(), LOCAL_CONFIG_PORTAL_PORT);
}

void ProvisioningManager::stopRuntimeServices() {
  mdns_.endMdns();
  localPortal_.stop();
}
