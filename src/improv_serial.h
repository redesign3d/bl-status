#pragma once

#include <Arduino.h>

#include "config.h"

class ImprovSerialHandler {
 public:
  virtual ~ImprovSerialHandler() = default;
  virtual bool handleImprovWifiSettings(const char* ssid, const char* password, char* url, size_t urlLen,
                                        char* message, size_t messageLen) = 0;
};

class ImprovSerial {
 public:
  ImprovSerial();

  void begin(Stream& serial, ImprovSerialHandler* handler);
  void stop();
  void loop(uint32_t nowMs);
  bool isRunning() const;

 private:
  enum class SerialType : uint8_t {
    kCurrentState = 0x01,
    kErrorState = 0x02,
    kRpc = 0x03,
    kRpcResponse = 0x04,
  };

  enum class Error : uint8_t {
    kNone = 0x00,
    kInvalidRpc = 0x01,
    kUnknownRpc = 0x02,
    kUnableToConnect = 0x03,
    kNotAuthorized = 0x04,
    kUnknown = 0xFF,
  };

  enum class State : uint8_t {
    kStopped = 0x00,
    kAwaitingAuthorization = 0x01,
    kAuthorized = 0x02,
    kProvisioning = 0x03,
    kProvisioned = 0x04,
  };

  enum class Command : uint8_t {
    kWifiSettings = 0x01,
    kGetCurrentState = 0x02,
    kGetDeviceInfo = 0x03,
    kGetWifiNetworks = 0x04,
  };

  struct RpcFields {
    const char* fields[4];
    size_t count;
  };

  bool acceptByte(size_t position, uint8_t byte) const;
  void handleFrame(const uint8_t* payload, size_t length);
  void handleRpc(const uint8_t* payload, size_t length);
  bool parseWifiSettings(const uint8_t* payload, size_t length, char* ssid, size_t ssidLen, char* password,
                         size_t passwordLen);
  void sendState(State state);
  void sendError(Error error);
  void sendRpcResponse(Command command, const RpcFields& fields);
  void sendWifiNetworks();
  void sendCurrentState();
  void announceStateIfNeeded(uint32_t nowMs);
  void buildDeviceName(char* out, size_t outLen) const;
  void buildDeviceInfoFields(char* firmwareVersion, size_t firmwareVersionLen, char* chipFamily,
                             size_t chipFamilyLen, char* deviceName, size_t deviceNameLen) const;

  Stream* serial_;
  ImprovSerialHandler* handler_;
  bool running_;
  State state_;
  uint8_t buffer_[IMPROV_SERIAL_MAX_PACKET_BYTES];
  size_t position_;
  uint32_t lastAnnouncementMs_;
};
