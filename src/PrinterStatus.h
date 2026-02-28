#pragma once

#include <Arduino.h>

struct PrinterStatus {
  String gcodeState;
  float progress;
  bool isPrinting;
  bool hasError;
  String statusText;
  float nozzleTemp;
  float nozzleTarget;
  float bedTemp;
  float bedTarget;
  int32_t layerNum;
  int32_t totalLayerNum;
  bool hasLayerInfo;
  bool printerLightOn;
  bool wifiConnected;
  bool mqttConnected;
  uint32_t lastUpdateMs;
  bool hasProgressData;
  uint32_t lastPrintActiveMs;
  int mcRemainingTimeSeconds;
  int wifiSignalDbm;
  int32_t printErrorCode;
  int mcPrintStage;
  int stgCur;

  PrinterStatus()
      : gcodeState("UNKNOWN"),
        progress(0.0f),
        isPrinting(false),
        hasError(false),
        statusText("Disconnected"),
        nozzleTemp(NAN),
        nozzleTarget(NAN),
        bedTemp(NAN),
        bedTarget(NAN),
        layerNum(-1),
        totalLayerNum(-1),
        hasLayerInfo(false),
        printerLightOn(false),
        wifiConnected(false),
        mqttConnected(false),
        lastUpdateMs(0),
        hasProgressData(false),
        lastPrintActiveMs(0),
        mcRemainingTimeSeconds(-1),
        wifiSignalDbm(0),
        printErrorCode(0),
        mcPrintStage(-1),
        stgCur(-1) {}
};

inline bool isValidTemp(float value) { return !isnan(value); }
