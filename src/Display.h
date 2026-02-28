#pragma once

#include <Adafruit_SSD1306.h>

#include "PrinterStatus.h"
#include "config.h"

bool initDisplay();
void drawStatus(const PrinterStatus& status);
void drawProvisioningScreen(const char* apSsid, const IPAddress& apIp);
void drawAdminPasswordScreen(const char* username, const char* password);
