#include "LedController.h"

#include <Arduino.h>
#include <algorithm>
#include <math.h>

namespace {
constexpr uint8_t BRIGHTNESS_WIFI = 128;   // 50%
constexpr uint8_t BRIGHTNESS_DIM = 51;     // 20%
constexpr uint8_t BRIGHTNESS_FULL = 255;   // 100%
constexpr uint8_t BRIGHTNESS_STEP = 3;
constexpr uint32_t BRIGHTNESS_STEP_INTERVAL_MS = 20;

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

uint8_t heartbeatIndex = 0;
uint32_t lastHeartbeatMs = 0;
bool pausePhase = false;
uint32_t lastPauseToggleMs = 0;
bool finishHighlightActive = false;
uint32_t finishHighlightStartMs = 0;
bool wasFinishedState = false;
uint8_t currentBrightness = BRIGHTNESS_DIM;
uint8_t targetBrightness = BRIGHTNESS_DIM;
uint32_t lastBrightnessUpdateMs = 0;
}

static uint32_t scaledColor(uint8_t r, uint8_t g, uint8_t b, float scale) {
  if (scale < 0.0f) scale = 0.0f;
  if (scale > 1.0f) scale = 1.0f;
  return strip.Color(static_cast<uint8_t>(r * scale), static_cast<uint8_t>(g * scale),
                     static_cast<uint8_t>(b * scale));
}

static uint8_t clampByte(int value) {
  if (value < 0) return 0;
  if (value > 255) return 255;
  return static_cast<uint8_t>(value);
}

static void requestBrightness(uint8_t target) { targetBrightness = target; }

static void applyBrightnessRamp(uint32_t nowMs) {
  if (currentBrightness == targetBrightness) {
    return;
  }
  if (nowMs - lastBrightnessUpdateMs < BRIGHTNESS_STEP_INTERVAL_MS) {
    return;
  }
  lastBrightnessUpdateMs = nowMs;
  int diff = static_cast<int>(targetBrightness) - static_cast<int>(currentBrightness);
  int step = (diff > 0) ? BRIGHTNESS_STEP : -BRIGHTNESS_STEP;
  if (abs(diff) < BRIGHTNESS_STEP) {
    step = diff;
  }
  currentBrightness = static_cast<uint8_t>(static_cast<int>(currentBrightness) + step);
  strip.setBrightness(currentBrightness);
}

void initLeds() {
  strip.begin();
  strip.setBrightness(BRIGHTNESS_DIM);
  currentBrightness = BRIGHTNESS_DIM;
  targetBrightness = BRIGHTNESS_DIM;
  strip.clear();
  strip.show();
}

static void renderDisconnected(uint32_t nowMs) {
  if (nowMs - lastHeartbeatMs >= 150) {
    heartbeatIndex = (heartbeatIndex + 1) % NUM_LEDS;
    lastHeartbeatMs = nowMs;
  }
  strip.clear();
  strip.setPixelColor(heartbeatIndex, strip.Color(0, 0, 40));
}

static void renderWifiState() {
  strip.clear();
  uint32_t color = strip.Color(0, 0, 80);
  for (uint16_t i = 0; i < NUM_LEDS; ++i) {
    strip.setPixelColor(i, color);
  }
}

static void renderIdleCool() {
  const uint32_t color = strip.Color(180, 200, 255);
  strip.clear();
  for (uint16_t i = 0; i < NUM_LEDS; ++i) {
    strip.setPixelColor(i, color);
  }
}

static void renderPrepare(uint32_t nowMs) {
  constexpr uint32_t sweepPeriod = 3000;
  float timePhase = (nowMs % sweepPeriod) / static_cast<float>(sweepPeriod);
  strip.clear();
  for (uint16_t i = 0; i < NUM_LEDS; ++i) {
    float offset = static_cast<float>(i) / NUM_LEDS;
    float phase = timePhase + offset;
    float wave = 0.4f + 0.6f * (0.5f * (sinf(phase * TWO_PI) + 1.0f));
    strip.setPixelColor(i, strip.Color(clampByte(255 * wave), clampByte(180 * wave), clampByte(0)));
  }
}

static void renderPrintingBar(float progress) {
  uint16_t lit = static_cast<uint16_t>(roundf((progress / 100.0f) * NUM_LEDS));
  if (progress > 0.0f && lit == 0) {
    lit = 1;
  }
  if (lit > NUM_LEDS) {
    lit = NUM_LEDS;
  }
  strip.clear();
  for (uint16_t i = 0; i < NUM_LEDS; ++i) {
    if (i < lit) {
      strip.setPixelColor(i, strip.Color(255, 255, 255));
    } else {
      strip.setPixelColor(i, strip.Color(20, 20, 20));
    }
  }
}

static void renderPaused(uint32_t nowMs) {
  if (nowMs - lastPauseToggleMs >= 500) {
    pausePhase = !pausePhase;
    lastPauseToggleMs = nowMs;
  }
  strip.clear();
  for (uint16_t i = 0; i < NUM_LEDS; ++i) {
    bool even = (i % 2) == 0;
    bool onPhase = pausePhase ? even : !even;
    if (onPhase) {
      strip.setPixelColor(i, strip.Color(255, 150, 0));
    } else {
      strip.setPixelColor(i, 0);
    }
  }
}

static void renderError(uint32_t nowMs) {
  constexpr uint32_t period = 2000;  // ~0.5 Hz pulse
  float phase = (nowMs % period) / static_cast<float>(period);
  float triangle = phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f;
  float brightness = 0.15f + 0.65f * triangle;
  strip.clear();
  for (uint16_t i = 0; i < NUM_LEDS; ++i) {
    strip.setPixelColor(i, strip.Color(clampByte(255 * brightness), 0, 0));
  }
}

static void renderFinishedGlow() {
  strip.clear();
  for (uint16_t i = 0; i < NUM_LEDS; ++i) {
    strip.setPixelColor(i, strip.Color(0, 255, 0));
  }
}

void updateLeds(const PrinterStatus& status, uint32_t nowMs) {
  bool wifiConnected = status.wifiConnected;
  bool mqttConnected = status.mqttConnected;
  bool dataFresh = status.lastUpdateMs != 0 && (nowMs - status.lastUpdateMs) <= STATUS_STALE_TIMEOUT_MS;

  bool finishedState = status.gcodeState == "FINISH";
  bool recentPrint =
      status.lastPrintActiveMs != 0 && (nowMs - status.lastPrintActiveMs) <= FINISH_RECENT_WINDOW_MS;
  if (finishedState && !wasFinishedState) {
    finishHighlightActive = recentPrint;
    finishHighlightStartMs = nowMs;
  }
  if (finishHighlightActive && (nowMs - finishHighlightStartMs) > 10000UL) {
    finishHighlightActive = false;
  }
  wasFinishedState = finishedState;

  if (!wifiConnected) {
    requestBrightness(BRIGHTNESS_DIM);
    renderDisconnected(nowMs);
  } else if (!mqttConnected) {
    requestBrightness(BRIGHTNESS_WIFI);
    renderWifiState();
  } else if (!dataFresh) {
    requestBrightness(BRIGHTNESS_DIM);
    renderIdleCool();
  } else if (status.hasError) {
    requestBrightness(BRIGHTNESS_FULL);
    renderError(nowMs);
  } else if (status.gcodeState == "PAUSE") {
    requestBrightness(BRIGHTNESS_FULL);
    renderPaused(nowMs);
  } else if (finishHighlightActive) {
    requestBrightness(BRIGHTNESS_FULL);
    renderFinishedGlow();
  } else {
    uint8_t targetBrightness = status.printerLightOn ? BRIGHTNESS_FULL : BRIGHTNESS_DIM;
    bool inPrinting = status.hasProgressData || status.isPrinting;
    bool inPrepare = (status.gcodeState == "PREPARE") && !status.hasProgressData;
    if (inPrinting) {
      requestBrightness(status.printerLightOn ? BRIGHTNESS_FULL : BRIGHTNESS_DIM);
      renderPrintingBar(status.progress);
    } else if (inPrepare) {
      requestBrightness(targetBrightness);
      renderPrepare(nowMs);
    } else {
      requestBrightness(targetBrightness);
      renderIdleCool();
    }
  }

  applyBrightnessRamp(nowMs);
  strip.show();
}
