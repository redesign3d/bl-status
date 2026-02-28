#pragma once

#include <Arduino.h>

#include "config.h"

struct AdminCredentials {
  char username[ADMIN_USERNAME_MAX_LEN + 1];
  char password[ADMIN_PASSWORD_MAX_LEN + 1];
};

inline void clearAdminCredentialsStruct(AdminCredentials* credentials) {
  if (!credentials) {
    return;
  }
  memset(credentials, 0, sizeof(AdminCredentials));
}

bool loadAdminCredentials(AdminCredentials* outCredentials);
bool ensureAdminCredentials(AdminCredentials* outCredentials, bool* outGenerated);
bool updateAdminPassword(const char* newPassword);
bool clearAdminCredentials();
bool validateAdminPassword(const char* password);
