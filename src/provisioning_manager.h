#pragma once

#include <Arduino.h>

#include "RuntimeConfig.h"
#include "captive_portal_dns.h"
#include "captive_portal_http.h"

enum class ProvisioningState : uint8_t {
  BOOT = 0,
  CHECK_CONFIG,
  NORMAL_OPERATION,
  ENTER_PROVISIONING,
  PROVISIONING_ACTIVE,
  REBOOT_PENDING,
};

class ProvisioningManager {
 public:
  ProvisioningManager();

  void begin();
  void loop(uint32_t nowMs);

  bool isNormalOperation() const;
  bool isProvisioningActive() const;
  ProvisioningState state() const;

  const DeviceConfig* activeConfig() const;
  const char* provisioningSsid() const;

  void notifyConnectivity(bool wifiConnected, uint32_t nowMs);
  void requestFactoryReset();

 private:
  bool loadConfigFromNvs();
  bool startProvisioning(uint32_t nowMs);
  void stopProvisioning();
  void scheduleReboot(uint32_t nowMs);

  void buildProvisioningSsid();
  void generateApPassphrase();
  void generatePairingCode();
  void generateResetToken();
  uint8_t randomByte();

  ProvisioningState state_;
  DeviceConfig activeConfig_;
  bool hasActiveConfig_;

  char apSsid_[32];
  char apPassphrase_[17];
  char pairingCode_[7];
  char resetToken_[17];

  uint32_t provisioningDeadlineMs_;
  uint32_t rebootAtMs_;
  uint32_t nextProvisionRetryMs_;

  uint32_t lastWifiFailureCheckMs_;
  uint16_t wifiFailureCount_;
  uint16_t wifiFailureThreshold_;

  CaptivePortalDns dns_;
  CaptivePortalHttp http_;
};
