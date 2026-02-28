#include "captive_portal_http.h"

#include <esp_random.h>
#include <string.h>

#include "nvs_config_store.h"

namespace {
uint32_t random32() { return esp_random(); }

char hexNibble(uint8_t v) { return (v < 10) ? static_cast<char>('0' + v) : static_cast<char>('a' + (v - 10)); }
}  // namespace

CaptivePortalHttp::CaptivePortalHttp()
    : server_(nullptr),
      running_(false),
      nowMs_(0),
      pairingExpiryMs_(0),
      sessionExpiryMs_(0),
      pendingProvision_(false),
      pendingReset_(false) {
  memset(pairingCode_, 0, sizeof(pairingCode_));
  memset(resetToken_, 0, sizeof(resetToken_));
  memset(sessionId_, 0, sizeof(sessionId_));
  memset(&pendingConfig_, 0, sizeof(pendingConfig_));
  memset(rateSlots_, 0, sizeof(rateSlots_));
}

CaptivePortalHttp::~CaptivePortalHttp() { stop(); }

bool CaptivePortalHttp::begin(uint16_t port, const char* pairingCode, uint32_t pairingExpiryMs,
                              const char* resetToken) {
  stop();
  if (!pairingCode || !resetToken) {
    return false;
  }

  strlcpy(pairingCode_, pairingCode, sizeof(pairingCode_));
  strlcpy(resetToken_, resetToken, sizeof(resetToken_));
  pairingExpiryMs_ = pairingExpiryMs;
  pendingProvision_ = false;
  pendingReset_ = false;
  clearSession();
  memset(rateSlots_, 0, sizeof(rateSlots_));

  server_ = new WebServer(port);
  if (!server_) {
    return false;
  }
  const char* headers[] = {"Cookie", "Content-Length"};
  server_->collectHeaders(headers, 2);

  server_->on("/", HTTP_GET, [this]() { handleRoot(); });
  server_->on("/provision", HTTP_POST, [this]() { handleProvision(); });
  server_->on("/reset", HTTP_POST, [this]() { handleReset(); });
  server_->on("/health", HTTP_GET, [this]() { handleHealth(); });
  server_->onNotFound([this]() { handleNotFound(); });
  server_->begin();
  running_ = true;
  Serial.println("Captive portal HTTP server started");
  return true;
}

void CaptivePortalHttp::loop(uint32_t nowMs) {
  nowMs_ = nowMs;
  if (!running_ || !server_) {
    return;
  }
  server_->handleClient();
}

void CaptivePortalHttp::stop() {
  if (server_) {
    server_->stop();
    delete server_;
    server_ = nullptr;
  }
  running_ = false;
  clearSession();
  memset(pairingCode_, 0, sizeof(pairingCode_));
  memset(resetToken_, 0, sizeof(resetToken_));
}

bool CaptivePortalHttp::isRunning() const { return running_; }

bool CaptivePortalHttp::consumeProvisionRequest(DeviceConfig* outConfig) {
  if (!pendingProvision_ || !outConfig) {
    return false;
  }
  *outConfig = pendingConfig_;
  pendingProvision_ = false;
  clearDeviceConfig(&pendingConfig_);
  return true;
}

bool CaptivePortalHttp::consumeResetRequest() {
  if (!pendingReset_) {
    return false;
  }
  pendingReset_ = false;
  return true;
}

void CaptivePortalHttp::invalidateAuth() { clearSession(); }

bool CaptivePortalHttp::allowRequest() {
  if (!server_) {
    return false;
  }
  IPAddress ip = server_->client().remoteIP();

  int freeIndex = -1;
  int oldestIndex = 0;
  uint32_t oldestTime = UINT32_MAX;

  for (int i = 0; i < static_cast<int>(sizeof(rateSlots_) / sizeof(rateSlots_[0])); ++i) {
    RateSlot& slot = rateSlots_[i];
    if (!slot.used && freeIndex < 0) {
      freeIndex = i;
      continue;
    }
    if (slot.used && slot.ip == ip) {
      if (nowMs_ - slot.windowStartMs > PROVISIONING_RATE_LIMIT_WINDOW_MS) {
        slot.windowStartMs = nowMs_;
        slot.count = 1;
        return true;
      }
      if (slot.count >= PROVISIONING_RATE_LIMIT_REQUESTS) {
        return false;
      }
      slot.count++;
      return true;
    }
    if (slot.used && slot.windowStartMs < oldestTime) {
      oldestTime = slot.windowStartMs;
      oldestIndex = i;
    }
  }

  int useIndex = (freeIndex >= 0) ? freeIndex : oldestIndex;
  rateSlots_[useIndex].used = true;
  rateSlots_[useIndex].ip = ip;
  rateSlots_[useIndex].windowStartMs = nowMs_;
  rateSlots_[useIndex].count = 1;
  return true;
}

bool CaptivePortalHttp::isPairingCodeValid(const String& provided, uint32_t nowMs) const {
  if (pairingCode_[0] == '\0') {
    return false;
  }
  if (nowMs > pairingExpiryMs_) {
    return false;
  }
  if (provided.length() != strlen(pairingCode_)) {
    return false;
  }
  return provided.equals(pairingCode_);
}

bool CaptivePortalHttp::hasValidSession(uint32_t nowMs) const {
  return (sessionId_[0] != '\0') && (nowMs <= sessionExpiryMs_);
}

bool CaptivePortalHttp::hasValidSessionFromRequest(uint32_t nowMs) const {
  if (!hasValidSession(nowMs)) {
    return false;
  }
  String sid = readCookie("SID");
  return sid.equals(sessionId_);
}

void CaptivePortalHttp::createSession(uint32_t nowMs) {
  for (size_t i = 0; i < sizeof(sessionId_) - 1; ++i) {
    if (i % 2 == 0) {
      uint8_t r = static_cast<uint8_t>(random32() & 0xFF);
      sessionId_[i] = hexNibble((r >> 4) & 0x0F);
      if (i + 1 < sizeof(sessionId_) - 1) {
        sessionId_[i + 1] = hexNibble(r & 0x0F);
        ++i;
      }
    }
  }
  sessionId_[sizeof(sessionId_) - 1] = '\0';
  sessionExpiryMs_ = nowMs + PROVISIONING_SESSION_TTL_MS;
}

void CaptivePortalHttp::clearSession() {
  memset(sessionId_, 0, sizeof(sessionId_));
  sessionExpiryMs_ = 0;
}

String CaptivePortalHttp::readCookie(const String& name) const {
  if (!server_ || !server_->hasHeader("Cookie")) {
    return "";
  }
  String cookie = server_->header("Cookie");
  String prefix = name + "=";
  int start = cookie.indexOf(prefix);
  if (start < 0) {
    return "";
  }
  start += prefix.length();
  int end = cookie.indexOf(';', start);
  if (end < 0) {
    end = cookie.length();
  }
  return cookie.substring(start, end);
}

bool CaptivePortalHttp::hasDisallowedControlChars(const String& value) const {
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value[i];
    if (c == '\0' || c < 0x20 || c > 0x7E) {
      return true;
    }
  }
  return false;
}

bool CaptivePortalHttp::parseConfigFromRequest(DeviceConfig* outConfig) {
  if (!server_ || !outConfig) {
    return false;
  }

  if (!server_->hasArg("ws") || !server_->hasArg("wp") || !server_->hasArg("ph") || !server_->hasArg("pp") ||
      !server_->hasArg("ps") || !server_->hasArg("mu") || !server_->hasArg("ac") || !server_->hasArg("ti")) {
    return false;
  }

  String ssid = server_->arg("ws");
  String password = server_->arg("wp");
  String host = server_->arg("ph");
  String portStr = server_->arg("pp");
  String serial = server_->arg("ps");
  String mqttUser = server_->arg("mu");
  String accessCode = server_->arg("ac");
  String tlsInsecure = server_->arg("ti");

  const String values[] = {ssid, password, host, portStr, serial, mqttUser, accessCode, tlsInsecure};
  for (const String& value : values) {
    if (value.length() > PROVISIONING_HTTP_MAX_BODY_BYTES) {
      return false;
    }
    if (hasDisallowedControlChars(value)) {
      return false;
    }
  }

  long port = portStr.toInt();
  if (port <= 0 || port > 65535) {
    return false;
  }

  clearDeviceConfig(outConfig);
  strlcpy(outConfig->wifiSsid, ssid.c_str(), sizeof(outConfig->wifiSsid));
  strlcpy(outConfig->wifiPassword, password.c_str(), sizeof(outConfig->wifiPassword));
  strlcpy(outConfig->printerHost, host.c_str(), sizeof(outConfig->printerHost));
  outConfig->printerPort = static_cast<uint16_t>(port);
  strlcpy(outConfig->printerSerial, serial.c_str(), sizeof(outConfig->printerSerial));
  strlcpy(outConfig->mqttUsername, mqttUser.c_str(), sizeof(outConfig->mqttUsername));
  strlcpy(outConfig->accessCode, accessCode.c_str(), sizeof(outConfig->accessCode));
  outConfig->tlsInsecure = tlsInsecure == "1";

  ConfigValidationResult validation = validateDeviceConfig(*outConfig);
  return validation.ok;
}

void CaptivePortalHttp::sendSecurityHeaders() {
  if (!server_) {
    return;
  }
  server_->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server_->sendHeader("Pragma", "no-cache");
  server_->sendHeader("X-Content-Type-Options", "nosniff");
  server_->sendHeader("X-Frame-Options", "DENY");
  server_->sendHeader("Referrer-Policy", "no-referrer");
  server_->sendHeader("Content-Security-Policy",
                      "default-src 'self'; style-src 'unsafe-inline' 'self'; form-action 'self'; base-uri 'none'");
}

void CaptivePortalHttp::sendUnauthorized() {
  sendSecurityHeaders();
  server_->send(401, "text/plain", "Unauthorized");
}

void CaptivePortalHttp::sendTooManyRequests() {
  sendSecurityHeaders();
  server_->send(429, "text/plain", "Too many requests");
}

void CaptivePortalHttp::sendGenericFailure() {
  sendSecurityHeaders();
  server_->send(400, "text/plain", "Request rejected");
}

void CaptivePortalHttp::sendAuthPage(const String& message) {
  sendSecurityHeaders();
  String body;
  body.reserve(900);
  body += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' "
            "content='width=device-width,initial-scale=1'>");
  body += F("<title>Device Provisioning</title><style>body{font-family:Arial,sans-serif;margin:24px;max-width:460px}"
            "input,button{width:100%;padding:10px;margin-top:8px}small{color:#555}</style></head><body>");
  body += F("<h2>Device Provisioning</h2><p>Enter the pairing code shown on the device output.</p>");
  if (message.length()) {
    body += F("<p><strong>");
    body += message;
    body += F("</strong></p>");
  }
  body += F("<form method='GET' action='/' autocomplete='off'>"
            "<label>Pairing code</label><input name='code' maxlength='6' inputmode='numeric' required>"
            "<button type='submit'>Unlock</button></form><small>Code expires quickly. Generate a new one by rebooting "
            "if needed.</small></body></html>");
  server_->send(200, "text/html", body);
}

void CaptivePortalHttp::sendProvisionPage(const String& message) {
  sendSecurityHeaders();
  String body;
  body.reserve(2200);
  body += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' "
            "content='width=device-width,initial-scale=1'>");
  body += F("<title>Configure Device</title><style>body{font-family:Arial,sans-serif;margin:24px;max-width:560px}"
            "label{display:block;margin-top:10px}input,select,button{width:100%;padding:10px;margin-top:4px}"
            ".warn{color:#8a2f00}</style></head><body>");
  body += F("<h2>Configure Device</h2>");
  if (message.length()) {
    body += F("<p><strong>");
    body += message;
    body += F("</strong></p>");
  }
  body += F("<form method='POST' action='/provision' autocomplete='off'>");
  body += F("<label>Wi-Fi SSID</label><input name='ws' maxlength='32' required>");
  body += F("<label>Wi-Fi Password</label><input type='password' name='wp' minlength='8' maxlength='63' required>");
  body += F("<label>Printer Host (IPv4 or DNS)</label><input name='ph' maxlength='63' required>");
  body += F("<label>Printer MQTT Port</label><input name='pp' inputmode='numeric' value='8883' required>");
  body += F("<label>Printer Serial</label><input name='ps' maxlength='31' required>");
  body += F("<label>MQTT Username</label><input name='mu' maxlength='31' required>");
  body += F("<label>Access Code</label><input type='password' name='ac' maxlength='63' required>");
  body += F("<label>TLS Mode</label><select name='ti'><option value='0' selected>Validate certificate</option>"
            "<option value='1'>Insecure (LAN only)</option></select>");
  body += F("<button type='submit'>Save And Reboot</button></form>");
  body += F("<hr><h3>Factory Reset</h3><p class='warn'>Clears provisioned connectivity settings.</p>");
  body += F("<form method='POST' action='/reset' autocomplete='off'>");
  body += F("<input type='hidden' name='confirm_token' value='");
  body += resetToken_;
  body += F("'><label>Type ERASE to confirm</label><input name='confirm' maxlength='5' required>");
  body += F("<button type='submit'>Reset Configuration</button></form></body></html>");
  server_->send(200, "text/html", body);
}

void CaptivePortalHttp::handleRoot() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }

  if (hasValidSessionFromRequest(nowMs_)) {
    sendProvisionPage("");
    return;
  }

  if (server_->hasArg("code") && isPairingCodeValid(server_->arg("code"), nowMs_)) {
    createSession(nowMs_);
    sendSecurityHeaders();
    server_->sendHeader("Set-Cookie", String("SID=") + sessionId_ + "; HttpOnly; SameSite=Strict");
    sendProvisionPage("Authenticated. Configure the device.");
    return;
  }

  sendAuthPage("Authentication required.");
}

void CaptivePortalHttp::handleProvision() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }

  if (!hasValidSessionFromRequest(nowMs_)) {
    sendUnauthorized();
    return;
  }

  if (server_->hasHeader("Content-Length")) {
    long bodySize = server_->header("Content-Length").toInt();
    if (bodySize < 0 || bodySize > static_cast<long>(PROVISIONING_HTTP_MAX_BODY_BYTES)) {
      sendSecurityHeaders();
      server_->send(413, "text/plain", "Payload too large");
      return;
    }
  }

  DeviceConfig cfg{};
  if (!parseConfigFromRequest(&cfg)) {
    sendGenericFailure();
    clearDeviceConfig(&cfg);
    return;
  }

  pendingConfig_ = cfg;
  pendingProvision_ = true;
  clearDeviceConfig(&cfg);

  sendSecurityHeaders();
  server_->send(202, "text/plain", "Provisioning request accepted");
}

void CaptivePortalHttp::handleReset() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  if (!hasValidSessionFromRequest(nowMs_)) {
    sendUnauthorized();
    return;
  }
  if (!server_->hasArg("confirm_token") || !server_->hasArg("confirm")) {
    sendGenericFailure();
    return;
  }
  if (!server_->arg("confirm_token").equals(resetToken_) || !server_->arg("confirm").equals("ERASE")) {
    sendGenericFailure();
    return;
  }
  pendingReset_ = true;
  sendSecurityHeaders();
  server_->send(202, "text/plain", "Reset request accepted");
}

void CaptivePortalHttp::handleHealth() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  sendSecurityHeaders();
  String body = String("{\"mode\":\"provisioning\",\"auth\":") +
                (hasValidSessionFromRequest(nowMs_) ? "\"session\"" : "\"pairing\"") + "}";
  server_->send(200, "application/json", body);
}

void CaptivePortalHttp::handleNotFound() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  sendSecurityHeaders();
  server_->sendHeader("Location", "/", true);
  server_->send(302, "text/plain", "");
}
