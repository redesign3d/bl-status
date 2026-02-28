#include "mdns_manager.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <string.h>

MdnsManager::MdnsManager() : running_(false), port_(0) { memset(hostname_, 0, sizeof(hostname_)); }

bool MdnsManager::beginMdns(const char* hostname, uint16_t port) {
  endMdns();
  if (!hostname || hostname[0] == '\0' || port == 0 || !isWifiReady()) {
    return false;
  }

  if (!MDNS.begin(hostname)) {
    Serial.println("mDNS start failed");
    return false;
  }

  MDNS.addService("http", "tcp", port);
  MDNS.addServiceTxt("http", "tcp", "path", "/");

  running_ = true;
  port_ = port;
  strlcpy(hostname_, hostname, sizeof(hostname_));
  Serial.printf("mDNS started: http://%s.local/\n", hostname_);
  return true;
}

void MdnsManager::endMdns() {
  if (running_) {
    MDNS.end();
    running_ = false;
  }
  port_ = 0;
  memset(hostname_, 0, sizeof(hostname_));
}

void MdnsManager::updateMdns(bool shouldRun, const char* hostname, uint16_t port) {
  if (!shouldRun || !isWifiReady()) {
    endMdns();
    return;
  }

  if (running_ && hostname && strcmp(hostname_, hostname) == 0 && port_ == port) {
    return;
  }

  beginMdns(hostname, port);
}

bool MdnsManager::isRunning() const { return running_; }

bool MdnsManager::isWifiReady() const {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  IPAddress ip = WiFi.localIP();
  return ip[0] != 0;
}
