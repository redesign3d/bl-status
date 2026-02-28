#pragma once

#include <Arduino.h>

#include "RuntimeConfig.h"
#include "captive_dns.h"
#include "captive_http.h"
#include "improv_serial.h"
#include "mdns_manager.h"
#include "softap_manager.h"

enum class ProvisioningState : uint8_t {
  BOOT = 0,
  CHECK_CONFIG,
  NORMAL_OPERATION,
  ENTER_PROVISIONING,
  PROVISIONING_ACTIVE,
  REBOOT_PENDING,
};

class ProvisioningManager : public CaptiveHttpHandler, public ImprovSerialHandler {
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

  bool applySubmittedConfig(const DeviceConfig& config, char* message, size_t messageLen) override;
  bool resetProvisioningConfig(char* message, size_t messageLen) override;
  bool handleImprovWifiSettings(const char* ssid, const char* password, char* url, size_t urlLen, char* message,
                                size_t messageLen) override;

 private:
  bool loadConfigFromNvs();
  bool startProvisioning(uint32_t nowMs);
  void stopProvisioning();
  void scheduleReboot(uint32_t nowMs);
  void buildProvisioningSsid();
  void generateResetToken();
  uint8_t randomByte();
  bool isPrintableAscii(const char* value) const;
  void refreshDraftInPortal();
  void updateRuntimeDiscovery(bool wifiConnected);
  void stopRuntimeDiscovery();

  ProvisioningState state_;
  DeviceConfig activeConfig_;
  bool hasActiveConfig_;
  DeviceConfig draftConfig_;
  bool provisioningCompletePending_;

  char apSsid_[33];
  char resetToken_[17];
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
  ImprovSerial improv_;
  MdnsManager mdns_;
};
