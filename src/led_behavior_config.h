#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "config.h"

enum class LedAnimationMode : uint8_t {
  SOLID = 0,
  FLASH,
  SINE,
  BREATHE,
};

enum class LedPrinterState : uint8_t {
  UNKNOWN = 0,
  IDLE,
  PREPARE,
  PRINTING,
  PAUSED,
  FINISHED,
  ERROR,
  COUNT,
};

constexpr uint8_t LED_CONFIG_SCHEMA_VERSION = 1;
constexpr size_t LED_PRINTER_STATE_COUNT = static_cast<size_t>(LedPrinterState::COUNT);

struct LedColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

struct LedStateStyle {
  LedAnimationMode mode;
  LedColor color;
  uint8_t baselineBrightness;
  uint16_t speedMs;
  uint8_t flashDutyPct;
};

struct LedBehaviorConfig {
  uint8_t maxBrightness;
  LedStateStyle states[LED_PRINTER_STATE_COUNT];
};

enum class LedConfigValidationError : uint8_t {
  kOk = 0,
  kInvalidMaxBrightness,
  kInvalidMode,
  kInvalidSpeed,
  kInvalidDuty,
  kSchemaUnsupported,
  kStorageFailure,
};

struct LedConfigValidationResult {
  bool ok;
  LedConfigValidationError error;
  LedPrinterState state;
};

void clearLedBehaviorConfig(LedBehaviorConfig* config);
void setDefaultLedBehaviorConfig(LedBehaviorConfig* config);
void normalizeLedBehaviorConfig(LedBehaviorConfig* config);
LedConfigValidationResult validateLedBehaviorConfig(const LedBehaviorConfig& config);

const char* ledAnimationModeKey(LedAnimationMode mode);
const char* ledAnimationModeLabel(LedAnimationMode mode);
bool ledAnimationModeFromKey(const char* key, LedAnimationMode* outMode);

const char* ledPrinterStateKey(LedPrinterState state);
const char* ledPrinterStateLabel(LedPrinterState state);
bool ledPrinterStateFromKey(const char* key, LedPrinterState* outState);

const char* ledConfigValidationCode(const LedConfigValidationResult& result);

bool runLedConfigValidationSelfTest(Stream& out);
