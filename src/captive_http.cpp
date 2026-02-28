#include "captive_http.h"

#include <string.h>

#include "config.h"

namespace {
constexpr char kPortalUrl[] = "http://192.168.4.1/";
constexpr char kPortalPageTop[] PROGMEM =
    "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' "
    "content='width=device-width,initial-scale=1'><title>Bambu Status Setup</title>"
    "<style>body{font-family:Arial,sans-serif;margin:20px;max-width:42rem;line-height:1.4}"
    "h1{margin-bottom:.25rem}ol{padding-left:1.2rem}code,input,button{font:inherit}"
    ".card{padding:1rem;border:1px solid #ccc;border-radius:.75rem;background:#fafafa}"
    "</style></head><body><h1>Bambu Status Setup</h1>"
    "<p>Connect to the setup Wi-Fi, then open <code>http://192.168.4.1/</code> if this page did not open automatically.</p>"
    "<div class='card'><ol><li>Join the open setup network.</li><li>Open <code>192.168.4.1</code>.</li>"
    "<li>Enter your Wi-Fi and printer settings.</li></ol></div><p><strong>Setup SSID:</strong> ";
constexpr char kPortalPageBottom[] PROGMEM =
    "</p><p><strong>Setup IP:</strong> <code>192.168.4.1</code></p>"
    "<p>The configuration form will appear here in the next firmware step.</p></body></html>";
}  // namespace

CaptiveHttp::CaptiveHttp() : server_(nullptr), running_(false), nowMs_(0) {
  memset(apSsid_, 0, sizeof(apSsid_));
  memset(apIp_, 0, sizeof(apIp_));
  memset(rateSlots_, 0, sizeof(rateSlots_));
}

CaptiveHttp::~CaptiveHttp() { stop(); }

bool CaptiveHttp::begin(uint16_t port, const char* apSsid, const IPAddress& apIp) {
  stop();
  if (!apSsid || apSsid[0] == '\0') {
    return false;
  }

  strlcpy(apSsid_, apSsid, sizeof(apSsid_));
  strlcpy(apIp_, apIp.toString().c_str(), sizeof(apIp_));
  memset(rateSlots_, 0, sizeof(rateSlots_));

  server_ = new WebServer(port);
  if (!server_) {
    return false;
  }

  server_->on("/", HTTP_GET, [this]() { handleRoot(); });
  server_->on("/health", HTTP_GET, [this]() { handleHealth(); });
  server_->on("/generate_204", HTTP_ANY, [this]() { handleCaptiveProbe(); });
  server_->on("/hotspot-detect.html", HTTP_ANY, [this]() { handleCaptiveProbe(); });
  server_->on("/connecttest.txt", HTTP_ANY, [this]() { handleCaptiveProbe(); });
  server_->on("/ncsi.txt", HTTP_ANY, [this]() { handleCaptiveProbe(); });
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
  running_ = false;
  memset(apSsid_, 0, sizeof(apSsid_));
  memset(apIp_, 0, sizeof(apIp_));
}

bool CaptiveHttp::isRunning() const { return running_; }

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

  int useIndex = (freeIndex >= 0) ? freeIndex : oldestIndex;
  rateSlots_[useIndex].used = true;
  rateSlots_[useIndex].ip = ip;
  rateSlots_[useIndex].windowStartMs = nowMs_;
  rateSlots_[useIndex].count = 1;
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
                      "default-src 'self'; style-src 'unsafe-inline' 'self'; form-action 'self'; base-uri 'none'");
}

void CaptiveHttp::sendPortalPage() {
  sendSecurityHeaders();
  server_->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server_->send(200, "text/html", "");
  server_->sendContent_P(kPortalPageTop);
  server_->sendContent(apSsid_);
  server_->sendContent_P(kPortalPageBottom);
  server_->sendContent("");
}

void CaptiveHttp::sendHealth() {
  sendSecurityHeaders();
  char body[160];
  snprintf(body, sizeof(body),
           "{\"state\":\"provisioning\",\"apSsid\":\"%s\",\"portal\":\"%s\"}",
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

void CaptiveHttp::handleRoot() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  sendPortalPage();
}

void CaptiveHttp::handleHealth() {
  if (!allowRequest()) {
    sendTooManyRequests();
    return;
  }
  sendHealth();
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
