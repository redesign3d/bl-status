#include "captive_dns.h"

CaptiveDns::CaptiveDns() : running_(false) {}

bool CaptiveDns::begin(const IPAddress& redirectIp, uint16_t port) {
  stop();
  server_.setErrorReplyCode(DNSReplyCode::NoError);
  running_ = server_.start(port, "*", redirectIp);
  if (!running_) {
    Serial.println("DNS captive server start failed");
  }
  return running_;
}

void CaptiveDns::loop() {
  if (running_) {
    server_.processNextRequest();
  }
}

void CaptiveDns::stop() {
  if (running_) {
    server_.stop();
    running_ = false;
  }
}

bool CaptiveDns::isRunning() const { return running_; }
