#include "led_behavior_config.h"

#include <string.h>

namespace {
constexpr uint16_t LED_FLASH_SPEED_MIN_MS = 50;
constexpr uint16_t LED_FLASH_SPEED_MAX_MS = 10000;
constexpr uint16_t LED_WAVE_SPEED_MIN_MS = 200;
constexpr uint16_t LED_WAVE_SPEED_MAX_MS = 20000;
constexpr uint8_t LED_FLASH_DUTY_MIN = 1;
constexpr uint8_t LED_FLASH_DUTY_MAX = 99;

struct StateDescriptor {
  LedPrinterState state;
  const char* key;
  const char* label;
};

struct ModeDescriptor {
  LedAnimationMode mode;
  const char* key;
  const char* label;
};

constexpr StateDescriptor kStateDescriptors[] = {
    {LedPrinterState::UNKNOWN, "unknown", "Unknown"},   {LedPrinterState::IDLE, "idle", "Idle"},
    {LedPrinterState::PREPARE, "prepare", "Prepare"},   {LedPrinterState::PRINTING, "printing", "Printing"},
    {LedPrinterState::PAUSED, "paused", "Paused"},      {LedPrinterState::FINISHED, "finished", "Finished"},
    {LedPrinterState::ERROR, "error", "Error"},
};

constexpr ModeDescriptor kModeDescriptors[] = {
    {LedAnimationMode::SOLID, "SOLID", "Solid"},
    {LedAnimationMode::FLASH, "FLASH", "Flashing"},
    {LedAnimationMode::SINE, "SINE", "Sine"},
    {LedAnimationMode::BREATHE, "BREATHE", "Breathing"},
};

constexpr size_t stateIndex(LedPrinterState state) { return static_cast<size_t>(state); }

LedConfigValidationResult fail(LedConfigValidationError error, LedPrinterState state) {
  LedConfigValidationResult result{};
  result.ok = false;
  result.error = error;
  result.state = state;
  return result;
}

void setStyle(LedStateStyle* style, LedAnimationMode mode, uint8_t r, uint8_t g, uint8_t b, uint8_t baseline,
              uint16_t speedMs, uint8_t flashDutyPct) {
  if (!style) {
    return;
  }
  style->mode = mode;
  style->color = LedColor{r, g, b};
  style->baselineBrightness = baseline;
  style->speedMs = speedMs;
  style->flashDutyPct = flashDutyPct;
}

bool isKnownMode(LedAnimationMode mode) {
  switch (mode) {
    case LedAnimationMode::SOLID:
    case LedAnimationMode::FLASH:
    case LedAnimationMode::SINE:
    case LedAnimationMode::BREATHE:
      return true;
    default:
      return false;
  }
}
}  // namespace

void clearLedBehaviorConfig(LedBehaviorConfig* config) {
  if (!config) {
    return;
  }
  memset(config, 0, sizeof(LedBehaviorConfig));
}

void setDefaultLedBehaviorConfig(LedBehaviorConfig* config) {
  if (!config) {
    return;
  }

  clearLedBehaviorConfig(config);
  config->maxBrightness = 255;

  setStyle(&config->states[stateIndex(LedPrinterState::UNKNOWN)], LedAnimationMode::SOLID, 180, 200, 255, 96, 1600, 50);
  setStyle(&config->states[stateIndex(LedPrinterState::IDLE)], LedAnimationMode::SOLID, 180, 200, 255, 96, 1600, 50);
  setStyle(&config->states[stateIndex(LedPrinterState::PREPARE)], LedAnimationMode::BREATHE, 255, 180, 0, 32, 2200, 50);
  setStyle(&config->states[stateIndex(LedPrinterState::PRINTING)], LedAnimationMode::SOLID, 255, 255, 255, 255, 1200, 50);
  setStyle(&config->states[stateIndex(LedPrinterState::PAUSED)], LedAnimationMode::FLASH, 255, 150, 0, 0, 1000, 50);
  setStyle(&config->states[stateIndex(LedPrinterState::FINISHED)], LedAnimationMode::SOLID, 0, 255, 0, 255, 1600, 50);
  setStyle(&config->states[stateIndex(LedPrinterState::ERROR)], LedAnimationMode::BREATHE, 255, 0, 0, 38, 2000, 50);
}

void normalizeLedBehaviorConfig(LedBehaviorConfig* config) {
  if (!config) {
    return;
  }

  if (config->maxBrightness == 0) {
    config->maxBrightness = 1;
  }
  for (size_t i = 0; i < LED_PRINTER_STATE_COUNT; ++i) {
    LedStateStyle& style = config->states[i];
    if (style.baselineBrightness > config->maxBrightness) {
      style.baselineBrightness = config->maxBrightness;
    }
    if (style.mode == LedAnimationMode::FLASH) {
      if (style.speedMs < LED_FLASH_SPEED_MIN_MS) {
        style.speedMs = LED_FLASH_SPEED_MIN_MS;
      } else if (style.speedMs > LED_FLASH_SPEED_MAX_MS) {
        style.speedMs = LED_FLASH_SPEED_MAX_MS;
      }
      if (style.flashDutyPct < LED_FLASH_DUTY_MIN) {
        style.flashDutyPct = LED_FLASH_DUTY_MIN;
      } else if (style.flashDutyPct > LED_FLASH_DUTY_MAX) {
        style.flashDutyPct = LED_FLASH_DUTY_MAX;
      }
    } else if (style.mode == LedAnimationMode::SINE || style.mode == LedAnimationMode::BREATHE) {
      if (style.speedMs < LED_WAVE_SPEED_MIN_MS) {
        style.speedMs = LED_WAVE_SPEED_MIN_MS;
      } else if (style.speedMs > LED_WAVE_SPEED_MAX_MS) {
        style.speedMs = LED_WAVE_SPEED_MAX_MS;
      }
      if (style.flashDutyPct < LED_FLASH_DUTY_MIN || style.flashDutyPct > LED_FLASH_DUTY_MAX) {
        style.flashDutyPct = 50;
      }
    } else {
      if (style.flashDutyPct < LED_FLASH_DUTY_MIN || style.flashDutyPct > LED_FLASH_DUTY_MAX) {
        style.flashDutyPct = 50;
      }
    }
  }
}

LedConfigValidationResult validateLedBehaviorConfig(const LedBehaviorConfig& config) {
  if (config.maxBrightness == 0) {
    return fail(LedConfigValidationError::kInvalidMaxBrightness, LedPrinterState::UNKNOWN);
  }

  for (size_t i = 0; i < LED_PRINTER_STATE_COUNT; ++i) {
    const LedPrinterState state = static_cast<LedPrinterState>(i);
    const LedStateStyle& style = config.states[i];
    if (!isKnownMode(style.mode)) {
      return fail(LedConfigValidationError::kInvalidMode, state);
    }
    switch (style.mode) {
      case LedAnimationMode::SOLID:
        break;
      case LedAnimationMode::FLASH:
        if (style.speedMs < LED_FLASH_SPEED_MIN_MS || style.speedMs > LED_FLASH_SPEED_MAX_MS) {
          return fail(LedConfigValidationError::kInvalidSpeed, state);
        }
        if (style.flashDutyPct < LED_FLASH_DUTY_MIN || style.flashDutyPct > LED_FLASH_DUTY_MAX) {
          return fail(LedConfigValidationError::kInvalidDuty, state);
        }
        break;
      case LedAnimationMode::SINE:
      case LedAnimationMode::BREATHE:
        if (style.speedMs < LED_WAVE_SPEED_MIN_MS || style.speedMs > LED_WAVE_SPEED_MAX_MS) {
          return fail(LedConfigValidationError::kInvalidSpeed, state);
        }
        break;
      default:
        return fail(LedConfigValidationError::kInvalidMode, state);
    }
  }

  LedConfigValidationResult result{};
  result.ok = true;
  result.error = LedConfigValidationError::kOk;
  result.state = LedPrinterState::UNKNOWN;
  return result;
}

const char* ledAnimationModeKey(LedAnimationMode mode) {
  for (const ModeDescriptor& descriptor : kModeDescriptors) {
    if (descriptor.mode == mode) {
      return descriptor.key;
    }
  }
  return "SOLID";
}

const char* ledAnimationModeLabel(LedAnimationMode mode) {
  for (const ModeDescriptor& descriptor : kModeDescriptors) {
    if (descriptor.mode == mode) {
      return descriptor.label;
    }
  }
  return "Solid";
}

bool ledAnimationModeFromKey(const char* key, LedAnimationMode* outMode) {
  if (!key || !outMode) {
    return false;
  }
  for (const ModeDescriptor& descriptor : kModeDescriptors) {
    if (strcmp(descriptor.key, key) == 0) {
      *outMode = descriptor.mode;
      return true;
    }
  }
  return false;
}

const char* ledPrinterStateKey(LedPrinterState state) {
  for (const StateDescriptor& descriptor : kStateDescriptors) {
    if (descriptor.state == state) {
      return descriptor.key;
    }
  }
  return "unknown";
}

const char* ledPrinterStateLabel(LedPrinterState state) {
  for (const StateDescriptor& descriptor : kStateDescriptors) {
    if (descriptor.state == state) {
      return descriptor.label;
    }
  }
  return "Unknown";
}

bool ledPrinterStateFromKey(const char* key, LedPrinterState* outState) {
  if (!key || !outState) {
    return false;
  }
  for (const StateDescriptor& descriptor : kStateDescriptors) {
    if (strcmp(descriptor.key, key) == 0) {
      *outState = descriptor.state;
      return true;
    }
  }
  return false;
}

const char* ledConfigValidationCode(const LedConfigValidationResult& result) {
  switch (result.error) {
    case LedConfigValidationError::kOk:
      return "ok";
    case LedConfigValidationError::kInvalidMaxBrightness:
      return "invalid_max_brightness";
    case LedConfigValidationError::kInvalidMode:
      return "invalid_mode";
    case LedConfigValidationError::kInvalidSpeed:
      return "invalid_speed";
    case LedConfigValidationError::kInvalidDuty:
      return "invalid_flash_duty";
    case LedConfigValidationError::kSchemaUnsupported:
      return "schema_unsupported";
    case LedConfigValidationError::kStorageFailure:
      return "storage_failure";
    default:
      return "invalid_led_config";
  }
}

bool runLedConfigValidationSelfTest(Stream& out) {
  LedBehaviorConfig config{};
  setDefaultLedBehaviorConfig(&config);

  bool ok = true;
  LedConfigValidationResult result = validateLedBehaviorConfig(config);
  if (!result.ok) {
    out.println("Self-test failed: expected valid LED config");
    ok = false;
  }

  config.maxBrightness = 0;
  result = validateLedBehaviorConfig(config);
  if (result.ok || result.error != LedConfigValidationError::kInvalidMaxBrightness) {
    out.println("Self-test failed: expected invalid LED max brightness");
    ok = false;
  }

  setDefaultLedBehaviorConfig(&config);
  config.states[stateIndex(LedPrinterState::PAUSED)].flashDutyPct = 0;
  result = validateLedBehaviorConfig(config);
  if (result.ok || result.error != LedConfigValidationError::kInvalidDuty) {
    out.println("Self-test failed: expected invalid LED flash duty");
    ok = false;
  }

  setDefaultLedBehaviorConfig(&config);
  config.states[stateIndex(LedPrinterState::PREPARE)].speedMs = 100;
  result = validateLedBehaviorConfig(config);
  if (result.ok || result.error != LedConfigValidationError::kInvalidSpeed) {
    out.println("Self-test failed: expected invalid LED wave speed");
    ok = false;
  }

  clearLedBehaviorConfig(&config);
  return ok;
}
