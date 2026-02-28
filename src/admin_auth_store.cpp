#include "admin_auth_store.h"

#include <ctype.h>
#include <esp_random.h>
#include <nvs.h>
#include <string.h>

#include "nvs_config_store.h"

namespace {
constexpr const char* kNamespace = "auth";
constexpr const char* kKeyAdminUser = "admin_user";
constexpr const char* kKeyAdminPass = "admin_pass";
constexpr char kPasswordAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

bool containsOnlyAdminTokenChars(const char* value) {
  if (!value) {
    return false;
  }
  for (size_t i = 0; value[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (!(isalnum(c) || c == '-' || c == '_')) {
      return false;
    }
  }
  return true;
}

bool validateAdminUsername(const char* username) {
  if (!username) {
    return false;
  }
  const size_t len = strlen(username);
  return len > 0 && len <= ADMIN_USERNAME_MAX_LEN && containsOnlyAdminTokenChars(username);
}

bool readString(nvs_handle_t handle, const char* key, char* dst, size_t dstLen) {
  size_t needed = dstLen;
  const esp_err_t err = nvs_get_str(handle, key, dst, &needed);
  if (err != ESP_OK || needed == 0 || needed > dstLen) {
    return false;
  }
  return true;
}

void generateAdminPassword(char* password, size_t passwordLen) {
  if (!password || passwordLen == 0) {
    return;
  }
  memset(password, 0, passwordLen);
  constexpr size_t alphabetLen = sizeof(kPasswordAlphabet) - 1;
  for (size_t i = 0; i < ADMIN_PASSWORD_GENERATED_LEN && i + 1 < passwordLen; ++i) {
    password[i] = kPasswordAlphabet[esp_random() % alphabetLen];
  }
  password[ADMIN_PASSWORD_GENERATED_LEN] = '\0';
}

bool saveAdminCredentialsInternal(const AdminCredentials& credentials) {
  if (!initConfigStore()) {
    return false;
  }
  if (!validateAdminUsername(credentials.username) || !validateAdminPassword(credentials.password)) {
    return false;
  }

  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return false;
  }

  err = nvs_set_str(handle, kKeyAdminUser, credentials.username);
  if (err == ESP_OK) {
    err = nvs_set_str(handle, kKeyAdminPass, credentials.password);
  }
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  if (err != ESP_OK) {
    return false;
  }

  AdminCredentials verify{};
  const bool ok = loadAdminCredentials(&verify) && strcmp(verify.username, credentials.username) == 0 &&
                  strcmp(verify.password, credentials.password) == 0;
  clearAdminCredentialsStruct(&verify);
  return ok;
}
}  // namespace

bool validateAdminPassword(const char* password) {
  if (!password) {
    return false;
  }
  const size_t len = strlen(password);
  return len >= ADMIN_PASSWORD_MIN_LEN && len <= ADMIN_PASSWORD_MAX_LEN && containsOnlyAdminTokenChars(password);
}

bool loadAdminCredentials(AdminCredentials* outCredentials) {
  if (!outCredentials || !initConfigStore()) {
    return false;
  }
  clearAdminCredentialsStruct(outCredentials);

  nvs_handle_t handle = 0;
  const esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    return false;
  }

  const bool ok = readString(handle, kKeyAdminUser, outCredentials->username, sizeof(outCredentials->username)) &&
                  readString(handle, kKeyAdminPass, outCredentials->password, sizeof(outCredentials->password)) &&
                  validateAdminUsername(outCredentials->username) && validateAdminPassword(outCredentials->password);
  nvs_close(handle);

  if (!ok) {
    clearAdminCredentialsStruct(outCredentials);
  }
  return ok;
}

bool ensureAdminCredentials(AdminCredentials* outCredentials, bool* outGenerated) {
  if (outGenerated) {
    *outGenerated = false;
  }
  if (loadAdminCredentials(outCredentials)) {
    return true;
  }

  AdminCredentials generated{};
  strlcpy(generated.username, DEFAULT_ADMIN_USERNAME, sizeof(generated.username));
  generateAdminPassword(generated.password, sizeof(generated.password));
  if (!saveAdminCredentialsInternal(generated)) {
    clearAdminCredentialsStruct(&generated);
    return false;
  }

  if (outCredentials) {
    *outCredentials = generated;
  }
  if (outGenerated) {
    *outGenerated = true;
  }
  clearAdminCredentialsStruct(&generated);
  return true;
}

bool updateAdminPassword(const char* newPassword) {
  if (!validateAdminPassword(newPassword)) {
    return false;
  }

  AdminCredentials credentials{};
  strlcpy(credentials.username, DEFAULT_ADMIN_USERNAME, sizeof(credentials.username));
  strlcpy(credentials.password, newPassword, sizeof(credentials.password));
  const bool ok = saveAdminCredentialsInternal(credentials);
  clearAdminCredentialsStruct(&credentials);
  return ok;
}

bool clearAdminCredentials() {
  if (!initConfigStore()) {
    return false;
  }

  nvs_handle_t handle = 0;
  const esp_err_t openErr = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (openErr == ESP_ERR_NVS_NOT_FOUND) {
    return true;
  }
  if (openErr != ESP_OK) {
    return false;
  }

  bool ok = true;
  esp_err_t err = nvs_erase_key(handle, kKeyAdminUser);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    ok = false;
  }
  err = nvs_erase_key(handle, kKeyAdminPass);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    ok = false;
  }
  if (ok) {
    err = nvs_commit(handle);
    ok = (err == ESP_OK);
  }
  nvs_close(handle);
  return ok;
}
