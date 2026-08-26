#pragma once

#include <cstddef>

namespace management_auth {

enum class LoginResult : unsigned char {
    Authenticated = 0,
    PasswordNotSet,
    InvalidPassword,
    RateLimited,
};

enum class PasswordResult : unsigned char {
    Success = 0,
    Invalid,
    AlreadySet,
    CurrentPasswordIncorrect,
    StorageFailure,
};

void begin();
bool clearPassword();
bool passwordSet();
bool authenticated(const char *cookieHeader);
LoginResult login(const char *password);
void logout();
const char *sessionToken();
PasswordResult createPassword(const char *newPassword);
PasswordResult changePassword(
    const char *currentPassword,
    const char *newPassword);

}  // namespace management_auth
