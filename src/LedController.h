#pragma once

#include <Adafruit_NeoPixel.h>

#include "PrinterStatus.h"
#include "config.h"
#include "led_behavior_config.h"

void initLeds();
bool applyLedBehaviorConfig(const LedBehaviorConfig& config);
void resetLedBehaviorConfigToDefaults();
const LedBehaviorConfig* currentLedBehaviorConfig();
void updateLeds(const PrinterStatus& status, uint32_t nowMs);
