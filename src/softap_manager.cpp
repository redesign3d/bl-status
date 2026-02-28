#include "softap_manager.h"

#include <string.h>

#include "config.h"

namespace {
constexpr uint8_t kApIpOctets[4] = {192, 168, 4, 1};
constexpr uint8_t kApNetmaskOctets[4] = {255, 255, 255, 0};
}  // namespace

SoftApManager::SoftApManager() : running_(false), ip_(kApIpOctets[0], kApIpOctets[1], kApIpOctets[2], kApIpOctets[3]) {
  memset(ssid_, 0, sizeof(ssid_));
}

bool SoftApManager::begin(const char* ssid) {
  stop();
  if (!ssid || ssid[0] == '\0') {
    return false;
  }

  strlcpy(ssid_, ssid, sizeof(ssid_));

  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  delay(50);
  WiFi.mode(WIFI_AP_STA);

  IPAddress gateway(kApIpOctets[0], kApIpOctets[1], kApIpOctets[2], kApIpOctets[3]);
  IPAddress netmask(kApNetmaskOctets[0], kApNetmaskOctets[1], kApNetmaskOctets[2], kApNetmaskOctets[3]);
  if (!WiFi.softAPConfig(ip_, gateway, netmask)) {
    Serial.println("SoftAP config failed");
    stop();
    return false;
  }

  if (!WiFi.softAP(ssid_, nullptr, 1, false, PROVISIONING_SOFTAP_MAX_CLIENTS)) {
    Serial.println("SoftAP start failed");
    stop();
    return false;
  }

  running_ = true;
  return true;
}

void SoftApManager::stop() {
  if (running_) {
    WiFi.softAPdisconnect(true);
    running_ = false;
  }
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_STA);
  memset(ssid_, 0, sizeof(ssid_));
}

bool SoftApManager::isRunning() const { return running_; }

const char* SoftApManager::ssid() const { return ssid_; }

IPAddress SoftApManager::ip() const { return ip_; }
