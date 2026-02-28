#include "device_identity.h"

#include <esp_system.h>
#include <stdio.h>

namespace {
String& macLast4Cache() {
  static String value;
  if (value.length() == 0) {
    char suffix[5];
    const uint16_t macSuffix = static_cast<uint16_t>(ESP.getEfuseMac() & 0xFFFFULL);
    snprintf(suffix, sizeof(suffix), "%04X", macSuffix);
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
