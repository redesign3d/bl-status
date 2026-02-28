#include "LedController.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "nvs_config_store.h"

namespace {
constexpr uint8_t BRIGHTNESS_WIFI = 128;
constexpr uint8_t BRIGHTNESS_DIM = 51;
constexpr uint8_t BRIGHTNESS_FULL = 255;

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
LedBehaviorConfig g_ledConfig{};

uint8_t heartbeatIndex = 0;
uint32_t lastHeartbeatMs = 0;
bool pausePhase = false;
uint32_t lastPauseToggleMs = 0;
bool finishHighlightActive = false;
uint32_t finishHighlightStartMs = 0;
bool wasFinishedState = false;

uint8_t clampByte(int value) {
  if (value < 0) return 0;
  if (value > 255) return 255;
  return static_cast<uint8_t>(value);
}

size_t stateIndex(LedPrinterState state) { return static_cast<size_t>(state); }

uint8_t capBrightness(uint8_t value) {
  const uint8_t cap = (g_ledConfig.maxBrightness == 0) ? 1 : g_ledConfig.maxBrightness;
  return (value > cap) ? cap : value;
}

uint32_t scaledColor(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
  const float scale = capBrightness(brightness) / 255.0f;
  return strip.Color(clampByte(static_cast<int>(roundf(r * scale))), clampByte(static_cast<int>(roundf(g * scale))),
                     clampByte(static_cast<int>(roundf(b * scale))));
}

uint32_t scaledColor(const LedColor& color, uint8_t brightness) {
  return scaledColor(color.r, color.g, color.b, brightness);
}

uint8_t styleBrightness(const LedStateStyle& style, uint32_t nowMs) {
  const uint8_t ceiling = (g_ledConfig.maxBrightness == 0) ? 1 : g_ledConfig.maxBrightness;
  const uint8_t floor = (style.baselineBrightness > ceiling) ? ceiling : style.baselineBrightness;
  const uint16_t period = (style.speedMs == 0) ? 1 : style.speedMs;

  switch (style.mode) {
    case LedAnimationMode::SOLID:
      return floor;
    case LedAnimationMode::FLASH: {
      const uint32_t phaseMs = nowMs % period;
      const uint32_t onWindow = (static_cast<uint32_t>(period) * style.flashDutyPct) / 100U;
      return (phaseMs < onWindow) ? ceiling : floor;
    }
    case LedAnimationMode::SINE: {
      const float phase = (nowMs % period) / static_cast<float>(period);
      const float wave = 0.5f * (sinf(phase * TWO_PI) + 1.0f);
      return static_cast<uint8_t>(roundf(floor + (ceiling - floor) * wave));
    }
    case LedAnimationMode::BREATHE: {
      const float phase = (nowMs % period) / static_cast<float>(period);
      const float wave = 0.5f * (sinf(phase * TWO_PI) + 1.0f);
      const float eased = wave * wave * (3.0f - 2.0f * wave);
      return static_cast<uint8_t>(roundf(floor + (ceiling - floor) * eased));
    }
    default:
      return floor;
  }
}

LedPrinterState resolvePrinterState(const PrinterStatus& status, uint32_t nowMs) {
  if (status.hasError) {
    return LedPrinterState::ERROR;
  }
  if (status.gcodeState == "PAUSE") {
    return LedPrinterState::PAUSED;
  }
  if (finishHighlightActive && status.gcodeState == "FINISH") {
    return LedPrinterState::FINISHED;
  }
  if (status.hasProgressData || status.isPrinting) {
    return LedPrinterState::PRINTING;
  }
  if (status.gcodeState == "PREPARE" && !status.hasProgressData) {
    return LedPrinterState::PREPARE;
  }
  if (status.gcodeState == "IDLE") {
    return LedPrinterState::IDLE;
  }
  if (status.gcodeState == "FINISH" && status.lastPrintActiveMs != 0 &&
      (nowMs - status.lastPrintActiveMs) <= FINISH_RECENT_WINDOW_MS) {
    return LedPrinterState::FINISHED;
  }
  return LedPrinterState::UNKNOWN;
}

void resetAnimationState() {
  heartbeatIndex = 0;
  lastHeartbeatMs = 0;
  pausePhase = false;
  lastPauseToggleMs = 0;
  finishHighlightActive = false;
  finishHighlightStartMs = 0;
  wasFinishedState = false;
}

void renderDisconnected(uint32_t nowMs) {
  if (nowMs - lastHeartbeatMs >= 150) {
    heartbeatIndex = (heartbeatIndex + 1) % NUM_LEDS;
    lastHeartbeatMs = nowMs;
  }
  strip.clear();
  strip.setPixelColor(heartbeatIndex, scaledColor(0, 0, 255, BRIGHTNESS_DIM));
}

void renderWifiState() {
  strip.clear();
  const uint32_t color = scaledColor(0, 0, 255, BRIGHTNESS_WIFI);
  for (uint16_t i = 0; i < NUM_LEDS; ++i) {
    strip.setPixelColor(i, color);
  }
}

void renderIdleCool() {
  strip.clear();
  const uint32_t color = scaledColor(180, 200, 255, BRIGHTNESS_DIM);
  for (uint16_t i = 0; i < NUM_LEDS; ++i) {
    strip.setPixelColor(i, color);
  }
}

void renderWholeStripState(const LedStateStyle& style, uint32_t nowMs) {
  strip.clear();
  const uint8_t brightness = styleBrightness(style, nowMs);
  const uint32_t color = scaledColor(style.color, brightness);
  for (uint16_t i = 0; i < NUM_LEDS; ++i) {
    strip.setPixelColor(i, color);
  }
}

void renderPrepareState(const LedStateStyle& style, uint32_t nowMs) {
  if (style.mode == LedAnimationMode::SOLID || style.mode == LedAnimationMode::FLASH) {
    renderWholeStripState(style, nowMs);
    return;
  }

  strip.clear();
  const uint8_t ceiling = capBrightness(g_ledConfig.maxBrightness);
  const uint8_t floor = (style.baselineBrightness > ceiling) ? ceiling : style.baselineBrightness;
  const uint16_t period = (style.speedMs == 0) ? 1 : style.speedMs;
  const float timePhase = (nowMs % period) / static_cast<float>(period);

  for (uint16_t i = 0; i < NUM_LEDS; ++i) {
    const float offset = static_cast<float>(i) / NUM_LEDS;
    const float phase = (timePhase + offset) * TWO_PI;
    float wave = 0.5f * (sinf(phase) + 1.0f);
    if (style.mode == LedAnimationMode::BREATHE) {
      wave = wave * wave * (3.0f - 2.0f * wave);
    }
    const uint8_t brightness = static_cast<uint8_t>(roundf(floor + (ceiling - floor) * wave));
    strip.setPixelColor(i, scaledColor(style.color, brightness));
  }
}

void renderPrintingState(const LedStateStyle& style, float progress, uint32_t nowMs) {
  uint16_t lit = static_cast<uint16_t>(roundf((progress / 100.0f) * NUM_LEDS));
  if (progress > 0.0f && lit == 0) {
    lit = 1;
  }
  if (lit > NUM_LEDS) {
    lit = NUM_LEDS;
  }

  strip.clear();
  const uint8_t brightness = styleBrightness(style, nowMs);
  const uint32_t activeColor = scaledColor(style.color, brightness);
  const uint32_t inactiveColor = scaledColor(20, 20, 20, BRIGHTNESS_DIM);
  for (uint16_t i = 0; i < NUM_LEDS; ++i) {
    strip.setPixelColor(i, (i < lit) ? activeColor : inactiveColor);
  }
}

void renderPausedState(const LedStateStyle& style, uint32_t nowMs) {
  if (style.mode != LedAnimationMode::FLASH) {
    renderWholeStripState(style, nowMs);
    return;
  }

  const uint16_t period = (style.speedMs == 0) ? 1 : style.speedMs;
  if (nowMs - lastPauseToggleMs >= (period / 2U)) {
    pausePhase = !pausePhase;
    lastPauseToggleMs = nowMs;
  }

  strip.clear();
  const uint8_t lowBrightness = (style.baselineBrightness > g_ledConfig.maxBrightness) ? g_ledConfig.maxBrightness
                                                                                        : style.baselineBrightness;
  const uint32_t onColor = scaledColor(style.color, g_ledConfig.maxBrightness);
  const uint32_t offColor = scaledColor(style.color, lowBrightness);
  for (uint16_t i = 0; i < NUM_LEDS; ++i) {
    const bool even = (i % 2U) == 0;
    const bool onPhase = pausePhase ? even : !even;
    strip.setPixelColor(i, onPhase ? onColor : offColor);
  }
}

void renderPrinterState(const PrinterStatus& status, uint32_t nowMs) {
  const LedPrinterState state = resolvePrinterState(status, nowMs);
  const LedStateStyle& style = g_ledConfig.states[stateIndex(state)];
  switch (state) {
    case LedPrinterState::PRINTING:
      renderPrintingState(style, status.progress, nowMs);
      break;
    case LedPrinterState::PAUSED:
      renderPausedState(style, nowMs);
      break;
    case LedPrinterState::PREPARE:
      renderPrepareState(style, nowMs);
      break;
    default:
      renderWholeStripState(style, nowMs);
      break;
  }
}
}  // namespace

void initLeds() {
  strip.begin();
  strip.setBrightness(255);
  if (!loadLedBehaviorConfig(&g_ledConfig, true)) {
    setDefaultLedBehaviorConfig(&g_ledConfig);
  }
  normalizeLedBehaviorConfig(&g_ledConfig);
  resetAnimationState();
  strip.clear();
  strip.show();
}

bool applyLedBehaviorConfig(const LedBehaviorConfig& config) {
  LedBehaviorConfig updated = config;
  normalizeLedBehaviorConfig(&updated);
  const LedConfigValidationResult validation = validateLedBehaviorConfig(updated);
  if (!validation.ok) {
    return false;
  }
  g_ledConfig = updated;
  resetAnimationState();
  return true;
}

void resetLedBehaviorConfigToDefaults() {
  setDefaultLedBehaviorConfig(&g_ledConfig);
  normalizeLedBehaviorConfig(&g_ledConfig);
  resetAnimationState();
}

const LedBehaviorConfig* currentLedBehaviorConfig() { return &g_ledConfig; }

void updateLeds(const PrinterStatus& status, uint32_t nowMs) {
  const bool wifiConnected = status.wifiConnected;
  const bool mqttConnected = status.mqttConnected;
  const bool dataFresh = status.lastUpdateMs != 0 && (nowMs - status.lastUpdateMs) <= STATUS_STALE_TIMEOUT_MS;

  const bool finishedState = status.gcodeState == "FINISH";
  const bool recentPrint = status.lastPrintActiveMs != 0 && (nowMs - status.lastPrintActiveMs) <= FINISH_RECENT_WINDOW_MS;
  if (finishedState && !wasFinishedState) {
    finishHighlightActive = recentPrint;
    finishHighlightStartMs = nowMs;
  }
  if (finishHighlightActive && (nowMs - finishHighlightStartMs) > 10000UL) {
    finishHighlightActive = false;
  }
  wasFinishedState = finishedState;

  if (!wifiConnected) {
    renderDisconnected(nowMs);
  } else if (!mqttConnected) {
    renderWifiState();
  } else if (!dataFresh) {
    renderIdleCool();
  } else {
    renderPrinterState(status, nowMs);
  }

  strip.show();
}
