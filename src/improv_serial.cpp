#include "improv_serial.h"

#include <WiFi.h>
#include <esp_system.h>
#include <string.h>

namespace {
constexpr char kHeader[] = "IMPROV";
constexpr uint8_t kProtocolVersion = 0x01;
constexpr uint32_t kAnnouncementIntervalMs = 5000UL;
constexpr size_t kMaxResponseFields = 4;

uint8_t checksumFor(const uint8_t* bytes, size_t length) {
  uint8_t checksum = 0;
  for (size_t i = 0; i < length; ++i) {
    checksum = static_cast<uint8_t>(checksum + bytes[i]);
  }
  return checksum;
}
}  // namespace

ImprovSerial::ImprovSerial()
    : serial_(nullptr), handler_(nullptr), running_(false), state_(State::kStopped), position_(0), lastAnnouncementMs_(0) {
  memset(buffer_, 0, sizeof(buffer_));
}

void ImprovSerial::begin(Stream& serial, ImprovSerialHandler* handler) {
  stop();
  serial_ = &serial;
  handler_ = handler;
  running_ = (handler_ != nullptr);
  state_ = running_ ? State::kAuthorized : State::kStopped;
  position_ = 0;
  lastAnnouncementMs_ = 0;
  if (running_) {
    sendCurrentState();
  }
}

void ImprovSerial::stop() {
  running_ = false;
  serial_ = nullptr;
  handler_ = nullptr;
  state_ = State::kStopped;
  position_ = 0;
  lastAnnouncementMs_ = 0;
  memset(buffer_, 0, sizeof(buffer_));
}

bool ImprovSerial::isRunning() const { return running_; }

void ImprovSerial::loop(uint32_t nowMs) {
  if (!running_ || !serial_) {
    return;
  }

  announceStateIfNeeded(nowMs);

  while (serial_->available() > 0) {
    const uint8_t byte = static_cast<uint8_t>(serial_->read());
    if (!acceptByte(position_, byte)) {
      position_ = 0;
      continue;
    }

    if (position_ >= sizeof(buffer_)) {
      position_ = 0;
      sendError(Error::kInvalidRpc);
      continue;
    }

    buffer_[position_++] = byte;
    if (position_ < 9) {
      continue;
    }

    const size_t expectedLength = static_cast<size_t>(10U + buffer_[8]);
    if (expectedLength > sizeof(buffer_)) {
      position_ = 0;
      sendError(Error::kInvalidRpc);
      continue;
    }

    if (position_ < expectedLength) {
      continue;
    }

    const uint8_t expectedChecksum = checksumFor(buffer_, expectedLength - 1);
    if (expectedChecksum != buffer_[expectedLength - 1]) {
      position_ = 0;
      sendError(Error::kInvalidRpc);
      continue;
    }

    if (buffer_[7] != static_cast<uint8_t>(SerialType::kRpc)) {
      position_ = 0;
      sendError(Error::kInvalidRpc);
      continue;
    }

    handleFrame(&buffer_[9], buffer_[8]);
    position_ = 0;
  }
}

bool ImprovSerial::acceptByte(size_t position, uint8_t byte) const {
  if (position < 6) {
    return byte == static_cast<uint8_t>(kHeader[position]);
  }
  if (position == 6) {
    return byte == kProtocolVersion;
  }
  if (position == 7) {
    return byte == static_cast<uint8_t>(SerialType::kRpc);
  }
  if (position == 8) {
    return byte <= (IMPROV_SERIAL_MAX_PACKET_BYTES - 10);
  }
  return true;
}

void ImprovSerial::handleFrame(const uint8_t* payload, size_t length) {
  if (!payload || length < 2) {
    sendError(Error::kInvalidRpc);
    return;
  }
  handleRpc(payload, length);
}

void ImprovSerial::handleRpc(const uint8_t* payload, size_t length) {
  if (!payload || length < 2) {
    sendError(Error::kInvalidRpc);
    return;
  }

  const Command command = static_cast<Command>(payload[0]);
  const uint8_t rpcLength = payload[1];
  if (static_cast<size_t>(rpcLength) != (length - 2)) {
    sendError(Error::kInvalidRpc);
    return;
  }

  switch (command) {
    case Command::kWifiSettings: {
      char ssid[WIFI_SSID_MAX_LEN + 1];
      char password[WIFI_PASSWORD_MAX_LEN + 1];
      memset(ssid, 0, sizeof(ssid));
      memset(password, 0, sizeof(password));
      if (!parseWifiSettings(payload, length, ssid, sizeof(ssid), password, sizeof(password))) {
        memset(ssid, 0, sizeof(ssid));
        memset(password, 0, sizeof(password));
        sendError(Error::kInvalidRpc);
        return;
      }

      state_ = State::kProvisioning;
      sendCurrentState();

      char url[80];
      char message[96];
      memset(url, 0, sizeof(url));
      memset(message, 0, sizeof(message));
      const bool ok = handler_ && handler_->handleImprovWifiSettings(ssid, password, url, sizeof(url), message, sizeof(message));
      memset(ssid, 0, sizeof(ssid));
      memset(password, 0, sizeof(password));
      if (!ok) {
        state_ = State::kAuthorized;
        sendError(Error::kUnableToConnect);
        sendCurrentState();
        return;
      }

      state_ = State::kProvisioned;
      sendError(Error::kNone);
      sendCurrentState();
      RpcFields fields{};
      fields.fields[0] = url[0] != '\0' ? url : "http://192.168.4.1/";
      fields.count = 1;
      sendRpcResponse(Command::kWifiSettings, fields);
      break;
    }

    case Command::kGetCurrentState:
      sendCurrentState();
      break;

    case Command::kGetDeviceInfo: {
      char firmwareVersion[16];
      char chipFamily[16];
      char deviceName[24];
      buildDeviceInfoFields(firmwareVersion, sizeof(firmwareVersion), chipFamily, sizeof(chipFamily), deviceName,
                            sizeof(deviceName));
      RpcFields fields{};
      fields.fields[0] = PRODUCT_NAME;
      fields.fields[1] = firmwareVersion;
      fields.fields[2] = chipFamily;
      fields.fields[3] = deviceName;
      fields.count = 4;
      sendRpcResponse(Command::kGetDeviceInfo, fields);
      break;
    }

    case Command::kGetWifiNetworks:
      sendWifiNetworks();
      break;

    default:
      sendError(Error::kUnknownRpc);
      break;
  }
}

bool ImprovSerial::parseWifiSettings(const uint8_t* payload, size_t length, char* ssid, size_t ssidLen, char* password,
                                     size_t passwordLen) {
  if (!payload || !ssid || !password || length < 4) {
    return false;
  }

  size_t index = 2;
  const uint8_t ssidLength = payload[index++];
  if (ssidLength == 0 || ssidLength >= ssidLen || (index + ssidLength) > length) {
    return false;
  }
  memcpy(ssid, payload + index, ssidLength);
  ssid[ssidLength] = '\0';
  index += ssidLength;

  if (index >= length) {
    return false;
  }

  const uint8_t passwordLength = payload[index++];
  if (passwordLength >= passwordLen || (index + passwordLength) > length) {
    memset(ssid, 0, ssidLen);
    return false;
  }
  if (passwordLength > 0) {
    memcpy(password, payload + index, passwordLength);
  }
  password[passwordLength] = '\0';
  index += passwordLength;

  if (index != length) {
    memset(ssid, 0, ssidLen);
    memset(password, 0, passwordLen);
    return false;
  }
  return true;
}

void ImprovSerial::sendState(State state) {
  if (!serial_) {
    return;
  }
  uint8_t frame[11];
  memcpy(frame, kHeader, 6);
  frame[6] = kProtocolVersion;
  frame[7] = static_cast<uint8_t>(SerialType::kCurrentState);
  frame[8] = 1;
  frame[9] = static_cast<uint8_t>(state);
  frame[10] = checksumFor(frame, 10);
  serial_->write(frame, sizeof(frame));
}

void ImprovSerial::sendError(Error error) {
  if (!serial_) {
    return;
  }
  uint8_t frame[11];
  memcpy(frame, kHeader, 6);
  frame[6] = kProtocolVersion;
  frame[7] = static_cast<uint8_t>(SerialType::kErrorState);
  frame[8] = 1;
  frame[9] = static_cast<uint8_t>(error);
  frame[10] = checksumFor(frame, 10);
  serial_->write(frame, sizeof(frame));
}

void ImprovSerial::sendRpcResponse(Command command, const RpcFields& fields) {
  if (!serial_ || fields.count > kMaxResponseFields) {
    return;
  }

  uint8_t rpcPayload[IMPROV_SERIAL_MAX_PACKET_BYTES];
  size_t rpcLength = 0;
  rpcPayload[rpcLength++] = static_cast<uint8_t>(command);
  rpcPayload[rpcLength++] = 0;

  size_t combinedFieldLength = 0;
  for (size_t i = 0; i < fields.count; ++i) {
    const char* field = fields.fields[i] ? fields.fields[i] : "";
    const size_t fieldLen = strlen(field);
    if (fieldLen > 255 || (rpcLength + 1 + fieldLen) >= sizeof(rpcPayload)) {
      return;
    }
    rpcPayload[rpcLength++] = static_cast<uint8_t>(fieldLen);
    memcpy(rpcPayload + rpcLength, field, fieldLen);
    rpcLength += fieldLen;
    combinedFieldLength += (1 + fieldLen);
  }
  rpcPayload[1] = static_cast<uint8_t>(combinedFieldLength);

  uint8_t frame[IMPROV_SERIAL_MAX_PACKET_BYTES];
  size_t frameLength = 0;
  memcpy(frame + frameLength, kHeader, 6);
  frameLength += 6;
  frame[frameLength++] = kProtocolVersion;
  frame[frameLength++] = static_cast<uint8_t>(SerialType::kRpcResponse);
  frame[frameLength++] = static_cast<uint8_t>(rpcLength);
  memcpy(frame + frameLength, rpcPayload, rpcLength);
  frameLength += rpcLength;
  frame[frameLength++] = checksumFor(frame, frameLength);
  serial_->write(frame, frameLength);
}

void ImprovSerial::sendWifiNetworks() {
  if (!serial_) {
    return;
  }

  const int networkCount = WiFi.scanNetworks();
  for (int i = 0; i < networkCount; ++i) {
    char rssi[12];
    snprintf(rssi, sizeof(rssi), "%d", WiFi.RSSI(i));
    const char* secure = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "NO" : "YES";
    String ssid = WiFi.SSID(i);
    RpcFields fields{};
    fields.fields[0] = ssid.c_str();
    fields.fields[1] = rssi;
    fields.fields[2] = secure;
    fields.count = 3;
    sendRpcResponse(Command::kGetWifiNetworks, fields);
    delay(1);
  }
  WiFi.scanDelete();

  RpcFields finalFields{};
  finalFields.count = 0;
  sendRpcResponse(Command::kGetWifiNetworks, finalFields);
}

void ImprovSerial::sendCurrentState() {
  sendState(state_);
}

void ImprovSerial::announceStateIfNeeded(uint32_t nowMs) {
  if (!running_) {
    return;
  }
  if (lastAnnouncementMs_ == 0 || (nowMs - lastAnnouncementMs_) >= kAnnouncementIntervalMs) {
    lastAnnouncementMs_ = nowMs;
    sendCurrentState();
  }
}

void ImprovSerial::buildDeviceName(char* out, size_t outLen) const {
  if (!out || outLen == 0) {
    return;
  }
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  snprintf(out, outLen, "%s-%02X%02X", PRODUCT_NAME, mac[4], mac[5]);
}

void ImprovSerial::buildDeviceInfoFields(char* firmwareVersion, size_t firmwareVersionLen, char* chipFamily,
                                         size_t chipFamilyLen, char* deviceName, size_t deviceNameLen) const {
  if (firmwareVersion && firmwareVersionLen > 0) {
    strlcpy(firmwareVersion, FIRMWARE_VERSION, firmwareVersionLen);
  }
  if (chipFamily && chipFamilyLen > 0) {
    String chip = ESP.getChipModel();
    strlcpy(chipFamily, chip.c_str(), chipFamilyLen);
  }
  if (deviceName && deviceNameLen > 0) {
    buildDeviceName(deviceName, deviceNameLen);
  }
}
