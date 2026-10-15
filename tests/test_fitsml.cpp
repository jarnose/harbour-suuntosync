// Qt-free test for the FIT -> cloud-SML builders.
//
//   g++ -std=c++17 -I ../src -o /tmp/t test_fitsml.cpp ../src/cloud/smljson.cpp
//       ../src/cloud/iso8601.cpp ../src/ble/smldecoder.cpp ../src/ble/sbemcontainer.cpp
//       ../src/ble/sbemtable.cpp ../src/ble/sbemdescriptors.cpp ../src/ble/sbemlayout.cpp
//       ../src/fit/activity.cpp ../src/fit/fitdecoder.cpp ../src/fit/fitcrc.cpp
//       ../src/ble/heatshrink/heatshrink_decoder.c
//   /tmp/t
//
// The activity is built here rather than decoded from a file: the FIT
// decoder is tested in harbour-edgesync, where it was written, and what is
// new here is the units and the shape of the two documents. Those are
// exactly what a wrong upload gets refused for - this project has already
// spent five rounds on a 523 that turned out to be degrees where the
// format wants radians.

#include "cloud/smljson.h"
#include "fit/activity.h"

#include <cctype>
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

void checkContains(const std::string &haystack, const std::string &needle)
{
    const bool found = haystack.find(needle) != std::string::npos;
    check(found, "contains " + needle);
    if (!found)
        std::printf("    in: %.400s\n", haystack.c_str());
}

Fit::Activity sampleActivity()
{
    Fit::Activity a;
    a.summary.startTime = 1788194034;   // 2026-08-31T19:33:54+03:00
    a.summary.sport = 11;               // walking
    a.summary.elapsed = 1500;
    a.summary.timer = 1400;             // 100 s of pauses
    a.summary.distance = 2094;
    a.summary.ascent = 15;
    a.summary.descent = 19;
    a.summary.calories = 132;
    a.summary.avgHeartRate = 87;
    a.summary.maxHeartRate = 99;
    a.summary.minAltitude = 104;
    a.summary.maxAltitude = 123.4;
    a.summary.manufacturer = 1;
    a.summary.serial = 3428415862;
    a.summary.productName = "Edge Explore 2";

    Fit::Sample first;
    first.time = 1788194034;
    first.lat = 61.48334;
    first.lon = 23.90815;
    first.altitude = 104.0;
    first.distance = 0;
    first.speed = 1.4;
    first.temperature = 20.0;
    first.heartRate = 81;
    first.cadence = 55;
    first.power = 0;
    a.samples.push_back(first);

    // A second with nothing but a clock: it must not become an entry.
    Fit::Sample empty;
    empty.time = 1788194035;
    a.samples.push_back(empty);
    return a;
}

} // namespace

int main()
{
    const Fit::Activity activity = sampleActivity();
    const std::string samples =
            SmlJson::buildFitSamples(activity, "garmin-3428415862", 180);
    const std::string summary =
            SmlJson::buildFitSummary(activity, "garmin-3428415862", 180, 12);

    // Position in radians, not degrees. 61.48334 deg = 1.0730867... rad, and
    // the captured cloud document for the same spot has 1.0730863.
    checkContains(samples, "\"Latitude\":1.0730867");
    checkContains(samples, "\"Longitude\":0.4172759");
    check(samples.find("61.48") == std::string::npos,
          "no degrees anywhere in samples.json");

    // Heart rate and cadence in hertz; 81 bpm is 1.35 Hz, 55 rpm is 0.917.
    checkContains(samples, "\"HR\":1.35");
    checkContains(samples, "\"Cadence\":0.917");
    // Temperature in kelvin: 20 C is 293.15.
    checkContains(samples, "\"Temperature\":293.2");
    // Power of 0 is a reading, not a gap, and has to survive.
    checkContains(samples, "\"Power\":0");
    checkContains(samples, "\"Source\":\"garmin-3428415862\"");
    checkContains(samples, "\"TimeISO8601\":\"2026-08-31T19:33:54.000+03:00\"");

    // One entry, not two: the clock-only sample is dropped.
    size_t entries = 0;
    for (size_t at = samples.find("\"Attributes\""); at != std::string::npos;
         at = samples.find("\"Attributes\"", at + 1)) {
        ++entries;
    }
    check(entries == 1, "a sample with no readings is not an entry");

    // Energy in joules: 132 kcal x 4184.
    checkContains(summary, "\"Energy\":552288");
    // The whole span as Duration and the stopped time beside it, the way
    // the watch states them - not the moving time as Duration.
    checkContains(summary, "\"Duration\":1500");
    checkContains(summary, "\"PauseDuration\":100");
    checkContains(summary, "\"ActivityType\":12");
    checkContains(summary, "\"Distance\":2094");
    checkContains(summary, "\"HR\":{\"Avg\":1.45,\"Max\":1.65}");
    checkContains(summary, "\"Altitude\":{\"Max\":123.4,\"Min\":104}");
    // Whose ride it was, from the file rather than from the paired watch.
    checkContains(summary, "\"Device\":{\"Name\":\"Edge Explore 2\"");
    checkContains(summary, "\"SerialNumber\":\"3428415862\"");
    // summary.json is stamped at the end of the workout, as the capture is.
    checkContains(summary, "\"TimeISO8601\":\"2026-08-31T19:58:54.000+03:00\"");

    // Nothing anywhere may carry a decimal comma, whatever locale the
    // phone is in - which is a real defect this project shipped once, in
    // the first version of the upload. A comma between two digits is the
    // only way to tell one from an ordinary separator.
    auto hasDecimalComma = [](const std::string &text) {
        for (size_t i = 1; i + 1 < text.size(); ++i) {
            if (text[i] == ',' && std::isdigit(static_cast<unsigned char>(text[i - 1]))
                    && std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
                return true;
            }
        }
        return false;
    };
    check(!hasDecimalComma(samples), "no decimal comma in samples.json");
    check(!hasDecimalComma(summary), "no decimal comma in summary.json");

    std::printf(failures == 0 ? "\nAll assertions passed.\n" : "\n%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
