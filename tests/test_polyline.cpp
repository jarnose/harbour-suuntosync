// Qt-free test for the encoded-polyline decoder.
//
//   g++ -std=c++17 ../src/cloud/polyline.cpp test_polyline.cpp -o /tmp/test_polyline \
//       && /tmp/test_polyline
//
// The first case is the format's own published example, which makes this a
// real conformance check rather than a test written against the
// implementation: "_p~iF~ps|U_ulLnnqC_mqNvxq`@" is documented to decode to
// (38.5, -120.2), (40.7, -120.95), (43.252, -126.453).

#include "../src/cloud/polyline.h"

#include <cmath>
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

void checkNear(double actual, double expected, double tolerance, const std::string &what)
{
    const bool ok = std::fabs(actual - expected) <= tolerance;
    if (ok)
        std::printf("ok: %s = %.6f\n", what.c_str(), actual);
    else
        std::printf("FAIL: %s = %.6f, expected %.6f\n", what.c_str(), actual, expected);
    if (!ok)
        ++g_failures;
}

void testPublishedExample()
{
    const auto p = Polyline::decode("_p~iF~ps|U_ulLnnqC_mqNvxq`@");
    check(p.size() == 3, "published example decodes to three points");
    if (p.size() != 3)
        return;
    checkNear(p[0].latitude, 38.5, 1e-9, "point 0 latitude");
    checkNear(p[0].longitude, -120.2, 1e-9, "point 0 longitude");
    checkNear(p[1].latitude, 40.7, 1e-9, "point 1 latitude");
    checkNear(p[1].longitude, -120.95, 1e-9, "point 1 longitude");
    checkNear(p[2].latitude, 43.252, 1e-9, "point 2 latitude");
    checkNear(p[2].longitude, -126.453, 1e-9, "point 2 longitude");
}

void testMalformedInputLosesTheRouteNotTheWorkout()
{
    check(Polyline::decode("").empty(), "empty input decodes to nothing");
    // A latitude with no longitude after it: truncated, so the whole thing
    // is rejected rather than half-decoded.
    check(Polyline::decode("_p~iF").empty(), "a truncated pair is rejected");
    // Bytes below the format's 63 offset can't appear in a valid polyline.
    check(Polyline::decode(std::string("\x01\x02", 2)).empty(), "out-of-range bytes are rejected");
}

void testPrecisionIsHonoured()
{
    const auto five = Polyline::decode("_p~iF~ps|U");
    const auto six = Polyline::decode("_p~iF~ps|U", 6);
    check(five.size() == 1 && six.size() == 1, "both precisions decode one point");
    if (five.size() == 1 && six.size() == 1)
        checkNear(six[0].latitude * 10, five[0].latitude, 1e-9,
                   "a 6-decimal decode is a tenth of the 5-decimal one");
}

} // namespace

int main()
{
    testPublishedExample();
    testMalformedInputLosesTheRouteNotTheWorkout();
    testPrecisionIsHonoured();

    if (g_failures == 0) {
        std::printf("\nAll assertions passed.\n");
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED.\n", g_failures);
    return 1;
}
