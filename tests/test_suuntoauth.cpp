// Standalone, Qt-free test harness for SuuntoAuth, run with plain g++:
//   g++ -std=c++17 ../src/cloud/suuntoauth.cpp test_suuntoauth.cpp -o /tmp/test_suuntoauth && /tmp/test_suuntoauth
//
// Every expected value below is a golden vector copied verbatim from
// tajchert/suuntool (MIT), internal/auth/{obfuscator,signer,totp}_test.go -
// an independent, already-working reverse engineering of the Suunto/Sports-
// Tracker cloud API's login/request-signing scheme. SuuntoAuth is a
// straight C++ port of that package's algorithms (SHA-1/SHA-256/HMAC/
// PBKDF2/base64, all hand-implemented here since this sandbox has neither
// Qt nor OpenSSL dev headers) - passing these vectors means the port is
// byte-exact before it's ever wired into a real login attempt.
#include "../src/cloud/suuntoauth.h"

#include <cstdio>
#include <string>

namespace {
int g_failures = 0;

std::string toHex(const std::string &s)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0xF]);
    }
    return out;
}

std::string toHex(const std::vector<uint8_t> &s)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    for (uint8_t c : s) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0xF]);
    }
    return out;
}

void expectEq(const std::string &name, const std::string &actual, const std::string &expected)
{
    if (actual == expected) {
        std::printf("PASS %s\n", name.c_str());
    } else {
        std::printf("FAIL %s\n  expected: %s\n  actual:   %s\n", name.c_str(), expected.c_str(),
                     actual.c_str());
        ++g_failures;
    }
}
} // namespace

int main()
{
    // Golden vector from tajchert/suuntool internal/auth/obfuscator_test.go
    expectEq("DeriveLoginSecret()", toHex(SuuntoAuth::deriveLoginSecret()),
              "77764342455243417a3730794252723964531c7e2b5803454d394c5564065765451d774e5c4b666f"
              "2059584252402d002e01efbfbdefbfbdefbfbd5f12633667570c5e7a2e505515245a2d4d204a230c"
              "577f6e253437050c57791376");

    // Golden vector from signer_test.go
    expectEq("SignParams(login, foo@bar.com/Pass123)",
              SuuntoAuth::signParams("login", { { "l", "foo@bar.com" }, { "p", "Pass123" } }),
              "Sf8mC1rPA6rrZh0uHpdwh-TkSlQLO0hkKs4S_6vdBqo");

    // Golden vector from totp_test.go
    expectEq("pbkdf2KeyForSalt(alice@example.com)",
              toHex(SuuntoAuth::pbkdf2KeyForSalt("alice@example.com")),
              "15d70cdb1125776ce85e91af922c2ba6db8917d994bf2a283fc5b249acf72e8e");

    // Same golden vector, via generateTotp: counter 1000000 == nowMs/30000, so
    // nowMs = 1000000 * 30000 lands exactly on that counter value.
    expectEq("generateTotp(alice@example.com, counter=1000000)",
              SuuntoAuth::generateTotp("alice@example.com", 1000000LL * 30000LL), "215251");

    if (g_failures == 0) {
        std::printf("\nAll tests passed.\n");
        return 0;
    }
    std::printf("\n%d test(s) FAILED.\n", g_failures);
    return 1;
}
