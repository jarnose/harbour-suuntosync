// Qt-free test for the ISO 8601 parser/formatter.
//
//   g++ -std=c++17 ../src/cloud/iso8601.cpp test_iso8601.cpp -o /tmp/test_iso8601 \
//       && /tmp/test_iso8601
//
// The load-bearing cases are the exact strings from the real capture
// (tests/fixtures/cloud_247_*.json and cloud_sml_samples.json), with their
// expected epoch values computed independently - by Python's datetime, not
// by this implementation.

#include "../src/cloud/iso8601.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void check(bool ok, const std::string &what)
{
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok)
        ++g_failures;
}

void checkParse(const std::string &text, int64_t expectedMs)
{
    int64_t got = 0;
    const bool ok = Iso8601::parseToUnixMs(text, &got);
    if (ok && got == expectedMs) {
        std::printf("ok: %s -> %lld\n", text.c_str(), static_cast<long long>(got));
    } else {
        std::printf("FAIL: %s -> %lld (ok=%d), expected %lld\n", text.c_str(),
                     static_cast<long long>(got), ok ? 1 : 0,
                     static_cast<long long>(expectedMs));
        ++g_failures;
    }
}

void checkRejected(const std::string &text)
{
    int64_t got = 0;
    check(!Iso8601::parseToUnixMs(text, &got), "rejected: \"" + text + "\"");
}

void testRealCapturedStrings()
{
    // From cloud_247_v1_sleep.json - the night that prompted this whole
    // detour. Expected value from Python:
    //   datetime.fromisoformat("2026-09-21T22:54:00.000+03:00").timestamp()*1000
    checkParse("2026-09-21T22:54:00.000+03:00", 1790020440000LL);
    // cloud_247_v1_sleepstages.json uses a +00:00 offset on the same night.
    checkParse("2026-09-21T19:54:00.000+00:00", 1790020440000LL);
    // cloud_sml_samples.json, with milliseconds that are not zero.
    checkParse("2026-09-22T15:20:28.430+03:00", 1790079628430LL);
    // cloud_247_v1_activity.json.
    checkParse("2026-09-22T21:30:00.000+03:00", 1790101800000LL);
}

void testOptionalParts()
{
    // No fractional part, and a "Z" zone.
    checkParse("2026-09-21T19:54:00Z", 1790020440000LL);
    // Offset without the colon.
    checkParse("2026-09-21T22:54:00+0300", 1790020440000LL);
    // More than three fractional digits: extra precision is dropped, not
    // an error, and must not leak into the milliseconds.
    checkParse("2026-09-21T19:54:00.123456Z", 1790020440123LL);
    // A single fractional digit means tenths, not thousandths.
    checkParse("2026-09-21T19:54:00.5Z", 1790020440500LL);
    // No zone at all is treated as UTC.
    checkParse("2026-09-21T19:54:00", 1790020440000LL);
}

void testMalformedIsRejectedRatherThanGuessed()
{
    checkRejected("");
    checkRejected("2026-09-21");
    checkRejected("2026-09-21T19:54");
    checkRejected("not a timestamp at all");
    checkRejected("2026-13-21T19:54:00Z");       // month 13
    checkRejected("2026-09-21T25:54:00Z");       // hour 25
    checkRejected("2026-09-21T19:54:00+03:00x"); // trailing junk
    checkRejected("2026-09-21T19:54:00+3:00");   // one-digit offset hour
}

void testRoundTrip()
{
    struct Case { int64_t ms; int offset; const char *expected; };
    const Case cases[] = {
        { 1790020440000LL, 180, "2026-09-21T22:54:00.000+03:00" },
        { 1790020440000LL, 0,   "2026-09-21T19:54:00.000+00:00" },
        { 1790079628430LL, 180, "2026-09-22T15:20:28.430+03:00" },
        { 1790020440000LL, -300, "2026-09-21T14:54:00.000-05:00" },
    };
    for (const Case &c : cases) {
        const std::string got = Iso8601::formatLocal(c.ms, c.offset);
        check(got == c.expected, "format " + std::to_string(c.ms) + " @"
                                   + std::to_string(c.offset) + " -> " + got);
        // And back again: whatever offset was used, the instant is the same.
        int64_t back = 0;
        check(Iso8601::parseToUnixMs(got, &back) && back == c.ms,
               "round-trips: " + got);
    }
}

} // namespace

int main()
{
    testRealCapturedStrings();
    testOptionalParts();
    testMalformedIsRejectedRatherThanGuessed();
    testRoundTrip();

    if (g_failures == 0) {
        std::printf("\nAll assertions passed.\n");
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED.\n", g_failures);
    return 1;
}
