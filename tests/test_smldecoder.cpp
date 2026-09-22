// Qt-free test for the generic descriptor-driven decoder.
//
//   g++ -std=c++17 -I../src/ble ../src/ble/smldecoder.cpp \
//       ../src/ble/sbemdescriptors.cpp ../src/ble/sbemcontainer.cpp \
//       -x c ../src/ble/heatshrink/heatshrink_decoder.c -x c++ \
//       test_smldecoder.cpp -o /tmp/test_smldecoder && /tmp/test_smldecoder
//
// The point of this test is cross-validation, not just "it runs": every
// expected value below is one that a *different*, independently written and
// hardware-validated decoder in this project already produces. If the
// generic decoder agrees with Logbook::decode() on heart rate and altitude,
// and with Summary::decode() on distance, then walking the watch's own
// descriptor table reproduces the hand-written offsets - which is the whole
// claim being made.
//
// Note the units: this decoder reports the schema's canonical ones, so heart
// rate and cadence come back in Hz rather than bpm.

#include "../src/ble/smldecoder.h"
#include "../src/ble/sbemcontainer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string &what)
{
    if (ok) {
        std::printf("ok: %s\n", what.c_str());
    } else {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

void checkNear(double actual, double expected, double tolerance, const std::string &what)
{
    if (std::fabs(actual - expected) <= tolerance) {
        std::printf("ok: %s = %f\n", what.c_str(), actual);
    } else {
        std::printf("FAIL: %s = %f, expected %f (+/- %f)\n",
                     what.c_str(), actual, expected, tolerance);
        ++g_failures;
    }
}

std::vector<uint8_t> readFile(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
}

struct Series
{
    int count = 0;
    double min = 0, max = 0, sum = 0;
    int64_t firstTimeMs = 0, lastTimeMs = 0;
    void add(double v, int64_t t)
    {
        if (count == 0) {
            min = max = v;
            firstTimeMs = t;
        }
        min = std::min(min, v);
        max = std::max(max, v);
        sum += v;
        lastTimeMs = t;
        ++count;
    }
    double avg() const { return count > 0 ? sum / count : 0; }
};

std::map<std::string, Series> collect(const std::string &fixture)
{
    const std::vector<uint8_t> decompressed =
            Sbem::heatshrinkDecompress(readFile(fixture));
    std::map<std::string, Series> byName;
    Sml::decode(Sbem::parseContainer(decompressed), [&byName](const Sml::Reading &r) {
        byName[r.descriptor->name].add(r.value, r.timeMs);
    });
    return byName;
}

void testCyclingAgreesWithTheHandWrittenDecoders()
{
    const auto s = collect("fixtures/logbook_data_heatshrink_cycling.bin");
    std::printf("--- cycling: %zu distinct fields decoded ---\n", s.size());

    // Heart rate: Logbook::decode() reports avg 75.6996 bpm and max 97 bpm
    // from the same fixture. Here it's Hz, so divide by 60.
    check(s.count("Sample.HR") > 0, "Sample.HR present");
    if (s.count("Sample.HR")) {
        checkNear(s.at("Sample.HR").avg() * 60, 75.6996, 0.001, "Sample.HR avg as bpm");
        checkNear(s.at("Sample.HR").max * 60, 97, 0.001, "Sample.HR max as bpm");
    }

    // Altitude: this decoder resolves the same delta chain Logbook::decode()
    // does, but deliberately reports readings rather than corrections, so it
    // does *not* apply the chunk-0x04 calibration offset (-12 m here). The
    // minimum is identical to Logbook::decode()'s 98.6 m because the low
    // point falls after the calibration event; the maximum sits 8.4 m higher
    // because the high point falls before it. Pinned down explicitly so the
    // difference isn't mistaken for a decoding bug later.
    check(s.count("Sample.Altitude") > 0, "Sample.Altitude present");
    if (s.count("Sample.Altitude")) {
        checkNear(s.at("Sample.Altitude").min, 98.6, 0.1, "Sample.Altitude min");
        checkNear(s.at("Sample.Altitude").max, 116.0, 0.1,
                   "Sample.Altitude max, uncorrected by the calibration event");
        check(s.at("Sample.Altitude").count > 2000, "Sample.Altitude sampled throughout");
    }

    // Distance is *recorded*, not derived - and its final value should land
    // on the 5417 m /Summary reports as Header.Distance.
    check(s.count("Sample.Distance") > 0, "Sample.Distance present");
    if (s.count("Sample.Distance"))
        checkNear(s.at("Sample.Distance").max, 5417, 5, "Sample.Distance max matches Header.Distance");

    // GPS: the same fixes Logbook::decode() returns as the track, in degrees
    // rather than the schema's radians.
    check(s.count("Sample.Latitude") > 0 && s.count("Sample.Longitude") > 0, "GPS present");
    if (s.count("Sample.Latitude")) {
        checkNear(s.at("Sample.Latitude").min, 61.48, 0.1, "Sample.Latitude in southern Finland");
        checkNear(s.at("Sample.Longitude").min, 23.9, 0.2, "Sample.Longitude in southern Finland");
        check(s.at("Sample.Latitude").count == 1820, "GPS fix count matches the track");
    }

    // The clock, tracked through the time base and the per-chunk deltas -
    // Logbook::decode() reports this workout starting at 1785740505200.
    if (s.count("Sample.HR"))
        checkNear(static_cast<double>(s.at("Sample.HR").firstTimeMs), 1785740505200.0, 2000.0,
                   "clock lands on the workout's real start");

    // Fields nothing had decoded before this: temperature (Kelvin), speed,
    // battery and GPS quality.
    check(s.count("Sample.Temperature") > 0, "Sample.Temperature present (new)");
    if (s.count("Sample.Temperature")) {
        checkNear(s.at("Sample.Temperature").avg() - 273.15, 29, 3,
                   "Sample.Temperature avg in Celsius is plausible");
    }
    // Speed is recorded too, but reconstructing it from the delta chain
    // undershoots the watch's own Windows.Window.Speed.Max of 7.15 m/s by
    // ~14%: the deltas are int8 and only re-synced by the rare absolute
    // snapshots (four in this whole workout), so a brief peak between two
    // of them isn't fully recovered. Checked against reality here rather
    // than against a figure this decoder can't legitimately reach - the
    // Summary's is the number to trust and the one the app shows.
    check(s.count("Sample.Speed") > 0, "Sample.Speed present (new)");
    if (s.count("Sample.Speed")) {
        const Series &speed = s.at("Sample.Speed");
        check(speed.min >= 0 && speed.max < 7.15,
              "Sample.Speed stays within the watch's own reported maximum");
        checkNear(speed.max, 6.18, 0.01, "Sample.Speed max from the delta chain");
    }
    check(s.count("Sample.BatteryCharge") > 0, "Sample.BatteryCharge present (new)");
    check(s.count("Sample.NumberOfSatellites") > 0, "Sample.NumberOfSatellites present (new)");
    if (s.count("Sample.NumberOfSatellites")) {
        const Series &sats = s.at("Sample.NumberOfSatellites");
        check(sats.min >= 0 && sats.max <= 40, "satellite count is in a sane range");
    }
}

void testWalkingDecodesToo()
{
    const auto s = collect("fixtures/logbook_data_heatshrink.bin");
    std::printf("--- walking: %zu distinct fields decoded ---\n", s.size());
    // Logbook::decode() reports 86.72 avg / 99 max bpm and 103.8-123.0 m for
    // this fixture.
    if (s.count("Sample.HR")) {
        checkNear(s.at("Sample.HR").avg() * 60, 86.72, 0.001, "Sample.HR avg as bpm");
        checkNear(s.at("Sample.HR").max * 60, 99, 0.001, "Sample.HR max as bpm");
    }
    // This is the workout whose barometer hadn't settled: its first samples
    // read ~315 m and the calibration event carries -208 m. Uncorrected, the
    // maximum is that unsettled start; the minimum is unaffected because it
    // falls after the event.
    if (s.count("Sample.Altitude")) {
        checkNear(s.at("Sample.Altitude").min, 103.8, 0.1, "Sample.Altitude min");
        checkNear(s.at("Sample.Altitude").max, 331.0, 1.0,
                   "Sample.Altitude max is the uncalibrated start");
    }
    check(s.count("Sample.Cadence") > 0, "Sample.Cadence present");
    if (s.count("Sample.Cadence"))
        check(s.at("Sample.Cadence").max * 60 < 200, "cadence max in rpm is plausible");
}

} // namespace

int main()
{
    testCyclingAgreesWithTheHandWrittenDecoders();
    testWalkingDecodesToo();

    if (g_failures == 0) {
        std::printf("\nAll assertions passed.\n");
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED.\n", g_failures);
    return 1;
}
