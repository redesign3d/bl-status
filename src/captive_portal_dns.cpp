#include "captive_portal_dns.h"

CaptivePortalDns::CaptivePortalDns() : running_(false) {}

bool CaptivePortalDns::begin(const IPAddress& redirectIp, uint16_t port) {
  stop();
  running_ = server_.start(port, "*", redirectIp);
  if (!running_) {
    Serial.println("DNS captive server start failed");
  }
  return running_;
}

void CaptivePortalDns::loop() {
  if (running_) {
    server_.processNextRequest();
  }
}

void CaptivePortalDns::stop() {
  if (running_) {
    server_.stop();
    running_ = false;
  }
}

bool CaptivePortalDns::isRunning() const { return running_; }
