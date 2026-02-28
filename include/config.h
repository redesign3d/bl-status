#pragma once

#include <Arduino.h>

// Runtime configuration schema and limits.
constexpr uint16_t CONFIG_SCHEMA_VERSION = 1;
constexpr size_t WIFI_SSID_MAX_LEN = 32;
constexpr size_t WIFI_PASSWORD_MAX_LEN = 63;
constexpr size_t PRINTER_HOST_MAX_LEN = 63;
constexpr size_t PRINTER_SERIAL_MAX_LEN = 31;
constexpr size_t MQTT_USERNAME_MAX_LEN = 31;
constexpr size_t ACCESS_CODE_MAX_LEN = 63;

constexpr uint16_t MIN_MQTT_PORT = 1;
constexpr uint16_t MAX_MQTT_PORT = 65535;
constexpr uint16_t DEFAULT_MQTT_TLS_PORT = 8883;

constexpr bool ENABLE_LED_SYNC = true;  // Increase idle brightness when printer light is on.

// Provisioning mode constants.
constexpr uint32_t PROVISIONING_AP_TIMEOUT_MS = 15UL * 60UL * 1000UL;  // 15 minutes.
constexpr uint32_t PROVISIONING_PAIRING_TTL_MS = 5UL * 60UL * 1000UL;  // 5 minutes.
constexpr uint32_t PROVISIONING_SESSION_TTL_MS = 10UL * 60UL * 1000UL;  // 10 minutes.
constexpr uint32_t PROVISIONING_REBOOT_DELAY_MS = 1500UL;
constexpr uint16_t PROVISIONING_HTTP_PORT = 80;
constexpr uint16_t PROVISIONING_DNS_PORT = 53;
constexpr size_t PROVISIONING_HTTP_MAX_BODY_BYTES = 1024;
constexpr uint16_t PROVISIONING_RATE_LIMIT_WINDOW_MS = 60 * 1000;
constexpr uint8_t PROVISIONING_RATE_LIMIT_REQUESTS = 40;

// Connectivity recovery: fall back into provisioning without erasing saved config.
constexpr uint32_t WIFI_FAILURE_CHECK_INTERVAL_MS = 10UL * 1000UL;
constexpr uint16_t WIFI_FAILURE_THRESHOLD_BASE = 18;  // ~3 minutes at 10s checks.
constexpr uint16_t WIFI_FAILURE_THRESHOLD_MAX = 72;

// Optional deterministic local validation test harness.
constexpr bool RUN_VALIDATION_SELF_TEST_ON_BOOT = false;

// Non-secret printer TLS defaults.
constexpr bool DEFAULT_TLS_INSECURE = false;
// Optional: PEM-encoded printer root certificate. A build script will look for a certificate
// file and generate include/generated_printer_cert.h automatically (default Mac path:
// /Applications/BambuStudio.app/Contents/Resources/cert/printer.cer). If no cert is found,
// we fall back to insecure TLS (LAN-only).
#ifndef BAMBU_PRINTER_ROOT_CA_HEADER
#define BAMBU_PRINTER_ROOT_CA_HEADER "generated_printer_cert.h"
#endif

#if defined(__has_include)
#if __has_include(BAMBU_PRINTER_ROOT_CA_HEADER)
#include BAMBU_PRINTER_ROOT_CA_HEADER
#define BAMBU_CERT_INCLUDED 1
#endif
#endif

#ifndef BAMBU_CERT_INCLUDED
constexpr const char* BAMBU_PRINTER_ROOT_CA = "";
#endif

// MQTT topic helper
constexpr const char* BAMBU_REPORT_TOPIC_PREFIX = "device/";
constexpr const char* BAMBU_REPORT_TOPIC_SUFFIX = "/report";

// Hardware configuration
constexpr uint8_t LED_PIN = 13;
constexpr uint16_t NUM_LEDS = 15;
constexpr uint8_t OLED_SDA_PIN = 21;
constexpr uint8_t OLED_SCL_PIN = 22;
constexpr uint8_t OLED_RESET_PIN = -1;  // Shared reset, not wired
constexpr uint8_t OLED_I2C_ADDRESS = 0x3C;

// Timing thresholds
constexpr uint32_t STATUS_STALE_TIMEOUT_MS = 60000;  // 60 seconds keep-last window
constexpr uint32_t FINISH_RECENT_WINDOW_MS = 300000;  // 5 minutes for finish recognition
