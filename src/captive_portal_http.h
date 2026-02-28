#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include "RuntimeConfig.h"

class CaptivePortalHttp {
 public:
  CaptivePortalHttp();
  ~CaptivePortalHttp();

  bool begin(uint16_t port, const char* pairingCode, uint32_t pairingExpiryMs, const char* resetToken);
  void loop(uint32_t nowMs);
  void stop();
  bool isRunning() const;

  bool consumeProvisionRequest(DeviceConfig* outConfig);
  bool consumeResetRequest();

  void invalidateAuth();

 private:
  struct RateSlot {
    bool used;
    IPAddress ip;
    uint32_t windowStartMs;
    uint8_t count;
  };

  bool allowRequest();
  bool isPairingCodeValid(const String& provided, uint32_t nowMs) const;
  bool hasValidSession(uint32_t nowMs) const;
  bool hasValidSessionFromRequest(uint32_t nowMs) const;
  void createSession(uint32_t nowMs);
  void clearSession();
  String readCookie(const String& name) const;
  bool parseConfigFromRequest(DeviceConfig* outConfig);
  bool hasDisallowedControlChars(const String& value) const;

  void sendSecurityHeaders();
  void sendUnauthorized();
  void sendTooManyRequests();
  void sendGenericFailure();
  void sendAuthPage(const String& message);
  void sendProvisionPage(const String& message);

  void handleRoot();
  void handleProvision();
  void handleReset();
  void handleHealth();
  void handleNotFound();

  WebServer* server_;
  bool running_;
  uint32_t nowMs_;

  char pairingCode_[7];
  uint32_t pairingExpiryMs_;
  char resetToken_[17];
  char sessionId_[33];
  uint32_t sessionExpiryMs_;

  bool pendingProvision_;
  DeviceConfig pendingConfig_;
  bool pendingReset_;
  RateSlot rateSlots_[8];
};
