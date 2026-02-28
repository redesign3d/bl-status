#include "Display.h"

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <qrcode.h>

static Adafruit_SSD1306 display(128, 64, &Wire, OLED_RESET_PIN);

namespace {
constexpr char kProvisioningQrUrl[] = "http://192.168.4.1/";

void drawWifiScreen() {
  display.clearDisplay();
  display.setTextWrap(false);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Bambu Status");

  display.setTextSize(2);
  display.setCursor(0, 16);
  display.println("WiFi");

  display.setTextSize(1);
  display.setCursor(0, 40);
  display.println("Connecting station");
  display.setCursor(0, 52);
  display.println("Connecting...");

  display.display();
}

void drawCheckmark(int x, int y, int size) {
  display.drawLine(x, y + size / 2, x + size / 3, y + size, SSD1306_WHITE);
  display.drawLine(x + size / 3, y + size, x + size, y, SSD1306_WHITE);
}

String formatTemp(float value) {
  if (!isValidTemp(value)) {
    return String("--");
  }
  int rounded = static_cast<int>(roundf(value));
  return String(rounded);
}

void drawQrCode(int originX, int originY) {
  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(3)];
  qrcode_initText(&qrcode, qrcodeData, 3, ECC_LOW, kProvisioningQrUrl);

  for (uint8_t y = 0; y < qrcode.size; ++y) {
    for (uint8_t x = 0; x < qrcode.size; ++x) {
      if (qrcode_getModule(&qrcode, x, y)) {
        display.fillRect(originX + (x * PROVISIONING_QR_SCALE), originY + (y * PROVISIONING_QR_SCALE),
                         PROVISIONING_QR_SCALE, PROVISIONING_QR_SCALE, SSD1306_WHITE);
      }
    }
  }
}

void printWrappedValue(const char* value, int x, int y, size_t firstLineChars, size_t secondLineChars) {
  if (!value) {
    return;
  }
  const size_t len = strlen(value);
  String first = String(value).substring(0, min(len, firstLineChars));
  display.setCursor(x, y);
  display.println(first);
  if (len > firstLineChars) {
    String second = String(value).substring(firstLineChars, min(len, firstLineChars + secondLineChars));
    display.setCursor(x, y + 10);
    display.println(second);
  }
}
}  // namespace

bool initDisplay() {
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
    Serial.println("Failed to initialize SSD1306 display");
    return false;
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.display();
  return true;
}

void drawStatus(const PrinterStatus& status) {
  if (!status.wifiConnected) {
    drawWifiScreen();
    return;
  }

  display.clearDisplay();
  display.setTextWrap(false);

  // Top line: static title
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Bambu Status");

  constexpr int charWidth = 6;
  int statusTextSize = 2;
  int statusWidth = status.statusText.length() * charWidth * statusTextSize;
  if (statusWidth > 128) {
    statusTextSize = 1;
    statusWidth = status.statusText.length() * charWidth * statusTextSize;
  }
  int statusY = (statusTextSize == 2) ? 12 : 14;
  bool highlightStatus = (statusTextSize == 1);
  if (highlightStatus) {
    int boxHeight = 8 * statusTextSize + 4;
    int boxY = statusY - 2;
    display.fillRect(0, boxY, 128, boxHeight, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  }
  bool hasPrinterLink = status.mqttConnected || status.lastUpdateMs > 0;
  bool wifiOnly = status.wifiConnected && !hasPrinterLink;

  String lineText = status.statusText;
  if (wifiOnly) {
    lineText = "WiFi [OK]";        // WiFi connected only
  } else if (hasPrinterLink) {
    lineText = "MQTT [OK]";        // MQTT connected and/or printer data received
  }

  display.setTextSize(statusTextSize);
  display.setCursor(0, statusY);
  display.println(lineText);
  if (highlightStatus) {
    display.setTextColor(SSD1306_WHITE);
  }

  display.setTextSize(1);
  int progressY = (statusTextSize == 2) ? 32 : 26;
  display.setCursor(0, progressY);
  int progressInt = static_cast<int>(roundf(status.progress));
  String progressLine = "Prog: " + String(progressInt) + "%";
  if (status.hasLayerInfo && status.layerNum >= 0 && status.totalLayerNum > 0) {
    progressLine += "  L " + String(status.layerNum) + "/" + String(status.totalLayerNum);
  }
  display.println(progressLine);

  display.setCursor(0, progressY + 10);
  String tempLine = "N:" + formatTemp(status.nozzleTemp) + "/" + formatTemp(status.nozzleTarget);
  tempLine += "  B:" + formatTemp(status.bedTemp) + "/" + formatTemp(status.bedTarget);
  display.println(tempLine);

  const int barX = 0;
  const int barY = 54;
  const int barWidth = 128;
  const int barHeight = 8;
  display.drawRect(barX, barY, barWidth, barHeight, SSD1306_WHITE);
  int innerWidth = barWidth - 2;
  int fillWidth = static_cast<int>((status.progress / 100.0f) * innerWidth + 0.5f);
  if (fillWidth < 0) fillWidth = 0;
  if (fillWidth > innerWidth) fillWidth = innerWidth;
  display.fillRect(barX + 1, barY + 1, fillWidth, barHeight - 2, SSD1306_WHITE);

  display.display();
}

void drawProvisioningScreen(const char* apSsid, const IPAddress& apIp) {
  display.clearDisplay();
  display.setTextWrap(false);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Setup Mode");

  drawQrCode(0, 11);
  display.drawRect(0, 11, 58, 52, SSD1306_WHITE);

  display.setCursor(64, 0);
  display.println("1 Join AP");
  printWrappedValue(apSsid ? apSsid : "BambuStatus", 64, 10, 10, 10);
  display.setCursor(64, 34);
  display.println("2 Open / Scan");
  display.setCursor(64, 44);
  display.println(apIp.toString());
  display.setCursor(64, 54);
  display.println("192.168.4.1");

  display.display();
}
