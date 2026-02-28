#pragma once

#include <Arduino.h>

class MdnsManager {
 public:
  MdnsManager();

  bool beginMdns(const char* hostname, uint16_t port);
  void endMdns();
  void updateMdns(bool shouldRun, const char* hostname, uint16_t port);
  bool isRunning() const;

 private:
  bool isWifiReady() const;

  bool running_;
  uint16_t port_;
  char hostname_[33];
};
