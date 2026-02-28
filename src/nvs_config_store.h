#pragma once

#include <Arduino.h>

#include "RuntimeConfig.h"
#include "led_behavior_config.h"

enum class ConfigValidationError : uint8_t {
  kOk = 0,
  kEmptyField,
  kLengthOutOfRange,
  kInvalidCharacters,
  kInvalidHost,
  kInvalidPort,
  kSchemaUnsupported,
  kStorageFailure,
  kNotProvisioned,
};

struct ConfigValidationResult {
  bool ok;
  ConfigValidationError error;
};

bool initConfigStore();
bool loadProvisionedConfig(DeviceConfig* outConfig);
bool saveProvisionedConfig(const DeviceConfig& config);
bool clearProvisionedConfig();
bool isConfigProvisioned();
ConfigValidationResult validateDeviceConfig(const DeviceConfig& config);
bool loadLedBehaviorConfig(LedBehaviorConfig* outConfig, bool defaultsIfMissing);
bool saveLedBehaviorConfigAtomic(const LedBehaviorConfig& config);
bool clearLedBehaviorConfigStore();
bool runValidationSelfTest(Stream& out);
