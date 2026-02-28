#pragma once

#include <Adafruit_NeoPixel.h>

#include "PrinterStatus.h"
#include "config.h"

void initLeds();
void updateLeds(const PrinterStatus& status, uint32_t nowMs);
