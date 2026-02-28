#pragma once

#include <Arduino.h>

inline String redactForLog(const char* value) {
  if (!value) {
    return "<null>";
  }
  size_t len = strlen(value);
  if (len == 0) {
    return "<empty>";
  }
  if (len <= 4) {
    return "****";
  }
  return String(value[0]) + "***" + String(value[len - 1]);
}
