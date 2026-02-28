#pragma once

#include <Arduino.h>

#include "led_behavior_config.h"

struct LedConfigJsonParseResult {
  bool ok;
  const char* code;
  bool hasState;
  LedPrinterState state;
};

bool parseLedConfigJsonPayload(const String& payload, LedBehaviorConfig* outConfig, LedConfigJsonParseResult* result,
                               String* errorMessage);
String buildLedConfigApiJson(const LedBehaviorConfig& config);
String buildLedConfigActionResponse(bool ok, const char* code, const char* message, bool hasState,
                                    LedPrinterState state);
void appendLedConfigEditorSection(String* body, const char* fetchPath, const char* savePath, const char* resetPath,
                                  const char* csrfToken, const char* networkLabel, const char* networkValue,
                                  const char* locationLabel, const char* locationValue);
bool runLedConfigJsonSelfTest(Stream& out);
