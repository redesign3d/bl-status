#pragma once

#include <Arduino.h>

#include "RuntimeConfig.h"
#include "captive_dns.h"
#include "captive_http.h"
#include "softap_manager.h"

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
  IPAddress provisioningIp() const;

  void notifyConnectivity(bool wifiConnected, uint32_t nowMs);
  void requestFactoryReset();

 private:
  bool loadConfigFromNvs();
  bool startProvisioning(uint32_t nowMs);
  void stopProvisioning();
  void scheduleReboot(uint32_t nowMs);
  void buildProvisioningSsid();

  ProvisioningState state_;
  DeviceConfig activeConfig_;
  bool hasActiveConfig_;

  char apSsid_[33];
  IPAddress apIp_;

  uint32_t provisioningDeadlineMs_;
  uint32_t rebootAtMs_;
  uint32_t nextProvisionRetryMs_;

  uint32_t lastWifiFailureCheckMs_;
  uint16_t wifiFailureCount_;
  uint16_t wifiFailureThreshold_;

  SoftApManager softAp_;
  CaptiveDns dns_;
  CaptiveHttp http_;
};
