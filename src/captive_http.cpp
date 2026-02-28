#include "captive_http.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <string.h>

#include "device_identity.h"
#include "led_config_web.h"
#include "nvs_config_store.h"

namespace {
constexpr char kPortalPageHead[] PROGMEM =
    "<style>body{font-family:Arial,sans-serif;margin:20px auto;max-width:42rem;line-height:1.4;padding:0 12px}"
    "h1,h2{margin-bottom:.4rem}.card{padding:1rem;border:1px solid #ccc;border-radius:.8rem;background:#fafafa;margin:1rem 0}"
    "label{display:block;margin-top:.8rem;font-weight:600}input,select,button{width:100%;padding:.75rem;margin-top:.25rem;font:inherit;box-sizing:border-box}"
    "button{cursor:pointer}.status{padding:.8rem;border-radius:.6rem}.ok{background:#edf7ed;color:#155724}.err{background:#fdecea;color:#721c24}"
    "small{display:block;color:#555;margin-top:.25rem}code{font-family:monospace}</style></head><body>";
constexpr char kPortalPageIntro[] PROGMEM =
    "<p>Connect to the open setup Wi-Fi, then open <code>192.168.4.1</code> if this page did not open automatically.</p>"
    "<div class='card'><ol><li>Join the setup network.</li><li>Open <code>http://192.168.4.1/</code>.</li><li>Enter Wi-Fi and printer details.</li><li>Save and wait for reboot.</li></ol></div>";
constexpr char kPortalPageTail[] PROGMEM = "</body></html>";
constexpr char kPortalUrl[] = "http://192.168.4.1/";

bool isPrintableAscii(const char* value) {
  if (!value) {
    return false;
  }
  for (size_t i = 0; value[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c < 0x20 || c > 0x7E) {
      return false;
    }
  }
  return true;
}

bool parseConfirmFlag(const String& payload) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    return false;
  }
  return doc["confirm"].is<bool>() && doc["confirm"].as<bool>();
}
}  // namespace

CaptiveHttp::CaptiveHttp()
    : server_(nullptr), handler_(nullptr), running_(false), nowMs_(0), hasDraftConfig_(false) {
  memset(apSsid_, 0, sizeof(apSsid_));
  memset(apIp_, 0, sizeof(apIp_));
  memset(resetToken_, 0, sizeof(resetToken_));
  clearDeviceConfig(&draftConfig_);
  memset(rateSlots_, 0, sizeof(rateSlots_));
}

CaptiveHttp::~CaptiveHttp() { stop(); }

bool CaptiveHttp::begin(uint16_t port, const char* apSsid, const IPAddress& apIp, const char* resetToken,
                        CaptiveHttpHandler* handler) {
  stop();
  if (!apSsid || apSsid[0] == '\0' || !resetToken || !handler) {
    return false;
  }

  strlcpy(apSsid_, apSsid, sizeof(apSsid_));
  strlcpy(apIp_, apIp.toString().c_str(), sizeof(apIp_));
  strlcpy(resetToken_, resetToken, sizeof(resetToken_));
  handler_ = handler;
  memset(rateSlots_, 0, sizeof(rateSlots_));

  server_ = new WebServer(port);
  if (!server_) {
    handler_ = nullptr;
    return false;
  }

  const char* headers[] = {"Content-Length"};
  server_->collectHeaders(headers, 1);
  server_->on("/", HTTP_GET, [this]() { handleRoot(); });
  server_->on("/health", HTTP_GET, [this]() { handleHealth(); });
  server_->on("/provision", HTTP_POST, [this]() { handleProvision(); });
  server_->on("/led-config", HTTP_GET, [this]() { handleLedConfigGet(); });
  server_->on("/led-config", HTTP_POST, [this]() { handleLedConfigPost(); });
  server_->on("/led-reset", HTTP_POST, [this]() { handleLedReset(); });
  server_->on("/reset", HTTP_POST, [this]() { handleReset(); });
  server_->on("/generate_204", HTTP_ANY, [this]() { handleCaptiveProbe(); });
  server_->on("/hotspot-detect.html", HTTP_ANY, [this]() { handleCaptiveProbe(); });
  server_->on("/connecttest.txt", HTTP_ANY, [this]() { handleCaptiveProbe(); });
  server_->on("/ncsi.txt", HTTP_ANY, [this]() { handleCaptiveProbe(); });
  server_->on("/fwlink", HTTP_ANY, [this]() { handleCaptiveProbe(); });
  server_->onNotFound([this]() { handleNotFound(); });
  server_->begin();
  running_ = true;
  Serial.println("Captive portal HTTP server started");
  return true;
}

void CaptiveHttp::loop(uint32_t nowMs) {
  nowMs_ = nowMs;
  if (running_ && server_) {
    server_->handleClient();
  }
}

void CaptiveHttp::stop() {
  if (server_) {
    server_->stop();
    delete server_;
    server_ = nullptr;
  }
  handler_ = nullptr;
  running_ = false;
  hasDraftConfig_ = false;
  clearDeviceConfig(&draftConfig_);
  memset(apSsid_, 0, sizeof(apSsid_));
  memset(apIp_, 0, sizeof(apIp_));
  memset(resetToken_, 0, sizeof(resetToken_));
}

bool CaptiveHttp::isRunning() const { return running_; }

void CaptiveHttp::setDraftConfig(const DeviceConfig* draftConfig) {
  if (!draftConfig) {
    hasDraftConfig_ = false;
    clearDeviceConfig(&draftConfig_);
    return;
  }
  draftConfig_ = *draftConfig;
  hasDraftConfig_ = true;
}

bool CaptiveHttp::parseJsonBody(String* outBody, String* errorMessage) const {
  if (!server_ || !outBody || !errorMessage) {
    return false;
  }
  if (server_->hasHeader("Content-Length")) {
    const long bodySize = server_->header("Content-Length").toInt();
    if (bodySize < 0 || bodySize > static_cast<long>(LED_CONFIG_HTTP_MAX_BODY_BYTES)) {
      *errorMessage = F("Request body too large.");
      return false;
    }
  }

  const String body = server_->arg("plain");
  if (body.length() == 0 || body.length() > LED_CONFIG_HTTP_MAX_BODY_BYTES) {
    *errorMessage = F("Request body too large.");
    return false;
  }

  *outBody = body;
  return true;
}

bool CaptiveHttp::allowRequest() {
  if (!server_) {
    return false;
  }

  IPAddress ip = server_->client().remoteIP();
  int freeIndex = -1;
  int oldestIndex = 0;
  uint32_t oldestWindow = UINT32_MAX;

  for (int i = 0; i < PROVISIONING_RATE_LIMIT_SLOTS; ++i) {
    RateSlot& slot = rateSlots_[i];
    if (!slot.used) {
      if (freeIndex < 0) {
        freeIndex = i;
      }
      continue;
    }
    if (slot.ip == ip) {
      if ((nowMs_ - slot.windowStartMs) > PROVISIONING_RATE_LIMIT_WINDOW_MS) {
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
    if (slot.windowStartMs < oldestWindow) {
      oldestWindow = slot.windowStartMs;
      oldestIndex = i;
    }
  }

  const int useIndex = (freeIndex >= 0) ? freeIndex : oldestIndex;
  rateSlots_[useIndex].used = true;
  rateSlots_[useIndex].ip = ip;
  rateSlots_[useIndex].windowStartMs = nowMs_;
  rateSlots_[useIndex].count = 1;
  return true;
}

bool CaptiveHttp::isAllowedField(const String& name) const {
  return name == "wifiSsid" || name == "wifiPassword" || name == "printerHost" || name == "printerPort" ||
         name == "printerSerial" || name == "mqttUsername" || name == "accessCode" || name == "tlsInsecure";
}

bool CaptiveHttp::hasDisallowedControlChars(const String& value) const {
  for (size_t i = 0; i < value.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c == 0 || c < 0x20 || c > 0x7E) {
      return true;
    }
  }
  return false;
}

bool CaptiveHttp::containsOnlyTokenChars(const char* value) const {
  if (!value) {
    return false;
  }
  for (size_t i = 0; value[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (!(isalnum(c) || c == '-' || c == '_' || c == '.')) {
      return false;
    }
  }
  return true;
}

bool CaptiveHttp::isValidHostValue(const char* host) const {
  if (!host) {
    return false;
  }
  const size_t len = strlen(host);
  if (len == 0 || len > PRINTER_HOST_MAX_LEN) {
    return false;
  }
  IPAddress ip;
  if (ip.fromString(host)) {
    return true;
  }
  if (host[0] == '.' || host[0] == '-' || host[len - 1] == '.' || host[len - 1] == '-') {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    const unsigned char c = static_cast<unsigned char>(host[i]);
    if (!(isalnum(c) || c == '.' || c == '-')) {
      return false;
    }
  }
  return true;
}

bool CaptiveHttp::parseBooleanArg(const String& value, bool* outValue) const {
  if (!outValue) {
    return false;
  }
  if (value == "1" || value == "true" || value == "on") {
    *outValue = true;
    return true;
  }
  if (value == "0" || value == "false" || value == "off") {
    *outValue = false;
    return true;
  }
  return false;
}

void CaptiveHttp::appendInvalidField(String* errorMessage, bool* firstField,
                                     const __FlashStringHelper* fieldLabel) const {
  if (!errorMessage || !firstField || !fieldLabel) {
    return;
  }
  if (*firstField) {
    *errorMessage = F("Invalid or missing fields: ");
    *firstField = false;
  } else {
    *errorMessage += F(", ");
  }
  *errorMessage += fieldLabel;
}

String CaptiveHttp::htmlEscape(const char* value) const {
  String out;
  if (!value) {
    return out;
  }
  out.reserve(strlen(value) + 8);
  for (size_t i = 0; value[i] != '\0'; ++i) {
    switch (value[i]) {
      case '&':
        out += F("&amp;");
        break;
      case '<':
        out += F("&lt;");
        break;
      case '>':
        out += F("&gt;");
        break;
      case '\"':
        out += F("&quot;");
        break;
      case '\'':
        out += F("&#39;");
        break;
      default:
        out += value[i];
        break;
    }
  }
  return out;
}

bool CaptiveHttp::parseConfigFromRequest(DeviceConfig* outConfig, String* errorMessage) {
  if (!server_ || !outConfig || !errorMessage) {
    return false;
  }

  if (server_->hasHeader("Content-Length")) {
    const long bodySize = server_->header("Content-Length").toInt();
    if (bodySize < 0 || bodySize > static_cast<long>(PROVISIONING_HTTP_MAX_BODY_BYTES)) {
      *errorMessage = F("Request body too large.");
      return false;
    }
  }

  for (int i = 0; i < server_->args(); ++i) {
    if (!isAllowedField(server_->argName(i))) {
      *errorMessage = F("Unexpected form field.");
      return false;
    }
  }

  DeviceConfig merged{};
  if (hasDraftConfig_) {
    merged = draftConfig_;
  } else {
    clearDeviceConfig(&merged);
    merged.printerPort = DEFAULT_MQTT_TLS_PORT;
    strlcpy(merged.mqttUsername, DEFAULT_MQTT_USERNAME, sizeof(merged.mqttUsername));
    merged.tlsInsecure = DEFAULT_TLS_INSECURE;
  }

  const String wifiSsid = server_->arg("wifiSsid");
  const String wifiPassword = server_->arg("wifiPassword");
  const String printerHost = server_->arg("printerHost");
  const String printerPort = server_->arg("printerPort");
  const String printerSerial = server_->arg("printerSerial");
  const String mqttUsername = server_->arg("mqttUsername");
  const String accessCode = server_->arg("accessCode");
  const String tlsInsecure = server_->arg("tlsInsecure");

  const String values[] = {wifiSsid, wifiPassword, printerHost, printerPort, printerSerial, mqttUsername, accessCode,
                           tlsInsecure};
  for (const String& value : values) {
    if (value.length() > PROVISIONING_HTTP_MAX_BODY_BYTES || hasDisallowedControlChars(value)) {
      *errorMessage = F("Request contains invalid characters.");
      clearDeviceConfig(&merged);
      return false;
    }
  }

  if (wifiSsid.length() > 0) {
    strlcpy(merged.wifiSsid, wifiSsid.c_str(), sizeof(merged.wifiSsid));
  }
  if (wifiPassword.length() > 0) {
    strlcpy(merged.wifiPassword, wifiPassword.c_str(), sizeof(merged.wifiPassword));
  }
  if (printerHost.length() > 0) {
    strlcpy(merged.printerHost, printerHost.c_str(), sizeof(merged.printerHost));
  }
  if (printerPort.length() > 0) {
    const long parsedPort = printerPort.toInt();
    if (parsedPort <= 0 || parsedPort > 65535) {
      *errorMessage = F("Invalid or missing fields: printerPort");
      clearDeviceConfig(&merged);
      return false;
    }
    merged.printerPort = static_cast<uint16_t>(parsedPort);
  }
  if (printerSerial.length() > 0) {
    strlcpy(merged.printerSerial, printerSerial.c_str(), sizeof(merged.printerSerial));
  }
  if (mqttUsername.length() > 0) {
    strlcpy(merged.mqttUsername, mqttUsername.c_str(), sizeof(merged.mqttUsername));
  }
  if (accessCode.length() > 0) {
    strlcpy(merged.accessCode, accessCode.c_str(), sizeof(merged.accessCode));
  }
  if (tlsInsecure.length() > 0) {
    bool parsedBool = false;
    if (!parseBooleanArg(tlsInsecure, &parsedBool)) {
      *errorMessage = F("Invalid or missing fields: tlsInsecure");
      clearDeviceConfig(&merged);
      return false;
    }
    merged.tlsInsecure = parsedBool;
  }

  bool firstField = true;
  errorMessage->remove(0);

  const size_t wifiSsidLen = strlen(merged.wifiSsid);
  if (wifiSsidLen == 0 || wifiSsidLen > WIFI_SSID_MAX_LEN || !isPrintableAscii(merged.wifiSsid)) {
    appendInvalidField(errorMessage, &firstField, F("wifiSsid"));
  }
  const size_t wifiPasswordLen = strlen(merged.wifiPassword);
  if (wifiPasswordLen > WIFI_PASSWORD_MAX_LEN || !isPrintableAscii(merged.wifiPassword) ||
      (wifiPasswordLen > 0 && wifiPasswordLen < 8)) {
    appendInvalidField(errorMessage, &firstField, F("wifiPassword"));
  }
  if (!isValidHostValue(merged.printerHost)) {
    appendInvalidField(errorMessage, &firstField, F("printerHost"));
  }
  if (merged.printerPort < MIN_MQTT_PORT || merged.printerPort > MAX_MQTT_PORT) {
    appendInvalidField(errorMessage, &firstField, F("printerPort"));
  }
  const size_t printerSerialLen = strlen(merged.printerSerial);
  if (printerSerialLen < 6 || printerSerialLen > PRINTER_SERIAL_MAX_LEN || !containsOnlyTokenChars(merged.printerSerial)) {
    appendInvalidField(errorMessage, &firstField, F("printerSerial"));
  }
  const size_t mqttUsernameLen = strlen(merged.mqttUsername);
  if (mqttUsernameLen == 0 || mqttUsernameLen > MQTT_USERNAME_MAX_LEN ||
      !containsOnlyTokenChars(merged.mqttUsername)) {
    appendInvalidField(errorMessage, &firstField, F("mqttUsername"));
  }
  const size_t accessCodeLen = strlen(merged.accessCode);
  if (accessCodeLen < 4 || accessCodeLen > ACCESS_CODE_MAX_LEN || !containsOnlyTokenChars(merged.accessCode)) {
    appendInvalidField(errorMessage, &firstField, F("accessCode"));
  }

  if (!firstField) {
    clearDeviceConfig(&merged);
    return false;
  }

  ConfigValidationResult validation = validateDeviceConfig(merged);
  if (!validation.ok) {
    *errorMessage = F("Configuration rejected.");
    clearDeviceConfig(&merged);
    return false;
  }

  *outConfig = merged;
  return true;
}

void CaptiveHttp::sendSecurityHeaders() {
  if (!server_) {
    return;
  }
  server_->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server_->sendHeader("Pragma", "no-cache");
  server_->sendHeader("X-Content-Type-Options", "nosniff");
  server_->sendHeader("X-Frame-Options", "DENY");
  server_->sendHeader("Referrer-Policy", "no-referrer");
  server_->sendHeader("Content-Security-Policy",
                      "default-src 'self'; style-src 'unsafe-inline' 'self'; script-src 'unsafe-inline' 'self'; "
                      "connect-src 'self'; form-action 'self'; base-uri 'none'");
}

void CaptiveHttp::sendJson(int code, const String& body) {
  sendSecurityHeaders();
  server_->send(code, "application/json", body);
}

void CaptiveHttp::sendPortalPage(const String& message, bool isError) {
  sendSecurityHeaders();

  const String wifiSsidValue = htmlEscape(draftConfig_.wifiSsid);
  const String printerHostValue = htmlEscape(draftConfig_.printerHost);
  const String printerSerialValue = htmlEscape(draftConfig_.printerSerial);
  const String mqttUsernameValue = htmlEscape(draftConfig_.mqttUsername);
  const String setupSsidValue = htmlEscape(apSsid_);
  const bool keepWifiPassword = hasDraftConfig_ && draftConfig_.wifiPassword[0] != '\0';
  const bool keepAccessCode = hasDraftConfig_ && draftConfig_.accessCode[0] != '\0';
  const String uiTitle = getUiTitle();

  String body;
  body.reserve(17000);
  body += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>");
  body += uiTitle;
  body += F(" Setup</title>");
  body += FPSTR(kPortalPageHead);
  body += F("<h1>");
  body += uiTitle;
  body += F("</h1>");
  body += FPSTR(kPortalPageIntro);
  if (message.length() > 0) {
    body += F("<p class='status ");
    body += isError ? F("err") : F("ok");
    body += F("'><strong>");
    body += htmlEscape(message.c_str());
    body += F("</strong></p>");
  }
  body += F("<div class='card'><p><strong>Setup SSID:</strong> <code>");
  body += setupSsidValue;
  body += F("</code><br><strong>Setup URL:</strong> <code>");
  body += apIp_;
  body += F("</code></p><form method='POST' action='/provision' autocomplete='off' accept-charset='utf-8'>");
  body += F("<label for='wifiSsid'>Wi-Fi SSID</label><input id='wifiSsid' name='wifiSsid' maxlength='32' value='");
  body += wifiSsidValue;
  body += F("' autocapitalize='none' spellcheck='false'>");
  body += F("<label for=wifiPassword>Wi-Fi Password</label><input id=wifiPassword type=password name=wifiPassword maxlength=63 autocomplete=new-password>");
  if (keepWifiPassword) {
    body += F("<small>Leave blank to keep the current Wi-Fi password. Blank is also valid for open networks.</small>");
  } else {
    body += F("<small>Leave blank for open networks.</small>");
  }
  body += F("<label for='printerHost'>Printer Host</label><input id='printerHost' name='printerHost' maxlength='255' value='");
  body += printerHostValue;
  body += F("' autocapitalize='none' spellcheck='false'>");
  body += F("<label for='printerPort'>Printer MQTT Port</label><input id='printerPort' name='printerPort' inputmode='numeric' value='");
  body += String(hasDraftConfig_ && draftConfig_.printerPort != 0 ? draftConfig_.printerPort : DEFAULT_MQTT_TLS_PORT);
  body += F("'>");
  body += F("<label for='printerSerial'>Printer Serial</label><input id='printerSerial' name='printerSerial' maxlength='31' value='");
  body += printerSerialValue;
  body += F("' autocapitalize='none' spellcheck='false'>");
  body += F("<label for='mqttUsername'>MQTT Username</label><input id='mqttUsername' name='mqttUsername' maxlength='31' value='");
  body += mqttUsernameValue;
  body += F("' autocapitalize='none' spellcheck='false'>");
  body += F("<label for='accessCode'>Access Code</label><input id='accessCode' type='password' name='accessCode' maxlength='63' autocomplete='new-password'>");
  if (keepAccessCode) {
    body += F("<small>Leave blank to keep the current printer access code.</small>");
  }
  body += F("<label for='tlsInsecure'>TLS</label><select id='tlsInsecure' name='tlsInsecure'>");
  body += draftConfig_.tlsInsecure ? F("<option value='0'>Validate certificate</option><option value='1' selected>Insecure (LAN only)</option>")
                                   : F("<option value='0' selected>Validate certificate</option><option value='1'>Insecure (LAN only)</option>");
  body += F("</select><button type='submit'>Save And Reboot</button></form></div>");
  appendLedConfigEditorSection(&body, "/led-config", "/led-config", "/led-reset", nullptr, "Setup SSID", apSsid_,
                               "Setup URL", kPortalUrl);
  body += F("<div class='card'><h2>Reset Config</h2><p>Erase provisioned settings and stay in setup mode.</p>");
  body += F("<form method='POST' action='/reset' autocomplete='off'><input type='hidden' name='confirm_token' value='");
  body += resetToken_;
  body += F("'><label for='confirm'>Type ERASE to confirm</label><input id='confirm' name='confirm' maxlength='5' autocapitalize='characters'>");
  body += F("<button type='submit'>Reset Configuration</button></form></div>");
  body += FPSTR(kPortalPageTail);
  server_->send(200, "text/html", body);
}

void CaptiveHttp::sendResultPage(int code, const String& title, const String& message) {
  sendSecurityHeaders();
  const String uiTitle = getUiTitle();
  String body;
  body.reserve(1500);
  body += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>");
  body += uiTitle;
  body += F(" Setup</title>");
  body += FPSTR(kPortalPageHead);
  body += F("<h1>");
  body += uiTitle;
  body += F("</h1><h2>");
  body += htmlEscape(title.c_str());
  body += F("</h2><p class='status ");
  body += (code >= 400) ? F("err") : F("ok");
  body += F("'><strong>");
  body += htmlEscape(message.c_str());
  body += F("</strong></p><p><a href='/'>Return to setup</a></p>");
  body += FPSTR(kPortalPageTail);
  server_->send(code, "text/html", body);
}

void CaptiveHttp::sendHealth() {
  sendSecurityHeaders();
  char body[192];
  snprintf(body, sizeof(body),
           "{\"state\":\"provisioning\",\"apSsid\":\"%s\",\"portal\":\"%s\",\"resetAvailable\":true}",
           apSsid_, kPortalUrl);
  server_->send(200, "application/json", body);
}

void CaptiveHttp::sendRedirect() {
  sendSecurityHeaders();
  server_->sendHeader("Location", kPortalUrl, true);
  server_->send(302, "text/plain", "");
}

void CaptiveHttp::sendTooManyRequests() {
  sendSecurityHeaders();
  server_->send(429, "text/plain", "Too many requests");
}

void CaptiveHttp::sendPayloadTooLarge() {
  sendSecurityHeaders();
  server_->send(413, "text/plain", "Payload too large");
}

void CaptiveHttp::handleRoot() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  sendPortalPage(String(), false);
}

void CaptiveHttp::handleHealth() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  sendHealth();
}

void CaptiveHttp::handleProvision() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  if (!handler_) {
    sendResultPage(500, F("Setup Error"), F("Provisioning backend unavailable."));
    return;
  }
  if (server_->hasHeader("Content-Length")) {
    const long bodySize = server_->header("Content-Length").toInt();
    if (bodySize < 0 || bodySize > static_cast<long>(PROVISIONING_HTTP_MAX_BODY_BYTES)) {
      sendPayloadTooLarge();
      return;
    }
  }

  DeviceConfig submitted{};
  String errorMessage;
  if (!parseConfigFromRequest(&submitted, &errorMessage)) {
    sendPortalPage(errorMessage, true);
    clearDeviceConfig(&submitted);
    return;
  }

  char message[96];
  memset(message, 0, sizeof(message));
  const bool ok = handler_->applySubmittedConfig(submitted, message, sizeof(message));
  clearDeviceConfig(&submitted);
  if (!ok) {
    sendResultPage(500, F("Save Failed"), message[0] != '\0' ? String(message) : String(F("Unable to save configuration.")));
    return;
  }

  sendResultPage(200, F("Configuration Saved"),
                 message[0] != '\0' ? String(message) : String(F("Saved. Rebooting now.")));
}

void CaptiveHttp::handleLedConfigGet() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  if (!handler_) {
    sendJson(500, buildLedConfigActionResponse(false, "backend_unavailable", "LED settings backend unavailable.", false,
                                               LedPrinterState::UNKNOWN));
    return;
  }

  LedBehaviorConfig config{};
  if (!handler_->loadActiveLedConfig(&config)) {
    sendJson(500, buildLedConfigActionResponse(false, "load_failed", "Unable to load LED settings.", false,
                                               LedPrinterState::UNKNOWN));
    return;
  }
  sendJson(200, buildLedConfigApiJson(config));
}

void CaptiveHttp::handleLedConfigPost() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  if (!handler_) {
    sendJson(500, buildLedConfigActionResponse(false, "backend_unavailable", "LED settings backend unavailable.", false,
                                               LedPrinterState::UNKNOWN));
    return;
  }

  String payload;
  String errorMessage;
  if (!parseJsonBody(&payload, &errorMessage)) {
    sendJson(413, buildLedConfigActionResponse(false, "payload_too_large",
                                               errorMessage.length() > 0 ? errorMessage.c_str() : "Request body too large.",
                                               false, LedPrinterState::UNKNOWN));
    return;
  }

  LedBehaviorConfig config{};
  LedConfigJsonParseResult parseResult{};
  if (!parseLedConfigJsonPayload(payload, &config, &parseResult, &errorMessage)) {
    sendJson(400, buildLedConfigActionResponse(false, parseResult.code,
                                               errorMessage.length() > 0 ? errorMessage.c_str() : "LED settings rejected.",
                                               parseResult.hasState, parseResult.state));
    return;
  }

  char message[96];
  memset(message, 0, sizeof(message));
  if (!handler_->saveLedConfig(config, message, sizeof(message))) {
    sendJson(500, buildLedConfigActionResponse(false, "save_failed",
                                               message[0] != '\0' ? message : "Unable to save LED settings.", false,
                                               LedPrinterState::UNKNOWN));
    return;
  }

  LedBehaviorConfig activeConfig{};
  if (!handler_->loadActiveLedConfig(&activeConfig)) {
    activeConfig = config;
  }
  String response = buildLedConfigActionResponse(true, "saved",
                                                 message[0] != '\0' ? message : "LED settings applied.", false,
                                                 LedPrinterState::UNKNOWN);
  if (response.endsWith("}")) {
    response.remove(response.length() - 1);
    response += F(",\"config\":");
    response += buildLedConfigApiJson(activeConfig);
    response += '}';
  }
  sendJson(200, response);
}

void CaptiveHttp::handleLedReset() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  if (!handler_) {
    sendJson(500, buildLedConfigActionResponse(false, "backend_unavailable", "LED settings backend unavailable.", false,
                                               LedPrinterState::UNKNOWN));
    return;
  }

  String payload;
  String errorMessage;
  if (!parseJsonBody(&payload, &errorMessage)) {
    sendJson(413, buildLedConfigActionResponse(false, "payload_too_large",
                                               errorMessage.length() > 0 ? errorMessage.c_str() : "Request body too large.",
                                               false, LedPrinterState::UNKNOWN));
    return;
  }
  if (!parseConfirmFlag(payload)) {
    sendJson(400, buildLedConfigActionResponse(false, "confirm_required",
                                               "Reset confirmation required for LED settings.", false,
                                               LedPrinterState::UNKNOWN));
    return;
  }

  char message[96];
  memset(message, 0, sizeof(message));
  if (!handler_->resetLedConfig(message, sizeof(message))) {
    sendJson(500, buildLedConfigActionResponse(false, "reset_failed",
                                               message[0] != '\0' ? message : "Unable to reset LED settings.", false,
                                               LedPrinterState::UNKNOWN));
    return;
  }

  LedBehaviorConfig activeConfig{};
  if (!handler_->loadActiveLedConfig(&activeConfig)) {
    setDefaultLedBehaviorConfig(&activeConfig);
    normalizeLedBehaviorConfig(&activeConfig);
  }
  String response = buildLedConfigActionResponse(true, "reset",
                                                 message[0] != '\0' ? message : "LED settings restored to defaults.",
                                                 false, LedPrinterState::UNKNOWN);
  if (response.endsWith("}")) {
    response.remove(response.length() - 1);
    response += F(",\"config\":");
    response += buildLedConfigApiJson(activeConfig);
    response += '}';
  }
  sendJson(200, response);
}

void CaptiveHttp::handleReset() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  if (!handler_) {
    sendResultPage(500, F("Reset Error"), F("Provisioning backend unavailable."));
    return;
  }
  if (!server_->hasArg("confirm_token") || !server_->hasArg("confirm") || server_->arg("confirm_token") != resetToken_ ||
      server_->arg("confirm") != "ERASE") {
    sendPortalPage(F("Reset request rejected. Type ERASE to confirm."), true);
    return;
  }

  char message[96];
  memset(message, 0, sizeof(message));
  const bool ok = handler_->resetProvisioningConfig(message, sizeof(message));
  if (!ok) {
    sendResultPage(500, F("Reset Failed"), message[0] != '\0' ? String(message) : String(F("Unable to clear configuration.")));
    return;
  }
  sendResultPage(200, F("Configuration Cleared"),
                 message[0] != '\0' ? String(message) : String(F("Configuration erased. Device remains in setup mode.")));
}

void CaptiveHttp::handleCaptiveProbe() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  sendRedirect();
}

void CaptiveHttp::handleNotFound() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  sendRedirect();
}
