#pragma once

#include <Arduino.h>
#include <DNSServer.h>

class CaptivePortalDns {
 public:
  CaptivePortalDns();

  bool begin(const IPAddress& redirectIp, uint16_t port);
  void loop();
  void stop();
  bool isRunning() const;

 private:
  DNSServer server_;
  bool running_;
};
