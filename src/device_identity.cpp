#include "device_identity.h"

#include <esp_mac.h>
#include <esp_system.h>
#include <stdio.h>

namespace {
String& macLast4Cache() {
  static String value;
  if (value.length() == 0) {
    uint8_t mac[6] = {0};
    char suffix[5];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
      const uint64_t efuseMac = ESP.getEfuseMac();
      mac[4] = static_cast<uint8_t>((efuseMac >> 32) & 0xFFU);
      mac[5] = static_cast<uint8_t>((efuseMac >> 40) & 0xFFU);
    }
    snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
    value = suffix;
  }
  return value;
}

String& hostnameCache() {
  static String value;
  if (value.length() == 0) {
    value = String("bl-status-") + macLast4Cache();
  }
  return value;
}

String& uiTitleCache() {
  static String value;
  if (value.length() == 0) {
    value = String("Bambu Status #") + macLast4Cache();
  }
  return value;
}
}  // namespace

String getMacLast4() { return macLast4Cache(); }

String getHostname() { return hostnameCache(); }

String getUiTitle() { return uiTitleCache(); }
