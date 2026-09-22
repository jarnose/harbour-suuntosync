// Standalone, Qt-free test harness for Summary::decode, run with plain g++
// (this environment has no Sailfish/Qt toolchain):
//
//   g++ -std=c++17 -I../src/ble ../src/ble/summarydecoder.cpp ../src/ble/sbemcontainer.cpp \
//       -x c ../src/ble/heatshrink/heatshrink_decoder.c -x c++ test_summarydecoder.cpp \
//       -o /tmp/test_summarydecoder && /tmp/test_summarydecoder
//
// fixtures/logbook_summary_cycling.bin is the real
// /Logbook/byId/1785740504/Summary payload from the 2026-09-21 HCI capture:
// the three paged responses (451 + 451 + 208 bytes) with their 19-byte
// headers stripped and concatenated, exactly what
// MdsWhiteboardClient::fetchSummary() assembles.
//
// This is the same workout as fixtures/logbook_data_heatshrink_cycling.bin,
// which makes the two decoders cross-checkable: everything Logbook::decode()
// derives from the sample stream should land near what the watch itself
// computed here.

#include "../src/ble/summarydecoder.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

namespace {

int g_failures = 0;

std::vector<uint8_t> readFile(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
}

void checkDouble(double actual, double expected, double tolerance, const std::string &name)
{
    if (std::fabs(actual - expected) <= tolerance) {
        std::printf("ok: %s = %f\n", name.c_str(), actual);
    } else {
        std::printf("FAIL: %s = %f, expected %f (+/- %f)\n", name.c_str(), actual, expected, tolerance);
        ++g_failures;
    }
}

void checkInt(int64_t actual, int64_t expected, const std::string &name)
{
    if (actual == expected) {
        std::printf("ok: %s = %lld\n", name.c_str(), static_cast<long long>(actual));
    } else {
        std::printf("FAIL: %s = %lld, expected %lld\n", name.c_str(),
                     static_cast<long long>(actual), static_cast<long long>(expected));
        ++g_failures;
    }
}

void testCyclingSummary()
{
    const Summary::DecodedSummary s =
            Summary::decode(readFile("fixtures/logbook_summary_cycling.bin"));

    checkInt(s.valid ? 1 : 0, 1, "valid");
    checkInt(s.activityId, 4, "activityId (cycling, same as the /Data decode)");
    checkDouble(s.durationSeconds, 4736.150, 0.001, "durationSeconds (total elapsed)");
    checkDouble(s.pauseDurationSeconds, 2436.334, 0.001, "pauseDurationSeconds");
    checkDouble(s.distanceMeters, 5417, 0.5, "distanceMeters");
    checkInt(s.stepCount, 0, "stepCount (no foot pod on this ride)");
    checkInt(s.hasAscent ? 1 : 0, 1, "hasAscent");
    checkDouble(s.ascentMeters, 27.843, 0.001, "ascentMeters");
    checkDouble(s.descentMeters, 19.470, 0.001, "descentMeters");
    checkInt(s.hasAltitude ? 1 : 0, 1, "hasAltitude");
    checkDouble(s.maxAltitudeMeters, 107.516, 0.001, "maxAltitudeMeters");
    checkDouble(s.minAltitudeMeters, 98.654, 0.001, "minAltitudeMeters");
    checkInt(s.hasEnergy ? 1 : 0, 1, "hasEnergy");
    checkDouble(s.energyKcal, 651466.125 / 4184.0, 0.01, "energyKcal");

    // Training metrics. This workout has EPOC, peak training effect and a
    // recovery time, but the watch left VO2max and training load at their
    // nillable-0 "not computed" value - so those two exercise the absent
    // path here, and their offsets are verified against the descriptor map
    // rather than against a non-zero reading.
    checkInt(s.hasEpoc ? 1 : 0, 1, "hasEpoc");
    checkDouble(s.epoc, 3.1, 0.001, "epoc");
    checkInt(s.hasPeakTrainingEffect ? 1 : 0, 1, "hasPeakTrainingEffect");
    checkDouble(s.peakTrainingEffect, 1.2, 0.001, "peakTrainingEffect");
    checkInt(s.hasRecoveryTime ? 1 : 0, 1, "hasRecoveryTime");
    checkDouble(s.recoveryTimeSeconds, 180, 0.5, "recoveryTimeSeconds");
    checkInt(s.hasMaxVo2 ? 1 : 0, 0, "hasMaxVo2 (not computed for this workout)");
    checkInt(s.hasTrainingLoad ? 1 : 0, 0, "hasTrainingLoad (not computed for this workout)");

    // Cross-checks against reality and against the /Data decoder, which is
    // the real point of this fixture:
    //
    // Jarno's app reported 38:19 (2299s) for this workout. Logbook::decode()
    // approximates that from GPS gaps and lands on 2140s (-7%); here it
    // falls straight out of the watch's own two figures.
    checkDouble(s.movingTimeSeconds, 2299, 1.0, "movingTimeSeconds matches the real 38:19 exactly");
    // Real distance 5420 m; the GPS-derived figure is 5393.7 m (-0.5%).
    checkDouble(s.distanceMeters, 5420, 5.0, "distanceMeters within 5m of the real 5420m");
    // Logbook::decode()'s altitude series gives 98.6-107.6 m for this same
    // workout - so the delta-chain reconstruction is right to ~0.1 m.
    checkDouble(s.minAltitudeMeters, 98.6, 0.1, "minAltitudeMeters agrees with the /Data decode");
    checkDouble(s.maxAltitudeMeters, 107.6, 0.1, "maxAltitudeMeters agrees with the /Data decode");
}

void testGarbageInputIsRejected()
{
    checkInt(Summary::decode({}).valid ? 1 : 0, 0, "empty payload is not valid");
    checkInt(Summary::decode({'n', 'o', 'p', 'e'}).valid ? 1 : 0, 0, "non-SBEM payload is not valid");
}

} // namespace

int main()
{
    testCyclingSummary();
    testGarbageInputIsRejected();

    if (g_failures == 0) {
        std::printf("\nAll assertions passed.\n");
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED.\n", g_failures);
    return 1;
}
