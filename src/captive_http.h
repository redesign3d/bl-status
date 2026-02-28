#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include "config.h"

class CaptiveHttp {
 public:
  CaptiveHttp();
  ~CaptiveHttp();

  bool begin(uint16_t port, const char* apSsid, const IPAddress& apIp);
  void loop(uint32_t nowMs);
  void stop();
  bool isRunning() const;

 private:
  struct RateSlot {
    bool used;
    IPAddress ip;
    uint32_t windowStartMs;
    uint8_t count;
  };

  bool allowRequest();
  void sendSecurityHeaders();
  void sendPortalPage();
  void sendHealth();
  void sendRedirect();
  void sendTooManyRequests();
  void handleRoot();
  void handleHealth();
  void handleCaptiveProbe();
  void handleNotFound();

  WebServer* server_;
  bool running_;
  uint32_t nowMs_;
  char apSsid_[33];
  char apIp_[16];
  RateSlot rateSlots_[PROVISIONING_RATE_LIMIT_SLOTS];
};
