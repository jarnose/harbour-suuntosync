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

void testWalkingFixture()
{
    std::printf("--- walking fixture ---\n");
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

    // Altitude (see logbookdecoder.h). This is the workout whose barometer
    // hadn't settled at the start: its raw first absolute sample reads
    // 315.2 m and the Altitude calibration event (chunk 0x04) carries a
    // -208 m offset. Without applying that to the pre-event samples the
    // range comes out 103.8-331.0 m and the descent ~229 m instead of ~18 m,
    // so these assertions are the regression test for that correction.
    checkInt(w.hasAltitude ? 1 : 0, 1, "hasAltitude");
    checkDouble(w.minAltitudeMeters, 103.8, 0.05, "minAltitudeMeters");
    checkDouble(w.maxAltitudeMeters, 123.0, 0.05, "maxAltitudeMeters");
    checkDouble(w.totalAscentMeters, 16.0, 0.05, "totalAscentMeters");
    checkDouble(w.totalDescentMeters, 18.4, 0.05, "totalDescentMeters");
    // Real app-reported ascent/descent for this workout: 15.4 / 18.7 m.
    checkDouble(w.totalAscentMeters, 15.4, 15.4 * 0.20, "totalAscentMeters within 20% of the real 15.4m");
    checkDouble(w.totalDescentMeters, 18.7, 18.7 * 0.20, "totalDescentMeters within 20% of the real 18.7m");
}

void testCyclingFixtureAgainstRealDeviceFetch()
{
    std::printf("--- cycling fixture ---\n");
    // Unlike the walking fixture above (a historical HCI capture replayed
    // through this project's own decode pipeline), this one's expected
    // values are what AppController::testLogbookFetch() actually reported
    // back from a real live BLE fetch against Jarno's Suunto Race
    // (2026-09-21): "OK - 57174 bytes compressed, activity=4,
    // duration=2140s, distance=5394m maxSpeed=9,6m/s avgHR=76 maxHR=97
    // steps=19448" - the first real end-to-end confirmation that
    // MdsWhiteboardClient::fetchLogbookData()'s simplified trigger
    // hypothesis actually works on hardware.
    //
    // That real result also caught two genuine bugs, both fixed here and
    // reflected in the expected values below rather than reproduced:
    //   - maxSpeed (9.6 m/s, i.e. 34.6 km/h - the real reported max was
    //     25.7 km/h) came from a single ~3s GPS gap covering an implausible
    //     distance (a position glitch, not real motion) dominating the
    //     naive point-to-point max - fixed by kMaxSpeedGapMaxMs.
    //   - steps (19448, nonsensical for a cycling workout with no cadence
    //     sensor paired) came from chunk 0x16 byte 10 reading the sentinel
    //     value 255 for every single sample (confirmed by direct
    //     inspection) and being integrated as if it were a real cadence -
    //     fixed by kNoCadenceSentinel.
    // duration/distance/avgHR/maxHR were already correct on the real
    // device (matching Jarno's real app-reported 38:19/5420m/77/97 to
    // within the tolerances documented in docs/logbook-data-format.md).
    const std::vector<uint8_t> compressed = readFile("fixtures/logbook_data_heatshrink_cycling.bin");
    checkInt(static_cast<int64_t>(compressed.size()), 57174, "compressed size matches the real device fetch");

    const Logbook::DecodedWorkout w = Logbook::decode(compressed);
    checkInt(w.activityId, 4, "activityId");
    checkDouble(w.totalTimeSeconds, 2140.000, 0.01, "totalTimeSeconds (matches the real device fetch exactly)");
    checkDouble(w.totalDistanceMeters, 5393.699, 0.5, "totalDistanceMeters (matches the real device fetch)");
    checkDouble(w.maxSpeedMs, 7.127, 0.01, "maxSpeedMs (fixed - was 9.6 on the real device before this fix)");
    checkDouble(w.avgHeartRateBpm, 75.700, 0.01, "avgHeartRateBpm (rounds to the real device's 76)");
    checkDouble(w.maxHeartRateBpm, 97, 0.01, "maxHeartRateBpm (matches the real device fetch exactly)");
    checkInt(w.stepCount, 0, "stepCount (fixed - was 19448 on the real device before this fix)");

    // Cross-check against Jarno's real reported app stats for this same
    // workout (38:19=2299s, 5420m, avg/max HR 77/97, max speed 25.7km/h) -
    // same tolerances as documented in docs/logbook-data-format.md.
    checkDouble(w.totalTimeSeconds, 2299, 2299 * 0.10, "totalTimeSeconds within 10% of the real 2299s (38:19)");
    checkDouble(w.totalDistanceMeters, 5420, 5420 * 0.02, "totalDistanceMeters within 2% of the real 5420m");
    checkDouble(w.maxSpeedMs, 25.7 / 3.6, 0.5, "maxSpeedMs close to the real 25.7 km/h max speed");

    // Altitude (see logbookdecoder.h) - this workout's calibration offset is
    // a mere -12 m, so unlike the walking fixture it barely moves.
    checkInt(w.hasAltitude ? 1 : 0, 1, "hasAltitude");
    checkDouble(w.minAltitudeMeters, 98.6, 0.05, "minAltitudeMeters");
    checkDouble(w.maxAltitudeMeters, 107.6, 0.05, "maxAltitudeMeters");
    checkDouble(w.totalAscentMeters, 18.0, 0.05, "totalAscentMeters");
    checkDouble(w.totalDescentMeters, 16.0, 0.05, "totalDescentMeters");
    // Real app-reported ascent/descent for this workout: 19.75 / 19.5 m.
    checkDouble(w.totalAscentMeters, 19.75, 19.75 * 0.20, "totalAscentMeters within 20% of the real 19.75m");
    checkDouble(w.totalDescentMeters, 19.5, 19.5 * 0.20, "totalDescentMeters within 20% of the real 19.5m");
}

int main()
{
    testWalkingFixture();
    testCyclingFixtureAgainstRealDeviceFetch();

    if (g_failures == 0) {
        std::printf("\nAll assertions passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d assertion(s) failed.\n", g_failures);
    return 1;
}
