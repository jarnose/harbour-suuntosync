// Qt-free golden-vector test for Logbook::decode, same discipline as the
// other tests in this directory: validated with plain g++ against a real
// captured workout before this ever touches BLE/Qt/WorkoutStore code.
//
// Expected values were computed by an independent Python re-implementation
// of the exact same algorithm against the same fixture (see
// docs/logbook-data-format.md for the research this algorithm is built on)
// - this test exists to catch the C++ port drifting from that algorithm,
// not to re-validate the algorithm itself against reality (that's already
// done in the docs, against Jarno's real FIT exports).
//
// Build: see test_sbemcontainer.cpp's header for the two-step gcc/g++ build
// (heatshrink_decoder.c needs a C compiler, the rest is C++).

#include "../src/ble/logbookdecoder.h"

#include <cmath>
#include <cstdio>
#include <fstream>

namespace {

int g_failures = 0;

void checkInt(int64_t actual, int64_t expected, const char *what)
{
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s - got %lld, expected %lld\n", what,
            static_cast<long long>(actual), static_cast<long long>(expected));
        g_failures++;
    } else {
        std::printf("ok: %s = %lld\n", what, static_cast<long long>(actual));
    }
}

void checkDouble(double actual, double expected, double tolerance, const char *what)
{
    if (std::fabs(actual - expected) > tolerance) {
        std::fprintf(stderr, "FAIL: %s - got %f, expected %f (tolerance %f)\n", what, actual, expected, tolerance);
        g_failures++;
    } else {
        std::printf("ok: %s = %f\n", what, actual);
    }
}

std::vector<uint8_t> readFile(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot open " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

int main()
{
    const std::vector<uint8_t> compressed = readFile("fixtures/logbook_data_heatshrink.bin");
    const Logbook::DecodedWorkout w = Logbook::decode(compressed);

    // Reference values from an independent Python re-implementation of this
    // same algorithm against the same fixture (see this file's header).
    checkInt(w.activityId, 12, "activityId");
    checkInt(static_cast<int64_t>(w.startTimeMs), 1788194034000LL, "startTimeMs");
    checkInt(static_cast<int64_t>(w.stopTimeMs), 1788195586990LL, "stopTimeMs");
    checkDouble(w.totalTimeSeconds, 1456.000, 0.01, "totalTimeSeconds");
    checkDouble(w.totalDistanceMeters, 2086.292, 0.5, "totalDistanceMeters");
    checkDouble(w.maxSpeedMs, 2.224, 0.01, "maxSpeedMs");
    checkDouble(w.avgHeartRateBpm, 86.720, 0.01, "avgHeartRateBpm");
    checkDouble(w.maxHeartRateBpm, 99, 0.01, "maxHeartRateBpm");
    checkInt(w.stepCount, 2731, "stepCount");

    // Sanity cross-check against reality (not just the Python port): this
    // fixture is the real walking workout documented in
    // docs/logbook-data-format.md - Jarno's app reported 2672 real steps,
    // 24:53 (1493s) active duration, 99 bpm max HR, 2094m distance. The
    // algorithm above is already known (and documented) to land within a
    // few percent of these, not exactly on them - see the "Ground-truth
    // calibration"/"Steps" sections of the doc for why.
    checkDouble(w.stepCount, 2672, 2672 * 0.05, "stepCount within 5% of the real 2672 steps");
    checkDouble(w.totalTimeSeconds, 1493, 1493 * 0.05, "totalTimeSeconds within 5% of the real 1493s");
    checkDouble(w.totalDistanceMeters, 2094, 2094 * 0.02, "totalDistanceMeters within 2% of the real 2094m");
    checkDouble(w.maxHeartRateBpm, 99, 0.5, "maxHeartRateBpm exactly matches the real 99 bpm");

    if (g_failures == 0) {
        std::printf("\nAll assertions passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d assertion(s) failed.\n", g_failures);
    return 1;
}
