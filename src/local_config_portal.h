#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include "RuntimeConfig.h"
#include "config.h"

class LocalConfigPortalHandler {
 public:
  virtual ~LocalConfigPortalHandler() = default;
  virtual const DeviceConfig* activeConfig() const = 0;
  virtual bool saveRuntimeConfig(const DeviceConfig& config, char* message, size_t messageLen) = 0;
  virtual bool requestRuntimeReboot(char* message, size_t messageLen) = 0;
  virtual bool requestFactoryResetAndReboot(char* message, size_t messageLen) = 0;
};

class LocalConfigPortal {
 public:
  LocalConfigPortal();
  ~LocalConfigPortal();

  bool begin(uint16_t port, LocalConfigPortalHandler* handler);
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
  bool ensureAuthenticated();
  bool validateCsrf() const;
  bool parseConfigUpdate(const DeviceConfig& currentConfig, DeviceConfig* outConfig, String* errorMessage) const;
  bool isAllowedConfigField(const String& name) const;
  bool hasDisallowedControlChars(const String& value) const;
  bool containsOnlyTokenChars(const char* value) const;
  bool isValidHostValue(const char* host) const;
  bool parseBooleanArg(const String& value, bool* outValue) const;
  bool deriveAuthPassword(const DeviceConfig& config, char* password, size_t passwordLen) const;
  void appendInvalidField(String* errorMessage, bool* firstField, const __FlashStringHelper* fieldLabel) const;
  String htmlEscape(const char* value) const;
  void generateCsrfToken();

  void sendSecurityHeaders();
  void sendLandingPage(const String& message, bool isError);
  void sendConfigPage(const String& message, bool isError);
  void sendResultPage(int code, const String& title, const String& message);
  void sendHealth();
  void sendTooManyRequests();

  void handleRoot();
  void handleConfigGet();
  void handleConfigPost();
  void handleReboot();
  void handleReset();
  void handleHealth();
  void handleNotFound();

  WebServer* server_;
  LocalConfigPortalHandler* handler_;
  bool running_;
  uint32_t nowMs_;
  char authUser_[16];
  char authPass_[LOCAL_PORTAL_PASSWORD_SUFFIX_LEN + 1];
  char csrfToken_[LOCAL_PORTAL_CSRF_TOKEN_LEN + 1];
  RateSlot rateSlots_[PROVISIONING_RATE_LIMIT_SLOTS];
};
