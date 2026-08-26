#include "management_auth.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstring>
#include <esp_system.h>
#include <mbedtls/sha256.h>

namespace management_auth {
namespace {

constexpr size_t kSaltLength = 16;
constexpr size_t kHashLength = 32;
constexpr size_t kMaximumPasswordLength = 64;
constexpr size_t kSessionBytes = 32;
constexpr size_t kSessionTextLength = kSessionBytes * 2;
constexpr uint32_t kFailedLoginDelayMs = 1000;

struct PasswordRecord {
    uint8_t salt[kSaltLength];
    uint8_t hash[kHashLength];
};

Preferences preferences;
PasswordRecord record{};
bool recordPresent = false;
char sessionText[kSessionTextLength + 1]{};
uint32_t nextLoginAllowedAt = 0;

bool validPassword(const char *password) {
    if (password == nullptr) return false;
    const size_t length = strlen(password);
    return length != 0 && length <= kMaximumPasswordLength;
}

bool hashPassword(
    const uint8_t *salt,
    const char *password,
    uint8_t *hash) {
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
#if MBEDTLS_VERSION_MAJOR >= 3
    const bool started = mbedtls_sha256_starts(&context, 0) == 0;
    const bool saltUpdated = started &&
        mbedtls_sha256_update(&context, salt, kSaltLength) == 0;
    const bool passwordUpdated = saltUpdated &&
        mbedtls_sha256_update(
            &context,
            reinterpret_cast<const unsigned char *>(password),
            strlen(password)) == 0;
    const bool finished = passwordUpdated &&
        mbedtls_sha256_finish(&context, hash) == 0;
#else
    const bool started = mbedtls_sha256_starts_ret(&context, 0) == 0;
    const bool saltUpdated = started &&
        mbedtls_sha256_update_ret(&context, salt, kSaltLength) == 0;
    const bool passwordUpdated = saltUpdated &&
        mbedtls_sha256_update_ret(
            &context,
            reinterpret_cast<const unsigned char *>(password),
            strlen(password)) == 0;
    const bool finished = passwordUpdated &&
        mbedtls_sha256_finish_ret(&context, hash) == 0;
#endif
    mbedtls_sha256_free(&context);
    return finished;
}

bool constantTimeEqual(const uint8_t *left, const uint8_t *right, size_t length) {
    uint8_t difference = 0;
    for (size_t index = 0; index < length; ++index) {
        difference |= static_cast<uint8_t>(left[index] ^ right[index]);
    }
    return difference == 0;
}

bool passwordMatches(const char *password) {
    if (!recordPresent || !validPassword(password)) return false;
    uint8_t candidate[kHashLength];
    if (!hashPassword(record.salt, password, candidate)) return false;
    return constantTimeEqual(record.hash, candidate, sizeof(candidate));
}

bool persistRecord(const PasswordRecord &next) {
    if (!preferences.begin("auth", false)) return false;
    const size_t written = preferences.putBytes("record", &next, sizeof(next));
    preferences.end();
    return written == sizeof(next);
}

void generateSession() {
    constexpr char hex[] = "0123456789abcdef";
    for (size_t index = 0; index < kSessionBytes; ++index) {
        const uint8_t value = static_cast<uint8_t>(esp_random());
        sessionText[index * 2] = hex[value >> 4];
        sessionText[index * 2 + 1] = hex[value & 0x0F];
    }
    sessionText[kSessionTextLength] = '\0';
}

bool cookieMatchesSession(const char *cookieHeader) {
    if (!recordPresent || sessionText[0] == '\0' || cookieHeader == nullptr) {
        return false;
    }

    const char *cursor = cookieHeader;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == ';') ++cursor;
        if (*cursor == '\0') break;
        const char *value = strstr(cursor, "s2w_session=");
        if (value == nullptr) return false;
        if (value != cursor) {
            const char preceding = value[-1];
            if (preceding != ' ' && preceding != ';') return false;
        }
        value += strlen("s2w_session=");
        size_t length = 0;
        while (value[length] != '\0' && value[length] != ';' &&
                value[length] != ' ') {
            ++length;
        }
        if (length != kSessionTextLength) return false;
        uint8_t difference = 0;
        for (size_t index = 0; index < kSessionTextLength; ++index) {
            difference |= static_cast<uint8_t>(
                value[index] ^ sessionText[index]);
        }
        return difference == 0;
    }
    return false;
}

PasswordResult writeNewPassword(const char *newPassword) {
    if (!validPassword(newPassword)) return PasswordResult::Invalid;

    PasswordRecord next{};
    for (size_t index = 0; index < sizeof(next.salt); index += sizeof(uint32_t)) {
        const uint32_t random = esp_random();
        const size_t remaining = sizeof(next.salt) - index;
        memcpy(next.salt + index, &random, min(remaining, sizeof(random)));
    }
    if (!hashPassword(next.salt, newPassword, next.hash)) {
        return PasswordResult::StorageFailure;
    }
    if (!persistRecord(next)) return PasswordResult::StorageFailure;
    record = next;
    recordPresent = true;
    logout();
    return PasswordResult::Success;
}

}  // namespace

void begin() {
    record = {};
    recordPresent = false;
    sessionText[0] = '\0';
    nextLoginAllowedAt = 0;

    if (!preferences.begin("auth", true)) return;
    const size_t length = preferences.getBytesLength("record");
    if (length == sizeof(record) &&
            preferences.getBytes("record", &record, sizeof(record)) == sizeof(record)) {
        recordPresent = true;
    }
    preferences.end();
}

bool clearPassword() {
    if (!preferences.begin("auth", false)) return false;
    const bool cleared = preferences.clear();
    preferences.end();
    if (!cleared) return false;

    record = {};
    recordPresent = false;
    logout();
    nextLoginAllowedAt = 0;
    return true;
}

bool passwordSet() {
    return recordPresent;
}

bool authenticated(const char *cookieHeader) {
    return cookieMatchesSession(cookieHeader);
}

LoginResult login(const char *password) {
    if (!recordPresent) return LoginResult::PasswordNotSet;
    const uint32_t now = millis();
    if (nextLoginAllowedAt != 0 &&
            static_cast<int32_t>(now - nextLoginAllowedAt) < 0) {
        return LoginResult::RateLimited;
    }
    if (!passwordMatches(password)) {
        nextLoginAllowedAt = now + kFailedLoginDelayMs;
        return LoginResult::InvalidPassword;
    }
    nextLoginAllowedAt = 0;
    generateSession();
    return LoginResult::Authenticated;
}

void logout() {
    sessionText[0] = '\0';
}

const char *sessionToken() {
    return sessionText;
}

PasswordResult createPassword(const char *newPassword) {
    if (recordPresent) return PasswordResult::AlreadySet;
    return writeNewPassword(newPassword);
}

PasswordResult changePassword(
    const char *currentPassword,
    const char *newPassword) {
    if (!recordPresent) return PasswordResult::AlreadySet;
    if (!passwordMatches(currentPassword)) {
        return PasswordResult::CurrentPasswordIncorrect;
    }
    return writeNewPassword(newPassword);
}

}  // namespace management_auth
