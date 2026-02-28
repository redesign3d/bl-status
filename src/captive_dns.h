#pragma once

#include <Arduino.h>
#include <DNSServer.h>

class CaptiveDns {
 public:
  CaptiveDns();

  bool begin(const IPAddress& redirectIp, uint16_t port);
  void loop();
  void stop();
  bool isRunning() const;

 private:
  DNSServer server_;
  bool running_;
};
