#include "Display.h"

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

static Adafruit_SSD1306 display(128, 64, &Wire, OLED_RESET_PIN);

static void drawWifiScreen() {
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

static void drawCheckmark(int x, int y, int size) {
  display.drawLine(x, y + size / 2, x + size / 3, y + size, SSD1306_WHITE);
  display.drawLine(x + size / 3, y + size, x + size, y, SSD1306_WHITE);
}

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

static String formatTemp(float value) {
  if (!isValidTemp(value)) {
    return String("--");
  }
  int rounded = static_cast<int>(roundf(value));
  return String(rounded);
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

void drawProvisioningScreen(const char* apSsid) {
  display.clearDisplay();
  display.setTextWrap(false);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Bambu Status");
  display.setTextSize(1);
  display.setCursor(0, 14);
  display.println("Provisioning Mode");
  display.setCursor(0, 28);
  display.println("Connect AP:");
  display.setCursor(0, 38);
  display.println(apSsid ? apSsid : "BambuStatus");
  display.setCursor(0, 52);
  display.println("Open 192.168.4.1");
  display.display();
}
