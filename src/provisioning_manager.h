#pragma once

#include <Arduino.h>

#include "RuntimeConfig.h"
#include "admin_auth_store.h"
#include "captive_dns.h"
#include "captive_http.h"
#include "improv_serial.h"
#include "local_config_portal.h"
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

class ProvisioningManager : public CaptiveHttpHandler, public ImprovSerialHandler, public LocalConfigPortalHandler {
 public:
  ProvisioningManager();

  void begin();
  void loop(uint32_t nowMs);

  bool isNormalOperation() const;
  bool isProvisioningActive() const;
  bool isRebootPending() const;
  ProvisioningState state() const;

  const DeviceConfig* activeConfig() const;
  const char* provisioningSsid() const;
  IPAddress provisioningIp() const;
  bool shouldShowAdminPassword(uint32_t nowMs) const;
  const char* adminUsernameForDisplay() const;
  const char* adminPasswordForDisplay() const;

  void notifyConnectivity(bool wifiConnected, uint32_t nowMs);
  void requestFactoryReset();

  bool applySubmittedConfig(const DeviceConfig& config, char* message, size_t messageLen) override;
  bool resetProvisioningConfig(char* message, size_t messageLen) override;
  bool handleImprovWifiSettings(const char* ssid, const char* password, char* url, size_t urlLen, char* message,
                                size_t messageLen) override;
  bool saveRuntimeConfig(const DeviceConfig& config, char* message, size_t messageLen) override;
  bool requestRuntimeReboot(char* message, size_t messageLen) override;
  bool requestFactoryResetAndReboot(char* message, size_t messageLen) override;

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
  bool ensureLocalAdminCredentials(AdminCredentials* credentials, bool* generated);
  void setPendingAdminCredentials(const AdminCredentials& credentials, uint32_t nowMs);
  void clearPendingAdminCredentials();
  void announceGeneratedAdminCredentials(const AdminCredentials& credentials, const char* reason);
  void updateRuntimeServices(bool wifiConnected);
  void stopRuntimeServices();

  ProvisioningState state_;
  DeviceConfig activeConfig_;
  bool hasActiveConfig_;
  DeviceConfig draftConfig_;
  bool provisioningCompletePending_;

  char apSsid_[33];
  char resetToken_[17];
  char pendingAdminUser_[ADMIN_USERNAME_MAX_LEN + 1];
  char pendingAdminPass_[ADMIN_PASSWORD_MAX_LEN + 1];
  IPAddress apIp_;

  uint32_t provisioningDeadlineMs_;
  uint32_t rebootAtMs_;
  uint32_t nextProvisionRetryMs_;
  uint32_t adminAnnouncementUntilMs_;

  uint32_t lastWifiFailureCheckMs_;
  uint16_t wifiFailureCount_;
  uint16_t wifiFailureThreshold_;

  SoftApManager softAp_;
  CaptiveDns dns_;
  CaptiveHttp http_;
  ImprovSerial improv_;
  LocalConfigPortal localPortal_;
  MdnsManager mdns_;
};
