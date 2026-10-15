// Qt-free round trip for the FIT encoder: encode an activity, decode it
// back with this project's own decoder, and compare.
//
//   g++ -std=c++17 -I ../src -o /tmp/t test_fitencoder.cpp
//       ../src/fit/fitencoder.cpp ../src/fit/activity.cpp
//       ../src/fit/fitdecoder.cpp ../src/fit/fitcrc.cpp
//   /tmp/t
//
// A round trip is the right test here because the decoder is the proven
// half: it reads real Garmin and Suunto files, so what it reads back is
// what those tools would read. It also covers the thing a hand-written
// encoder gets wrong first - a field written in the wrong unit comes back
// wrong, and a field written at the wrong number comes back absent.

#include "fit/activity.h"
#include "fit/fitencoder.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const std::string &what)
{
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok)
        ++failures;
}

void checkNear(double got, double want, double tolerance, const std::string &what)
{
    const bool ok = std::fabs(got - want) <= tolerance;
    std::printf("%s: %s (%.6f vs %.6f)\n", ok ? "ok" : "FAIL", what.c_str(), got, want);
    if (!ok)
        ++failures;
}

Fit::Activity original()
{
    Fit::Activity a;
    a.summary.startTime = 1792068684;
    a.summary.sport = 2;
    a.summary.subSport = 28;
    a.summary.elapsed = 3810.73;
    a.summary.timer = 1983.0;
    a.summary.distance = 5149.1;
    a.summary.avgSpeed = 2.6;
    a.summary.maxSpeed = 6.07;
    a.summary.avgHeartRate = 87;
    a.summary.maxHeartRate = 99;
    a.summary.avgCadence = 55;
    a.summary.maxCadence = 72;
    a.summary.avgPower = 120;
    a.summary.maxPower = 410;
    a.summary.ascent = 40;
    a.summary.descent = 46;
    a.summary.minAltitude = 97.8;
    a.summary.maxAltitude = 111.4;
    a.summary.avgTemperature = 13;
    a.summary.calories = 142;
    a.summary.startLat = 61.48338;
    a.summary.startLon = 23.90830;
    a.summary.manufacturer = 1;
    a.summary.product = 4169;
    a.summary.serial = 3428415862;

    for (int i = 0; i < 5; ++i) {
        Fit::Sample s;
        s.time = 1792068684 + i;
        s.lat = 61.48338 + i * 0.0001;
        s.lon = 23.90830 + i * 0.0001;
        s.altitude = 105.0 + i;
        s.distance = i * 7.5;
        s.speed = 2.5 + i * 0.25;
        s.temperature = 13;
        s.heartRate = 80 + i;
        s.cadence = 50 + i;
        s.power = 100 + i;
        a.samples.push_back(s);
    }
    // One with nothing but a clock and a position: the gaps have to come
    // back as gaps, not as zeros.
    Fit::Sample sparse;
    sparse.time = 1792068690;
    sparse.lat = 61.4840;
    sparse.lon = 23.9090;
    a.samples.push_back(sparse);

    Fit::Lap lap;
    lap.startTime = 1792068684;
    lap.elapsed = 1800;
    lap.timer = 1700;
    lap.distance = 3000;
    lap.avgSpeed = 1.76;
    lap.maxSpeed = 6.07;
    lap.avgHeartRate = 85;
    lap.maxHeartRate = 99;
    lap.avgCadence = 54;
    lap.avgPower = 118;
    lap.ascent = 25;
    lap.descent = 20;
    lap.calories = 90;
    a.laps.push_back(lap);
    return a;
}

} // namespace

int main()
{
    const Fit::Activity in = original();
    const std::vector<uint8_t> bytes = Fit::encodeActivity(in);
    check(!bytes.empty(), "something was written");
    check(bytes.size() > 14, "and it is more than a header");
    check(bytes[0] == 14, "a 14-byte header");
    check(bytes[8] == '.' && bytes[9] == 'F' && bytes[10] == 'I' && bytes[11] == 'T',
          "the .FIT signature at offset 8");

    Fit::Activity out;
    std::string error;
    check(Fit::parseActivity(bytes, out, error),
          "our own decoder reads it back" + (error.empty() ? "" : ": " + error));
    check(out.stats.crcOk, "and both CRCs match");

    // The summary, field by field. Every one of these is a number written
    // in a unit and read back in another if the scale is wrong.
    check(out.summary.startTime == in.summary.startTime, "start time");
    check(out.summary.sport == 2 && out.summary.subSport == 28, "sport 2/28 survives");
    check(Fit::sportKey(out.summary.sport, out.summary.subSport) == "e_biking",
          "and still reads as an e-bike ride");
    checkNear(out.summary.elapsed, in.summary.elapsed, 0.001, "elapsed time");
    checkNear(out.summary.timer, in.summary.timer, 0.001, "timer");
    checkNear(out.summary.distance, in.summary.distance, 0.01, "distance");
    checkNear(out.summary.avgSpeed, in.summary.avgSpeed, 0.001, "average speed");
    checkNear(out.summary.maxSpeed, in.summary.maxSpeed, 0.001, "maximum speed");
    check(out.summary.avgHeartRate == 87 && out.summary.maxHeartRate == 99, "heart rate");
    check(out.summary.avgCadence == 55 && out.summary.maxCadence == 72, "cadence");
    check(out.summary.avgPower == 120 && out.summary.maxPower == 410, "power");
    checkNear(out.summary.ascent, 40, 0.5, "ascent");
    checkNear(out.summary.descent, 46, 0.5, "descent");
    // Altitude goes out as (m + 500) * 5, so a fifth of a metre is the
    // resolution the format has.
    checkNear(out.summary.minAltitude, in.summary.minAltitude, 0.2, "minimum altitude");
    checkNear(out.summary.maxAltitude, in.summary.maxAltitude, 0.2, "maximum altitude");
    checkNear(out.summary.avgTemperature, 13, 0.5, "temperature");
    check(out.summary.calories == 142, "calories");
    checkNear(out.summary.startLat, in.summary.startLat, 0.00001, "start latitude");
    checkNear(out.summary.startLon, in.summary.startLon, 0.00001, "start longitude");
    check(out.summary.manufacturer == 1 && out.summary.product == 4169, "the device");
    check(out.summary.serial == 3428415862u, "and its serial number");

    // The samples.
    check(out.samples.size() == in.samples.size(), "every sample came back");
    if (out.samples.size() == in.samples.size()) {
        for (size_t i = 0; i < in.samples.size(); ++i) {
            const Fit::Sample &a = in.samples[i];
            const Fit::Sample &b = out.samples[i];
            check(a.time == b.time, "sample " + std::to_string(i) + " time");
            checkNear(b.lat, a.lat, 0.00001, "sample " + std::to_string(i) + " latitude");
            checkNear(b.lon, a.lon, 0.00001, "sample " + std::to_string(i) + " longitude");
            if (!std::isnan(a.altitude))
                checkNear(b.altitude, a.altitude, 0.2, "sample " + std::to_string(i) + " altitude");
            if (!std::isnan(a.speed))
                checkNear(b.speed, a.speed, 0.001, "sample " + std::to_string(i) + " speed");
            check(a.heartRate == b.heartRate, "sample " + std::to_string(i) + " heart rate");
            check(a.cadence == b.cadence, "sample " + std::to_string(i) + " cadence");
            check(a.power == b.power, "sample " + std::to_string(i) + " power");
        }
        // The sparse one: a gap has to come back as a gap.
        const Fit::Sample &sparse = out.samples.back();
        check(sparse.heartRate == -1, "a missing heart rate is still missing");
        check(sparse.cadence == -1, "a missing cadence is still missing");
        check(sparse.power == -1, "a missing power is still missing");
        check(std::isnan(sparse.altitude), "a missing altitude is still missing");
        check(std::isnan(sparse.speed), "a missing speed is still missing");
        check(sparse.hasPosition(), "but its position is there");
    }

    // The lap.
    check(out.laps.size() == 1, "the lap came back");
    if (out.laps.size() == 1) {
        const Fit::Lap &a = in.laps[0];
        const Fit::Lap &b = out.laps[0];
        check(a.startTime == b.startTime, "lap start time");
        checkNear(b.elapsed, a.elapsed, 0.001, "lap elapsed");
        checkNear(b.timer, a.timer, 0.001, "lap timer");
        checkNear(b.distance, a.distance, 0.01, "lap distance");
        checkNear(b.avgSpeed, a.avgSpeed, 0.001, "lap average speed");
        check(a.avgHeartRate == b.avgHeartRate, "lap average heart rate");
        check(a.avgCadence == b.avgCadence, "lap average cadence");
        check(a.avgPower == b.avgPower, "lap average power");
        checkNear(b.ascent, a.ascent, 0.5, "lap ascent");
        checkNear(b.descent, a.descent, 0.5, "lap descent");
        check(a.calories == b.calories, "lap calories");
    }

    std::printf(failures == 0 ? "\nAll assertions passed.\n" : "\n%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
