#pragma once

#include <Arduino.h>
#include <WiFi.h>

class SoftApManager {
 public:
  SoftApManager();

  bool begin(const char* ssid);
  void stop();
  bool isRunning() const;
  const char* ssid() const;
  IPAddress ip() const;

 private:
  bool running_;
  char ssid_[33];
  IPAddress ip_;
};
