#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include "RuntimeConfig.h"
#include "config.h"

class CaptiveHttpHandler {
 public:
  virtual ~CaptiveHttpHandler() = default;
  virtual bool applySubmittedConfig(const DeviceConfig& config, char* message, size_t messageLen) = 0;
  virtual bool resetProvisioningConfig(char* message, size_t messageLen) = 0;
};

class CaptiveHttp {
 public:
  CaptiveHttp();
  ~CaptiveHttp();

  bool begin(uint16_t port, const char* apSsid, const IPAddress& apIp, const char* resetToken, CaptiveHttpHandler* handler);
  void loop(uint32_t nowMs);
  void stop();
  bool isRunning() const;
  void setDraftConfig(const DeviceConfig* draftConfig);

 private:
  struct RateSlot {
    bool used;
    IPAddress ip;
    uint32_t windowStartMs;
    uint8_t count;
  };

  bool allowRequest();
  bool parseConfigFromRequest(DeviceConfig* outConfig, String* errorMessage);
  bool isAllowedField(const String& name) const;
  bool hasDisallowedControlChars(const String& value) const;
  bool isValidHostValue(const char* host) const;
  bool containsOnlyTokenChars(const char* value) const;
  bool parseBooleanArg(const String& value, bool* outValue) const;
  void appendInvalidField(String* errorMessage, bool* firstField, const __FlashStringHelper* fieldLabel) const;
  String htmlEscape(const char* value) const;

  void sendSecurityHeaders();
  void sendPortalPage(const String& message, bool isError);
  void sendResultPage(int code, const String& title, const String& message);
  void sendHealth();
  void sendRedirect();
  void sendTooManyRequests();
  void sendPayloadTooLarge();
  void handleRoot();
  void handleHealth();
  void handleProvision();
  void handleReset();
  void handleCaptiveProbe();
  void handleNotFound();

  WebServer* server_;
  CaptiveHttpHandler* handler_;
  bool running_;
  uint32_t nowMs_;
  char apSsid_[33];
  char apIp_[16];
  char resetToken_[17];
  DeviceConfig draftConfig_;
  bool hasDraftConfig_;
  RateSlot rateSlots_[PROVISIONING_RATE_LIMIT_SLOTS];
};
