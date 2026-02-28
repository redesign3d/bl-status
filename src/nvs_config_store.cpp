#include "nvs_config_store.h"

#include <WiFi.h>
#include <ctype.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>

namespace {
constexpr const char* kNamespace = "cfg";
constexpr const char* kKeySchemaVersion = "cv";
constexpr const char* kKeyProvisioned = "pv";
constexpr const char* kKeyWifiSsid = "ws";
constexpr const char* kKeyWifiPassword = "wp";
constexpr const char* kKeyPrinterHost = "ph";
constexpr const char* kKeyPrinterPort = "pp";
constexpr const char* kKeyPrinterSerial = "ps";
constexpr const char* kKeyMqttUsername = "mu";
constexpr const char* kKeyAccessCode = "ac";
constexpr const char* kKeyTlsInsecure = "ti";

constexpr const char* kLedNamespace = "led";
constexpr const char* kKeyLedVersion = "lv";
constexpr const char* kKeyLedMaxBrightness = "mb";
constexpr const char* kKeyLedStates = "st";

bool g_nvsReady = false;

struct StoredLedStateStyleV1 {
  uint8_t mode;
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t baselineBrightness;
  uint16_t speedMs;
  uint8_t flashDutyPct;
};

struct StoredLedStateBlobV1 {
  StoredLedStateStyleV1 states[LED_PRINTER_STATE_COUNT];
};

bool isPrintableAscii(const char* value) {
  if (!value) {
    return false;
  }
  size_t len = strlen(value);
  for (size_t i = 0; i < len; ++i) {
    char c = value[i];
    if (c < 0x20 || c > 0x7E) {
      return false;
    }
  }
  return true;
}

bool containsOnlyTokenChars(const char* value) {
  if (!value) {
    return false;
  }
  size_t len = strlen(value);
  for (size_t i = 0; i < len; ++i) {
    char c = value[i];
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) {
      return false;
    }
  }
  return true;
}

bool isValidHost(const char* host) {
  if (!host) {
    return false;
  }
  size_t len = strlen(host);
  if (len == 0 || len > PRINTER_HOST_MAX_LEN) {
    return false;
  }
  IPAddress ip;
  if (ip.fromString(host)) {
    return true;
  }

  if (host[0] == '.' || host[0] == '-' || host[len - 1] == '.' || host[len - 1] == '-') {
    return false;
  }

  for (size_t i = 0; i < len; ++i) {
    char c = host[i];
    bool ok = isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

ConfigValidationResult fail(ConfigValidationError err) {
  ConfigValidationResult result{};
  result.ok = false;
  result.error = err;
  return result;
}

ConfigValidationResult validateString(const char* value, size_t minLen, size_t maxLen, bool tokenOnly) {
  if (!value) {
    return fail(ConfigValidationError::kEmptyField);
  }
  size_t len = strlen(value);
  if (len < minLen || len > maxLen) {
    return fail(ConfigValidationError::kLengthOutOfRange);
  }
  if (!isPrintableAscii(value)) {
    return fail(ConfigValidationError::kInvalidCharacters);
  }
  if (tokenOnly && !containsOnlyTokenChars(value)) {
    return fail(ConfigValidationError::kInvalidCharacters);
  }
  return ConfigValidationResult{true, ConfigValidationError::kOk};
}

bool readString(nvs_handle_t handle, const char* key, char* dst, size_t dstLen) {
  size_t needed = dstLen;
  esp_err_t err = nvs_get_str(handle, key, dst, &needed);
  if (err != ESP_OK) {
    return false;
  }
  if (needed == 0 || needed > dstLen) {
    return false;
  }
  return true;
}

void copyLedStateToStored(const LedStateStyle& source, StoredLedStateStyleV1* dest) {
  if (!dest) {
    return;
  }
  dest->mode = static_cast<uint8_t>(source.mode);
  dest->r = source.color.r;
  dest->g = source.color.g;
  dest->b = source.color.b;
  dest->baselineBrightness = source.baselineBrightness;
  dest->speedMs = source.speedMs;
  dest->flashDutyPct = source.flashDutyPct;
}

void copyStoredToLedState(const StoredLedStateStyleV1& source, LedStateStyle* dest) {
  if (!dest) {
    return;
  }
  dest->mode = static_cast<LedAnimationMode>(source.mode);
  dest->color = LedColor{source.r, source.g, source.b};
  dest->baselineBrightness = source.baselineBrightness;
  dest->speedMs = source.speedMs;
  dest->flashDutyPct = source.flashDutyPct;
}

bool readLedStateBlob(nvs_handle_t handle, const char* key, StoredLedStateBlobV1* blob) {
  if (!blob) {
    return false;
  }
  size_t expectedSize = sizeof(StoredLedStateBlobV1);
  esp_err_t err = nvs_get_blob(handle, key, blob, &expectedSize);
  return err == ESP_OK && expectedSize == sizeof(StoredLedStateBlobV1);
}

bool readConfigInternal(bool requireProvisioned, DeviceConfig* outConfig) {
  if (!outConfig) {
    return false;
  }
  clearDeviceConfig(outConfig);

  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    return false;
  }

  uint16_t schemaVersion = 0;
  uint8_t provisioned = 0;
  uint8_t tlsInsecure = 0;
  uint16_t port = 0;

  err = nvs_get_u16(handle, kKeySchemaVersion, &schemaVersion);
  if (err != ESP_OK) {
    nvs_close(handle);
    return false;
  }
  if (schemaVersion != CONFIG_SCHEMA_VERSION) {
    nvs_close(handle);
    return false;
  }

  err = nvs_get_u8(handle, kKeyProvisioned, &provisioned);
  if (err != ESP_OK) {
    nvs_close(handle);
    return false;
  }
  if (requireProvisioned && provisioned != 1) {
    nvs_close(handle);
    return false;
  }

  if (!readString(handle, kKeyWifiSsid, outConfig->wifiSsid, sizeof(outConfig->wifiSsid)) ||
      !readString(handle, kKeyWifiPassword, outConfig->wifiPassword, sizeof(outConfig->wifiPassword)) ||
      !readString(handle, kKeyPrinterHost, outConfig->printerHost, sizeof(outConfig->printerHost)) ||
      !readString(handle, kKeyPrinterSerial, outConfig->printerSerial, sizeof(outConfig->printerSerial)) ||
      !readString(handle, kKeyMqttUsername, outConfig->mqttUsername, sizeof(outConfig->mqttUsername)) ||
      !readString(handle, kKeyAccessCode, outConfig->accessCode, sizeof(outConfig->accessCode))) {
    nvs_close(handle);
    clearDeviceConfig(outConfig);
    return false;
  }

  err = nvs_get_u16(handle, kKeyPrinterPort, &port);
  if (err != ESP_OK) {
    nvs_close(handle);
    clearDeviceConfig(outConfig);
    return false;
  }
  outConfig->printerPort = port;

  err = nvs_get_u8(handle, kKeyTlsInsecure, &tlsInsecure);
  if (err != ESP_OK) {
    nvs_close(handle);
    clearDeviceConfig(outConfig);
    return false;
  }
  outConfig->tlsInsecure = tlsInsecure != 0;

  nvs_close(handle);
  return true;
}

bool verifyConfigInStore() {
  DeviceConfig verifyConfig{};
  if (!readConfigInternal(false, &verifyConfig)) {
    clearDeviceConfig(&verifyConfig);
    return false;
  }
  ConfigValidationResult validation = validateDeviceConfig(verifyConfig);
  clearDeviceConfig(&verifyConfig);
  return validation.ok;
}

bool migrateLedConfigIfNeeded(uint8_t version, uint8_t maxBrightness, const StoredLedStateBlobV1& stored,
                              LedBehaviorConfig* outConfig) {
  if (!outConfig) {
    return false;
  }

  switch (version) {
    case LED_CONFIG_SCHEMA_VERSION:
      outConfig->maxBrightness = maxBrightness;
      for (size_t i = 0; i < LED_PRINTER_STATE_COUNT; ++i) {
        copyStoredToLedState(stored.states[i], &outConfig->states[i]);
      }
      normalizeLedBehaviorConfig(outConfig);
      return true;
    default:
      return false;
  }
}

bool readLedConfigInternal(LedBehaviorConfig* outConfig) {
  if (!outConfig) {
    return false;
  }

  clearLedBehaviorConfig(outConfig);

  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(kLedNamespace, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    return false;
  }

  uint8_t version = 0;
  uint8_t maxBrightness = 0;
  StoredLedStateBlobV1 stored{};
  bool ok = false;

  err = nvs_get_u8(handle, kKeyLedVersion, &version);
  if (err == ESP_OK) {
    err = nvs_get_u8(handle, kKeyLedMaxBrightness, &maxBrightness);
  }
  if (err == ESP_OK) {
    ok = readLedStateBlob(handle, kKeyLedStates, &stored);
  }
  nvs_close(handle);

  if (err != ESP_OK || !ok) {
    return false;
  }
  if (!migrateLedConfigIfNeeded(version, maxBrightness, stored, outConfig)) {
    return false;
  }
  LedConfigValidationResult validation = validateLedBehaviorConfig(*outConfig);
  if (!validation.ok) {
    clearLedBehaviorConfig(outConfig);
    return false;
  }
  return true;
}

bool verifyLedConfigInStore(const LedBehaviorConfig& expectedConfig) {
  LedBehaviorConfig stored{};
  if (!readLedConfigInternal(&stored)) {
    return false;
  }

  LedBehaviorConfig expected = expectedConfig;
  normalizeLedBehaviorConfig(&expected);
  bool matches = stored.maxBrightness == expected.maxBrightness;
  if (matches) {
    for (size_t i = 0; i < LED_PRINTER_STATE_COUNT && matches; ++i) {
      const LedStateStyle& lhs = stored.states[i];
      const LedStateStyle& rhs = expected.states[i];
      matches = lhs.mode == rhs.mode && lhs.color.r == rhs.color.r && lhs.color.g == rhs.color.g &&
                lhs.color.b == rhs.color.b && lhs.baselineBrightness == rhs.baselineBrightness &&
                lhs.speedMs == rhs.speedMs && lhs.flashDutyPct == rhs.flashDutyPct;
    }
  }

  clearLedBehaviorConfig(&stored);
  clearLedBehaviorConfig(&expected);
  return matches;
}

bool eraseKeys(nvs_handle_t handle) {
  const char* keys[] = {kKeySchemaVersion, kKeyProvisioned,  kKeyWifiSsid,     kKeyWifiPassword,
                        kKeyPrinterHost,  kKeyPrinterPort,  kKeyPrinterSerial, kKeyMqttUsername,
                        kKeyAccessCode,   kKeyTlsInsecure};
  for (size_t i = 0; i < (sizeof(keys) / sizeof(keys[0])); ++i) {
    esp_err_t err = nvs_erase_key(handle, keys[i]);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
      return false;
    }
  }
  return true;
}
}  // namespace

bool initConfigStore() {
  if (g_nvsReady) {
    return true;
  }
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    esp_err_t eraseErr = nvs_flash_erase();
    if (eraseErr != ESP_OK) {
      Serial.printf("NVS erase failed: %d\n", static_cast<int>(eraseErr));
      return false;
    }
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    Serial.printf("NVS init failed: %d\n", static_cast<int>(err));
    return false;
  }
  g_nvsReady = true;
  return true;
}

ConfigValidationResult validateDeviceConfig(const DeviceConfig& config) {
  ConfigValidationResult r = validateString(config.wifiSsid, 1, WIFI_SSID_MAX_LEN, false);
  if (!r.ok) {
    return r;
  }
  size_t wifiPasswordLen = strlen(config.wifiPassword);
  if (wifiPasswordLen > WIFI_PASSWORD_MAX_LEN) {
    return fail(ConfigValidationError::kLengthOutOfRange);
  }
  if (!isPrintableAscii(config.wifiPassword)) {
    return fail(ConfigValidationError::kInvalidCharacters);
  }
  if (wifiPasswordLen > 0 && wifiPasswordLen < 8) {
    return fail(ConfigValidationError::kLengthOutOfRange);
  }
  if (!isValidHost(config.printerHost)) {
    return fail(ConfigValidationError::kInvalidHost);
  }
  if (config.printerPort < MIN_MQTT_PORT || config.printerPort > MAX_MQTT_PORT) {
    return fail(ConfigValidationError::kInvalidPort);
  }
  r = validateString(config.printerSerial, 6, PRINTER_SERIAL_MAX_LEN, true);
  if (!r.ok) {
    return r;
  }
  r = validateString(config.mqttUsername, 1, MQTT_USERNAME_MAX_LEN, true);
  if (!r.ok) {
    return r;
  }
  r = validateString(config.accessCode, 4, ACCESS_CODE_MAX_LEN, true);
  if (!r.ok) {
    return r;
  }
  return ConfigValidationResult{true, ConfigValidationError::kOk};
}

bool saveProvisionedConfig(const DeviceConfig& config) {
  if (!initConfigStore()) {
    return false;
  }

  ConfigValidationResult validation = validateDeviceConfig(config);
  if (!validation.ok) {
    return false;
  }

  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    Serial.printf("NVS open write failed: %d\n", static_cast<int>(err));
    return false;
  }

  if (!eraseKeys(handle)) {
    nvs_close(handle);
    return false;
  }

  err = nvs_set_u16(handle, kKeySchemaVersion, CONFIG_SCHEMA_VERSION);
  if (err == ESP_OK) err = nvs_set_u8(handle, kKeyProvisioned, 0);
  if (err == ESP_OK) err = nvs_set_str(handle, kKeyWifiSsid, config.wifiSsid);
  if (err == ESP_OK) err = nvs_set_str(handle, kKeyWifiPassword, config.wifiPassword);
  if (err == ESP_OK) err = nvs_set_str(handle, kKeyPrinterHost, config.printerHost);
  if (err == ESP_OK) err = nvs_set_u16(handle, kKeyPrinterPort, config.printerPort);
  if (err == ESP_OK) err = nvs_set_str(handle, kKeyPrinterSerial, config.printerSerial);
  if (err == ESP_OK) err = nvs_set_str(handle, kKeyMqttUsername, config.mqttUsername);
  if (err == ESP_OK) err = nvs_set_str(handle, kKeyAccessCode, config.accessCode);
  if (err == ESP_OK) err = nvs_set_u8(handle, kKeyTlsInsecure, config.tlsInsecure ? 1 : 0);
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);

  if (err != ESP_OK) {
    Serial.printf("NVS commit config failed: %d\n", static_cast<int>(err));
    return false;
  }

  if (!verifyConfigInStore()) {
    Serial.println("Config verify failed after write");
    return false;
  }

  err = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return false;
  }
  err = nvs_set_u8(handle, kKeyProvisioned, 1);
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  if (err != ESP_OK) {
    Serial.printf("NVS set provisioned failed: %d\n", static_cast<int>(err));
    return false;
  }

  DeviceConfig finalCheck{};
  bool ok = loadProvisionedConfig(&finalCheck);
  clearDeviceConfig(&finalCheck);
  return ok;
}

bool loadProvisionedConfig(DeviceConfig* outConfig) {
  if (!outConfig || !initConfigStore()) {
    return false;
  }
  if (!readConfigInternal(true, outConfig)) {
    return false;
  }
  ConfigValidationResult validation = validateDeviceConfig(*outConfig);
  if (!validation.ok) {
    clearDeviceConfig(outConfig);
    return false;
  }
  return true;
}

bool clearProvisionedConfig() {
  if (!initConfigStore()) {
    return false;
  }

  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return false;
  }
  bool ok = eraseKeys(handle);
  if (ok) {
    err = nvs_commit(handle);
    ok = (err == ESP_OK);
  }
  nvs_close(handle);
  return ok;
}

bool isConfigProvisioned() {
  DeviceConfig config{};
  bool ok = loadProvisionedConfig(&config);
  clearDeviceConfig(&config);
  return ok;
}

bool loadLedBehaviorConfig(LedBehaviorConfig* outConfig, bool defaultsIfMissing) {
  if (!outConfig || !initConfigStore()) {
    return false;
  }

  if (readLedConfigInternal(outConfig)) {
    return true;
  }

  if (!defaultsIfMissing) {
    return false;
  }

  setDefaultLedBehaviorConfig(outConfig);
  return true;
}

bool saveLedBehaviorConfigAtomic(const LedBehaviorConfig& config) {
  if (!initConfigStore()) {
    return false;
  }

  LedBehaviorConfig normalized = config;
  normalizeLedBehaviorConfig(&normalized);
  LedConfigValidationResult validation = validateLedBehaviorConfig(normalized);
  if (!validation.ok) {
    clearLedBehaviorConfig(&normalized);
    return false;
  }

  StoredLedStateBlobV1 stored{};
  for (size_t i = 0; i < LED_PRINTER_STATE_COUNT; ++i) {
    copyLedStateToStored(normalized.states[i], &stored.states[i]);
  }

  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(kLedNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    clearLedBehaviorConfig(&normalized);
    return false;
  }

  err = nvs_set_u8(handle, kKeyLedVersion, LED_CONFIG_SCHEMA_VERSION);
  if (err == ESP_OK) {
    err = nvs_set_u8(handle, kKeyLedMaxBrightness, normalized.maxBrightness);
  }
  if (err == ESP_OK) {
    err = nvs_set_blob(handle, kKeyLedStates, &stored, sizeof(stored));
  }
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);

  if (err != ESP_OK) {
    clearLedBehaviorConfig(&normalized);
    return false;
  }

  const bool ok = verifyLedConfigInStore(normalized);
  clearLedBehaviorConfig(&normalized);
  return ok;
}

bool clearLedBehaviorConfigStore() {
  if (!initConfigStore()) {
    return false;
  }

  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(kLedNamespace, NVS_READWRITE, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return true;
  }
  if (err != ESP_OK) {
    return false;
  }

  const char* keys[] = {kKeyLedVersion, kKeyLedMaxBrightness, kKeyLedStates};
  bool ok = true;
  for (size_t i = 0; i < (sizeof(keys) / sizeof(keys[0])); ++i) {
    err = nvs_erase_key(handle, keys[i]);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
      ok = false;
      break;
    }
  }
  if (ok) {
    err = nvs_commit(handle);
    ok = (err == ESP_OK);
  }
  nvs_close(handle);
  return ok;
}

bool runValidationSelfTest(Stream& out) {
  DeviceConfig cfg{};
  strlcpy(cfg.wifiSsid, "QA-Network", sizeof(cfg.wifiSsid));
  strlcpy(cfg.wifiPassword, "CorrectHorseBattery", sizeof(cfg.wifiPassword));
  strlcpy(cfg.printerHost, "192.168.1.50", sizeof(cfg.printerHost));
  cfg.printerPort = DEFAULT_MQTT_TLS_PORT;
  strlcpy(cfg.printerSerial, "03919C452112094", sizeof(cfg.printerSerial));
  strlcpy(cfg.mqttUsername, "bblp", sizeof(cfg.mqttUsername));
  strlcpy(cfg.accessCode, "ABC12345", sizeof(cfg.accessCode));
  cfg.tlsInsecure = false;

  bool ok = true;
  ConfigValidationResult r = validateDeviceConfig(cfg);
  if (!r.ok) {
    out.println("Self-test failed: expected valid config");
    ok = false;
  }

  cfg.printerPort = 0;
  r = validateDeviceConfig(cfg);
  if (r.ok) {
    out.println("Self-test failed: expected invalid port");
    ok = false;
  }
  cfg.printerPort = DEFAULT_MQTT_TLS_PORT;

  strlcpy(cfg.wifiPassword, "short", sizeof(cfg.wifiPassword));
  r = validateDeviceConfig(cfg);
  if (r.ok) {
    out.println("Self-test failed: expected invalid wifi password");
    ok = false;
  }
  strlcpy(cfg.wifiPassword, "CorrectHorseBattery", sizeof(cfg.wifiPassword));

  strlcpy(cfg.printerHost, "bad host@", sizeof(cfg.printerHost));
  r = validateDeviceConfig(cfg);
  if (r.ok) {
    out.println("Self-test failed: expected invalid host");
    ok = false;
  }

  if (!runLedConfigValidationSelfTest(out)) {
    ok = false;
  }

  clearDeviceConfig(&cfg);
  return ok;
}
