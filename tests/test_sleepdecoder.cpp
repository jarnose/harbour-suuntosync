// Qt-free test for the sleep timeline decoder.
//
//   g++ -std=c++17 ../src/ble/sleepdecoder.cpp ../src/ble/sbemcontainer.cpp \
//       heatshrink_decoder.o test_sleepdecoder.cpp -o /tmp/test_sleepdecoder \
//       && /tmp/test_sleepdecoder
//
// The fixture is a real file fetched off the watch on 2026-09-23. Its
// expected values are not this decoder's output - they are what the Suunto
// cloud reports for the same night, captured independently from
// 247.sports-tracker.com (tests/fixtures/cloud_247_v1_sleep.json). Two
// separate paths to the same numbers is the whole point.

#include "../src/ble/sleepdecoder.h"
#include "../src/ble/sbemcontainer.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

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
        std::printf("ok: %s = %g\n", what.c_str(), actual);
    else
        std::printf("FAIL: %s = %g, expected %g\n", what.c_str(), actual, expected);
    if (!ok)
        ++g_failures;
}

std::vector<uint8_t> readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
}

} // namespace

int main()
{
    const std::vector<uint8_t> raw = readFile("fixtures/sleep_timeline.sbem");
    check(!raw.empty(), "fixture read");
    if (raw.empty())
        return 1;

    // The file is not Heatshrink-compressed - it comes off the filesystem
    // as-is, unlike a workout's /Data.
    const std::vector<Sbem::Chunk> chunks = Sbem::parseContainer(raw);
    check(!chunks.empty(), "SBEM0102 container parses (it is not 0103)");
    check(chunks.size() > 800, "all chunks found (" + std::to_string(chunks.size()) + ")");

    const std::vector<SleepTimeline::Night> nights = SleepTimeline::decode(chunks);
    check(nights.size() >= 10, "several nights decoded (" + std::to_string(nights.size()) + ")");

    // The night the cloud fixture also describes: sleepId 1790020440.
    const SleepTimeline::Night *known = nullptr;
    for (const SleepTimeline::Night &n : nights) {
        if (n.sleepId == 1790020440u)
            known = &n;
    }
    check(known != nullptr, "the cross-checkable night is present");
    if (!known)
        return 1;

    // Every figure below is from cloud_247_v1_sleep.json's entryData.
    checkNear(known->durationSeconds, 27960, 0.5, "duration");
    checkNear(known->deepSeconds, 5610, 0.5, "deepSleepDuration");
    checkNear(known->lightSeconds, 15630, 0.5, "lightSleepDuration");
    checkNear(known->remSeconds, 6690, 0.5, "remSleepDuration");
    checkNear(known->wakeAfterOnsetSeconds, 30, 0.5, "wakeAfterSleepOnsetDuration");
    checkNear(known->onsetLatencySeconds, 0, 0.5, "sleepOnsetLatencyDuration");
    checkNear(known->heartRateAvgHz, 0.8333333, 1e-5, "hrAvg (hertz)");
    checkNear(known->heartRateMinHz, 0.76666665, 1e-5, "hrMin (hertz)");
    checkNear(known->maxSpo2, 0.99, 1e-5, "maxSpo2");
    checkNear(known->altitudeMetres, 208, 0.5, "altitude");
    check(known->hrvAverageMs == 32, "avgHrv = 32");
    check(known->hrvSampleCount == 55, "avgHrvSampleCount = 55");
    check(known->qualityPercent == 82, "quality = 82% (cloud says 0.82)");

    // 0xFF means "not measured" and must not reach the cloud as 255, which
    // it did once: the upload came back with "'quality' with value 2.55 is
    // outside of range 0.0...1.0". Three of the captured nights have it.
    int missingQuality = 0;
    for (const SleepTimeline::Night &n : nights) {
        check(n.qualityPercent <= 100, "quality is a percentage or -1, never 255");
        if (n.qualityPercent < 0)
            ++missingQuality;
    }
    check(missingQuality == 3, "three nights have no quality reading ("
           + std::to_string(missingQuality) + ")");
    check(!known->isNap, "isNap false");
    check(known->source == "suunto-247-sleep-2352D0000247", "source string");

    // The timestamp: the cloud's "2026-09-21T22:54:00.000+03:00" is
    // 1790020440000 ms UTC, and the file stores microseconds.
    check(known->startMs == 1790020440000LL,
           "start timestamp converted from microseconds ("
           + std::to_string(known->startMs) + ")");

    // Stage totals must add up to the per-stage figures above. This is what
    // pins the enum: if REM and deep were swapped, these two would swap.
    double byStage[4] = { 0, 0, 0, 0 };
    for (const SleepTimeline::StageSample &s : known->stages) {
        const int idx = static_cast<int>(s.stage);
        if (idx >= 0 && idx < 4)
            byStage[idx] += s.durationSeconds;
    }
    check(!known->stages.empty(), "stage samples present ("
           + std::to_string(known->stages.size()) + ")");
    checkNear(byStage[0], 30, 0.5, "stage 0 total = awake");
    checkNear(byStage[1], 6690, 0.5, "stage 1 total = REM");
    checkNear(byStage[2], 15630, 0.5, "stage 2 total = light");
    checkNear(byStage[3], 5610, 0.5, "stage 3 total = deep");

    // Newest first.
    for (size_t i = 1; i < nights.size(); ++i) {
        if (nights[i - 1].startMs < nights[i].startMs) {
            check(false, "nights are ordered newest first");
            break;
        }
    }

    if (g_failures == 0) {
        std::printf("\nAll assertions passed.\n");
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED.\n", g_failures);
    return 1;
}
