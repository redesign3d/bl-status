#include "local_config_portal.h"

#include <ctype.h>
#include <esp_random.h>
#include <string.h>

#include "admin_auth_store.h"
#include "device_identity.h"
#include "nvs_config_store.h"

namespace {
constexpr char kPortalBaseHead[] PROGMEM =
    "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:Arial,sans-serif;margin:20px auto;max-width:44rem;line-height:1.4;padding:0 12px}"
    "h1,h2{margin-bottom:.4rem}.card{padding:1rem;border:1px solid #ccc;border-radius:.8rem;background:#fafafa;margin:1rem 0}"
    "label{display:block;margin-top:.8rem;font-weight:600}input,select,button{width:100%;padding:.75rem;margin-top:.25rem;font:inherit;box-sizing:border-box}"
    "button{cursor:pointer}.status{padding:.8rem;border-radius:.6rem}.ok{background:#edf7ed;color:#155724}.err{background:#fdecea;color:#721c24}"
    "small{display:block;color:#555;margin-top:.25rem}code{font-family:monospace}.actions a{margin-right:1rem}</style></head><body>";
constexpr char kPortalTail[] PROGMEM = "</body></html>";

char hexNibble(uint8_t nibble) {
  return (nibble < 10) ? static_cast<char>('0' + nibble) : static_cast<char>('A' + (nibble - 10));
}

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
}  // namespace

LocalConfigPortal::LocalConfigPortal() : server_(nullptr), handler_(nullptr), running_(false), nowMs_(0) {
  memset(adminUser_, 0, sizeof(adminUser_));
  memset(adminPass_, 0, sizeof(adminPass_));
  memset(csrfToken_, 0, sizeof(csrfToken_));
  memset(rateSlots_, 0, sizeof(rateSlots_));
}

LocalConfigPortal::~LocalConfigPortal() { stop(); }

bool LocalConfigPortal::begin(uint16_t port, LocalConfigPortalHandler* handler) {
  stop();
  if (!handler || port == 0 || WiFi.status() != WL_CONNECTED || WiFi.localIP()[0] == 0) {
    return false;
  }
  handler_ = handler;
  if (!reloadCredentials()) {
    handler_ = nullptr;
    return false;
  }
  generateCsrfToken();
  memset(rateSlots_, 0, sizeof(rateSlots_));

  server_ = new WebServer(port);
  if (!server_) {
    stop();
    return false;
  }

  const char* headers[] = {"Content-Length"};
  server_->collectHeaders(headers, 1);
  server_->on("/", HTTP_GET, [this]() { handleRoot(); });
  server_->on("/config", HTTP_GET, [this]() { handleConfigGet(); });
  server_->on("/config", HTTP_POST, [this]() { handleConfigPost(); });
  server_->on("/admin-password", HTTP_POST, [this]() { handleAdminPasswordPost(); });
  server_->on("/reboot", HTTP_POST, [this]() { handleReboot(); });
  server_->on("/reset", HTTP_POST, [this]() { handleReset(); });
  server_->on("/health", HTTP_GET, [this]() { handleHealth(); });
  server_->onNotFound([this]() { handleNotFound(); });
  server_->begin();
  running_ = true;
  Serial.println("Local config portal started");
  return true;
}

void LocalConfigPortal::loop(uint32_t nowMs) {
  nowMs_ = nowMs;
  if (running_ && server_) {
    server_->handleClient();
  }
}

void LocalConfigPortal::stop() {
  if (server_) {
    server_->stop();
    delete server_;
    server_ = nullptr;
  }
  handler_ = nullptr;
  running_ = false;
  memset(adminUser_, 0, sizeof(adminUser_));
  memset(adminPass_, 0, sizeof(adminPass_));
  memset(csrfToken_, 0, sizeof(csrfToken_));
}

bool LocalConfigPortal::isRunning() const { return running_; }

bool LocalConfigPortal::allowRequest() {
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

bool LocalConfigPortal::ensureAuthenticated() {
  if (!server_) {
    return false;
  }
  if (server_->authenticate(adminUser_, adminPass_)) {
    return true;
  }
  const String realm = getUiTitle() + String(" Admin");
  sendSecurityHeaders();
  server_->requestAuthentication(BASIC_AUTH, realm.c_str(), "Authentication required");
  return false;
}

bool LocalConfigPortal::validateCsrf() const {
  return server_ && server_->hasArg("csrf") && server_->arg("csrf") == csrfToken_;
}

bool LocalConfigPortal::isAllowedConfigField(const String& name) const {
  return name == "wifiSsid" || name == "wifiPassword" || name == "printerHost" || name == "printerPort" ||
         name == "printerSerial" || name == "mqttUsername" || name == "accessCode" || name == "tlsInsecure" ||
         name == "csrf";
}

bool LocalConfigPortal::hasDisallowedControlChars(const String& value) const {
  for (size_t i = 0; i < value.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c == 0 || c < 0x20 || c > 0x7E) {
      return true;
    }
  }
  return false;
}

bool LocalConfigPortal::containsOnlyTokenChars(const char* value) const {
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

bool LocalConfigPortal::isValidHostValue(const char* host) const {
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

bool LocalConfigPortal::parseBooleanArg(const String& value, bool* outValue) const {
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

void LocalConfigPortal::appendInvalidField(String* errorMessage, bool* firstField,
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

String LocalConfigPortal::htmlEscape(const char* value) const {
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

void LocalConfigPortal::generateCsrfToken() {
  for (size_t i = 0; i < LOCAL_PORTAL_CSRF_TOKEN_LEN; ++i) {
    csrfToken_[i] = hexNibble(static_cast<uint8_t>(esp_random() & 0x0F));
  }
  csrfToken_[LOCAL_PORTAL_CSRF_TOKEN_LEN] = '\0';
}

bool LocalConfigPortal::reloadCredentials() {
  AdminCredentials credentials{};
  const bool ok = loadAdminCredentials(&credentials);
  if (ok) {
    strlcpy(adminUser_, credentials.username, sizeof(adminUser_));
    strlcpy(adminPass_, credentials.password, sizeof(adminPass_));
  }
  clearAdminCredentialsStruct(&credentials);
  return ok;
}

bool LocalConfigPortal::parseConfigUpdate(const DeviceConfig& currentConfig, DeviceConfig* outConfig,
                                          String* errorMessage) const {
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
    if (!isAllowedConfigField(server_->argName(i))) {
      *errorMessage = F("Unexpected form field.");
      return false;
    }
  }

  DeviceConfig merged = currentConfig;
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
  if (mqttUsernameLen == 0 || mqttUsernameLen > MQTT_USERNAME_MAX_LEN || !containsOnlyTokenChars(merged.mqttUsername)) {
    appendInvalidField(errorMessage, &firstField, F("mqttUsername"));
  }
  const size_t accessCodeLen = strlen(merged.accessCode);
  if (accessCodeLen < 4 || accessCodeLen > ACCESS_CODE_MAX_LEN || !containsOnlyTokenChars(merged.accessCode)) {
    appendInvalidField(errorMessage, &firstField, F("accessCode"));
  }

  if (!firstField) {
    return false;
  }

  const ConfigValidationResult validation = validateDeviceConfig(merged);
  if (!validation.ok) {
    *errorMessage = F("Configuration rejected.");
    return false;
  }

  *outConfig = merged;
  return true;
}

void LocalConfigPortal::sendSecurityHeaders() {
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

void LocalConfigPortal::sendLandingPage(const String& message, bool isError) {
  const String uiTitle = getUiTitle();
  String body;
  body.reserve(2600);
  body += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>");
  body += uiTitle;
  body += F("</title>");
  body += FPSTR(kPortalBaseHead);
  body += F("<h1>");
  body += uiTitle;
  body += F("</h1><div class='card'><p><strong>Connected Wi-Fi:</strong> <code>");
  body += htmlEscape(WiFi.SSID().c_str());
  body += F("</code><br><strong>IP Address:</strong> <code>");
  body += WiFi.localIP().toString();
  body += F("</code><br><strong>mDNS:</strong> <code>http://");
  body += getHostname();
  body += F(".local/</code></p><p class='actions'><a href='/config'>Edit configuration</a></p></div>");
  if (message.length() > 0) {
    body += F("<p class='status ");
    body += isError ? F("err") : F("ok");
    body += F("'><strong>");
    body += htmlEscape(message.c_str());
    body += F("</strong></p>");
  }
  body += FPSTR(kPortalTail);
  sendSecurityHeaders();
  server_->send(200, "text/html", body);
}

void LocalConfigPortal::sendConfigPage(const String& message, bool isError) {
  const DeviceConfig* currentConfig = handler_ ? handler_->activeConfig() : nullptr;
  if (!currentConfig) {
    sendResultPage(500, F("Configuration Error"), F("No active configuration available."));
    return;
  }

  const String uiTitle = getUiTitle();
  const String wifiSsidValue = htmlEscape(currentConfig->wifiSsid);
  const String printerHostValue = htmlEscape(currentConfig->printerHost);
  const String printerSerialValue = htmlEscape(currentConfig->printerSerial);
  const String mqttUsernameValue = htmlEscape(currentConfig->mqttUsername);
  String body;
  body.reserve(5200);
  body += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>");
  body += uiTitle;
  body += F(" Config</title>");
  body += FPSTR(kPortalBaseHead);
  body += F("<h1>");
  body += uiTitle;
  body += F("</h1><p>Reach this device at <code>http://");
  body += getHostname();
  body += F(".local/</code> while connected to the same network.</p>");
  if (message.length() > 0) {
    body += F("<p class='status ");
    body += isError ? F("err") : F("ok");
    body += F("'><strong>");
    body += htmlEscape(message.c_str());
    body += F("</strong></p>");
  }
  body += F("<div class='card'><h2>Device Config</h2><form method='POST' action='/config' autocomplete='off'>");
  body += F("<input type='hidden' name='csrf' value='");
  body += csrfToken_;
  body += F("'><label for='wifiSsid'>Wi-Fi SSID</label><input id='wifiSsid' name='wifiSsid' maxlength='32' value='");
  body += wifiSsidValue;
  body += F("' autocapitalize='none' spellcheck='false'>");
  body += F("<label for='wifiPassword'>Wi-Fi Password</label><input id='wifiPassword' type='password' name='wifiPassword' maxlength='63' autocomplete='new-password'><small>Leave blank to keep the current Wi-Fi password. Blank is valid for open networks.</small>");
  body += F("<label for='printerHost'>Printer Host</label><input id='printerHost' name='printerHost' maxlength='255' value='");
  body += printerHostValue;
  body += F("' autocapitalize='none' spellcheck='false'>");
  body += F("<label for='printerPort'>Printer MQTT Port</label><input id='printerPort' name='printerPort' inputmode='numeric' value='");
  body += String(currentConfig->printerPort);
  body += F("'>");
  body += F("<label for='printerSerial'>Printer Serial</label><input id='printerSerial' name='printerSerial' maxlength='31' value='");
  body += printerSerialValue;
  body += F("' autocapitalize='none' spellcheck='false'>");
  body += F("<label for='mqttUsername'>MQTT Username</label><input id='mqttUsername' name='mqttUsername' maxlength='31' value='");
  body += mqttUsernameValue;
  body += F("' autocapitalize='none' spellcheck='false'>");
  body += F("<label for='accessCode'>Access Code</label><input id='accessCode' type='password' name='accessCode' maxlength='63' autocomplete='new-password'><small>Leave blank to keep the current printer access code.</small>");
  body += F("<label for='tlsInsecure'>TLS</label><select id='tlsInsecure' name='tlsInsecure'>");
  body += currentConfig->tlsInsecure ? F("<option value='0'>Validate certificate</option><option value='1' selected>Insecure (LAN only)</option>")
                                     : F("<option value='0' selected>Validate certificate</option><option value='1'>Insecure (LAN only)</option>");
  body += F("</select><button type='submit'>Save And Reboot</button></form></div>");
  body += F("<div class='card'><h2>Admin Password</h2><form method='POST' action='/admin-password' autocomplete='off'>");
  body += F("<input type='hidden' name='csrf' value='");
  body += csrfToken_;
  body += F("'><label for='newAdminPassword'>New Admin Password</label><input id='newAdminPassword' type='password' name='newAdminPassword' maxlength='32' autocomplete='new-password'><label for='confirmAdminPassword'>Confirm Admin Password</label><input id='confirmAdminPassword' type='password' name='confirmAdminPassword' maxlength='32' autocomplete='new-password'><small>Use 12-32 letters, digits, dash, or underscore.</small><button type='submit'>Rotate Admin Password</button></form></div>");
  body += F("<div class='card'><h2>Device Actions</h2><form method='POST' action='/reboot' autocomplete='off'><input type='hidden' name='csrf' value='");
  body += csrfToken_;
  body += F("'><button type='submit'>Reboot Device</button></form><hr><form method='POST' action='/reset' autocomplete='off'><input type='hidden' name='csrf' value='");
  body += csrfToken_;
  body += F("'><label for='confirm'>Type ERASE to factory reset</label><input id='confirm' name='confirm' maxlength='5' autocapitalize='characters'><button type='submit'>Factory Reset</button></form></div>");
  body += FPSTR(kPortalTail);
  sendSecurityHeaders();
  server_->send(200, "text/html", body);
}

void LocalConfigPortal::sendResultPage(int code, const String& title, const String& message) {
  const String uiTitle = getUiTitle();
  String body;
  body.reserve(1600);
  body += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>");
  body += uiTitle;
  body += F("</title>");
  body += FPSTR(kPortalBaseHead);
  body += F("<h1>");
  body += uiTitle;
  body += F("</h1><h2>");
  body += htmlEscape(title.c_str());
  body += F("</h2><p class='status ");
  body += (code >= 400) ? F("err") : F("ok");
  body += F("'><strong>");
  body += htmlEscape(message.c_str());
  body += F("</strong></p><p><a href='/'>Home</a> <a href='/config'>Config</a></p>");
  body += FPSTR(kPortalTail);
  sendSecurityHeaders();
  server_->send(code, "text/html", body);
}

void LocalConfigPortal::sendHealth() {
  sendSecurityHeaders();
  char body[256];
  snprintf(body, sizeof(body),
           "{\"state\":\"normal\",\"hostname\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\"}",
           getHostname().c_str(), WiFi.localIP().toString().c_str(), WiFi.SSID().c_str());
  server_->send(200, "application/json", body);
}

void LocalConfigPortal::sendTooManyRequests() {
  sendSecurityHeaders();
  server_->send(429, "text/plain", "Too many requests");
}

void LocalConfigPortal::handleRoot() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  if (!ensureAuthenticated()) {
    return;
  }
  sendLandingPage(String(), false);
}

void LocalConfigPortal::handleConfigGet() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  if (!ensureAuthenticated()) {
    return;
  }
  sendConfigPage(String(), false);
}

void LocalConfigPortal::handleConfigPost() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  if (!ensureAuthenticated()) {
    return;
  }
  if (!validateCsrf()) {
    sendResultPage(403, F("Request Rejected"), F("Invalid CSRF token."));
    return;
  }

  const DeviceConfig* currentConfig = handler_ ? handler_->activeConfig() : nullptr;
  if (!currentConfig) {
    sendResultPage(500, F("Configuration Error"), F("No active configuration available."));
    return;
  }

  DeviceConfig updated{};
  String errorMessage;
  if (!parseConfigUpdate(*currentConfig, &updated, &errorMessage)) {
    sendConfigPage(errorMessage, true);
    clearDeviceConfig(&updated);
    return;
  }

  char message[96];
  memset(message, 0, sizeof(message));
  const bool ok = handler_ && handler_->saveRuntimeConfig(updated, message, sizeof(message));
  clearDeviceConfig(&updated);
  if (!ok) {
    sendResultPage(500, F("Save Failed"), message[0] != '\0' ? String(message) : String(F("Unable to save configuration.")));
    return;
  }
  sendResultPage(200, F("Configuration Saved"), message[0] != '\0' ? String(message) : String(F("Saved. Rebooting now.")));
}

void LocalConfigPortal::handleAdminPasswordPost() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  if (!ensureAuthenticated()) {
    return;
  }
  if (!validateCsrf()) {
    sendResultPage(403, F("Request Rejected"), F("Invalid CSRF token."));
    return;
  }

  const String newPassword = server_->arg("newAdminPassword");
  const String confirmPassword = server_->arg("confirmAdminPassword");
  if (newPassword.length() > ADMIN_PASSWORD_MAX_LEN || confirmPassword.length() > ADMIN_PASSWORD_MAX_LEN ||
      hasDisallowedControlChars(newPassword) || hasDisallowedControlChars(confirmPassword)) {
    sendConfigPage(F("Admin password contains invalid characters."), true);
    return;
  }
  if (newPassword != confirmPassword) {
    sendConfigPage(F("Admin password confirmation did not match."), true);
    return;
  }
  if (!validateAdminPassword(newPassword.c_str())) {
    sendConfigPage(F("Admin password must be 12-32 letters, digits, dash, or underscore."), true);
    return;
  }
  if (!updateAdminPassword(newPassword.c_str()) || !reloadCredentials()) {
    sendResultPage(500, F("Password Update Failed"), F("Unable to update admin password."));
    return;
  }
  sendResultPage(200, F("Password Updated"), F("Admin password updated. Re-authentication may be required."));
}

void LocalConfigPortal::handleReboot() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  if (!ensureAuthenticated()) {
    return;
  }
  if (!validateCsrf()) {
    sendResultPage(403, F("Request Rejected"), F("Invalid CSRF token."));
    return;
  }

  char message[96];
  memset(message, 0, sizeof(message));
  const bool ok = handler_ && handler_->requestRuntimeReboot(message, sizeof(message));
  if (!ok) {
    sendResultPage(500, F("Reboot Failed"), message[0] != '\0' ? String(message) : String(F("Unable to schedule reboot.")));
    return;
  }
  sendResultPage(200, F("Reboot Scheduled"), message[0] != '\0' ? String(message) : String(F("Device rebooting.")));
}

void LocalConfigPortal::handleReset() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  if (!ensureAuthenticated()) {
    return;
  }
  if (!validateCsrf()) {
    sendResultPage(403, F("Request Rejected"), F("Invalid CSRF token."));
    return;
  }
  if (!server_->hasArg("confirm") || server_->arg("confirm") != "ERASE") {
    sendConfigPage(F("Type ERASE to confirm factory reset."), true);
    return;
  }

  char message[96];
  memset(message, 0, sizeof(message));
  const bool ok = handler_ && handler_->requestFactoryResetAndReboot(message, sizeof(message));
  if (!ok) {
    sendResultPage(500, F("Reset Failed"), message[0] != '\0' ? String(message) : String(F("Unable to reset configuration.")));
    return;
  }
  sendResultPage(200, F("Factory Reset Scheduled"),
                 message[0] != '\0' ? String(message) : String(F("Configuration cleared. Rebooting into setup mode.")));
}

void LocalConfigPortal::handleHealth() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  sendHealth();
}

void LocalConfigPortal::handleNotFound() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  if (!ensureAuthenticated()) {
    return;
  }
  sendResultPage(404, F("Not Found"), F("The requested page was not found."));
}
