#pragma once

#include <Arduino.h>
#include <string.h>

#include "config.h"

struct DeviceConfig {
  char wifiSsid[WIFI_SSID_MAX_LEN + 1];
  char wifiPassword[WIFI_PASSWORD_MAX_LEN + 1];
  char printerHost[PRINTER_HOST_MAX_LEN + 1];
  uint16_t printerPort;
  char printerSerial[PRINTER_SERIAL_MAX_LEN + 1];
  char mqttUsername[MQTT_USERNAME_MAX_LEN + 1];
  char accessCode[ACCESS_CODE_MAX_LEN + 1];
  bool tlsInsecure;
};

inline void clearDeviceConfig(DeviceConfig* cfg) {
  if (!cfg) {
    return;
  }
  memset(cfg, 0, sizeof(DeviceConfig));
}
