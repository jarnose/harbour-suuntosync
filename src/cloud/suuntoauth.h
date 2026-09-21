#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

// Login/request-signing for the Suunto cloud account (Sports-Tracker
// backend, host api.sports-tracker.com) - a from-scratch C++ port of
// tajchert/suuntool's internal/auth Go package (MIT), which independently
// reverse-engineered this scheme from com.stt.android.suunto v6.8.13 (the
// APK on disk here is v6.7.12 - close enough to very likely still be
// current, unconfirmed until a real login succeeds). Deliberately Qt-free
// (STL only, own SHA-1/SHA-256/HMAC/PBKDF2/base64): this sandbox has neither
// Qt nor OpenSSL dev headers, so hand-rolling was the only way to validate
// this against real golden vectors (see tests/test_suuntoauth.cpp, 4/4
// passing) before it ever touches a real account's credentials.
//
// What each piece is for:
// - deriveLoginSecret()/signParams(): every request to a mutating or
//   authenticated endpoint is signed - SHA256("POST&" + path + "&k=v..." +
//   "&secret=" + secret), base64url, where `secret` is an obfuscated
//   constant baked into the official app (deriveLoginSecret() recovers it).
// - generateTotp(): /login2 itself requires a TOTP alongside the password -
//   not a user-facing 2FA code, just another app-embedded secret used as a
//   PBKDF2 seed (deriveTotpMasterSecret()), keyed by the account email.
namespace SuuntoAuth {

std::vector<uint8_t> sha1(const uint8_t *data, size_t len);
std::vector<uint8_t> sha256(const uint8_t *data, size_t len);
std::vector<uint8_t> hmacSha1(const std::vector<uint8_t> &key, const uint8_t *data, size_t len);
std::vector<uint8_t> pbkdf2HmacSha1(const std::vector<uint8_t> &password,
                                     const std::vector<uint8_t> &salt, uint32_t iterations,
                                     size_t dkLen);

std::string base64Encode(const std::vector<uint8_t> &data);
std::vector<uint8_t> base64Decode(const std::string &data);
std::string base64UrlNoPad(const std::vector<uint8_t> &data);

// Re-encodes bytes as UTF-8, replacing any invalid byte with U+FFFD (EF BF
// BD) one byte at a time - matches Go's unicode/utf8.DecodeRune behavior
// exactly (this in turn ports com.stt.android's Java `new String(bytes,
// UTF_8)` semantics), which the obfuscation pipeline depends on bit-for-bit.
std::string utf8Replace(const std::vector<uint8_t> &bytes);

std::string deriveLoginSecret();
std::string deriveTotpMasterSecret();

// Exposed for direct golden-vector testing (mirrors the pbkdf2KeyForSalt Go
// test in tajchert/suuntool's internal/auth package) - the per-account TOTP
// key derived from deriveTotpMasterSecret() and the account email/username.
std::vector<uint8_t> pbkdf2KeyForSalt(const std::string &salt);

using Param = std::pair<std::string, std::string>;
// SHA256("POST&" + path + "&k=v&k=v...&secret=" + DeriveLoginSecret()), base64url no padding.
std::string signParams(const std::string &path, const std::vector<Param> &params);

// RFC 6238 TOTP, 6 digits, 30s step, PBKDF2(HMAC-SHA1, 100 iters, 32-byte
// key)-derived secret keyed by `salt` (the account email/username).
// nowMs is the caller-supplied current time in unix milliseconds (kept as a
// parameter rather than reading the clock internally, so this stays
// deterministic/testable).
std::string generateTotp(const std::string &salt, int64_t nowMs);

} // namespace SuuntoAuth
