// Qt-free test for the per-watch descriptor table and the field layout
// resolved from it.
//
//   gcc -std=c11 -c ../src/ble/heatshrink/heatshrink_decoder.c -o /tmp/hs.o
//   g++ -std=c++17 ../src/ble/sbemtable.cpp ../src/ble/sbemlayout.cpp
//       ../src/ble/sbemdescriptors.cpp ../src/ble/sbemcontainer.cpp
//       ../src/ble/logbookdecoder.cpp ../src/ble/summarydecoder.cpp /tmp/hs.o
//       test_sbemlayout.cpp -o /tmp/test_sbemlayout
//   /tmp/test_sbemlayout
//
// Two watches, two numbering schemes. The compiled-in table is a Suunto
// Race's; the fixture is a Suunto 9 Baro's, read off the watch on
// 2026-09-25 with AppController::testDescriptorsFetch().
//
// The Race half is a regression guard with teeth: every number asserted
// there is a constant that used to be hard-coded in the decoders and was
// validated, back then, against Jarno's own reported workout figures. If
// resolving from the table ever stops reproducing them, this fails.

#include "../src/ble/logbookdecoder.h"
#include "../src/ble/sbemlayout.h"
#include "../src/ble/summarydecoder.h"

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

void checkInt(long long got, long long want, const std::string &what)
{
    check(got == want, what + " (" + std::to_string(got) + ", expected "
           + std::to_string(want) + ")");
}

std::vector<uint8_t> readFile(const char *path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
}

// The sample groups, asserted by the field they carry rather than by id -
// which is the whole point of the exercise.
void checkSampleGroups(const SbemLayout::Layout &layout, const char *who,
                        uint16_t gps, uint16_t hr, uint16_t absolute, uint16_t delta)
{
    const SbemLayout::Group *g = layout.group(gps);
    check(g != nullptr, std::string(who) + ": GPS group resolved");
    if (g) {
        checkInt(g->utc, 2, std::string(who) + " GPS: Sample.UTC offset");
        checkInt(g->latitude, 10, std::string(who) + " GPS: Sample.Latitude offset");
        checkInt(g->longitude, 14, std::string(who) + " GPS: Sample.Longitude offset");
    }

    g = layout.group(hr);
    check(g != nullptr, std::string(who) + ": heart-rate group resolved");
    if (g)
        checkInt(g->heartRate, 2, std::string(who) + " HR: Sample.HR offset");

    g = layout.group(absolute);
    check(g != nullptr, std::string(who) + ": absolute sample group resolved");
    if (g)
        checkInt(g->altitudeAbsolute, 18, std::string(who) + " abs: Sample.Altitude offset");

    g = layout.group(delta);
    check(g != nullptr, std::string(who) + ": delta sample group resolved");
    if (g) {
        checkInt(g->altitudeDelta, 8, std::string(who) + " delta: altitude delta offset");
        checkInt(g->cadence, 10, std::string(who) + " delta: Sample.Cadence offset");
    }
}

void checkHeader(const SbemLayout::Layout &layout, const char *who, uint16_t group)
{
    checkInt(layout.headerGroup, group, std::string(who) + ": header group id");
    const SbemLayout::Header &h = layout.header;
    // The offsets the Summary decoder used to hard-code, every one.
    checkInt(h.dateTime, 8, std::string(who) + " header: DateTime");
    checkInt(h.duration, 16, std::string(who) + " header: Duration");
    checkInt(h.pauseDuration, 20, std::string(who) + " header: PauseDuration");
    checkInt(h.distance, 24, std::string(who) + " header: Distance");
    checkInt(h.stepCount, 28, std::string(who) + " header: StepCount");
    checkInt(h.activityType, 36, std::string(who) + " header: ActivityType");
    checkInt(h.ascent, 40, std::string(who) + " header: Ascent");
    checkInt(h.descent, 48, std::string(who) + " header: Descent");
    checkInt(h.altitudeMax, 60, std::string(who) + " header: Altitude.Max");
    checkInt(h.altitudeMin, 64, std::string(who) + " header: Altitude.Min");
    checkInt(h.energy, 68, std::string(who) + " header: Energy");
    checkInt(h.epoc, 72, std::string(who) + " header: EPOC");
    checkInt(h.peakTrainingEffect, 76, std::string(who) + " header: PeakTrainingEffect");
    checkInt(h.recoveryTime, 80, std::string(who) + " header: RecoveryTime");
    checkInt(h.maxVo2, 84, std::string(who) + " header: MAXVO2");
    checkInt(h.trainingLoad, 90, std::string(who) + " header: TraingingLoadPeak");
    checkInt(h.minimumSize, 94, std::string(who) + " header: minimum readable size");
}

} // namespace

int main()
{
    // ---- the compiled-in Race table ----
    const SbemLayout::Layout &race = SbemLayout::builtin();
    check(race.valid(), "built-in layout resolves");
    checkSampleGroups(race, "Race", 0x0c, 0x12, 0x15, 0x16);
    checkHeader(race, "Race", 0x1b);

    const SbemLayout::Group *timeBase = race.group(0x01);
    check(timeBase && timeBase->timeBase, "Race: chunk 0x01 is the time base");
    const SbemLayout::Group *activity = race.group(0x08);
    check(activity && activity->activityType == 2, "Race: activity type at offset 2 in 0x08");
    const SbemLayout::Group *altEvent = race.group(0x04);
    check(altEvent && altEvent->altitudeOffset == 4,
          "Race: altitude calibration offset at 4 in 0x04");

    // ---- a 9 Baro's own table, read off the watch ----
    const std::vector<uint8_t> raw = readFile("fixtures/descriptors_9baro.bin");
    check(!raw.empty(), "9 Baro descriptors fixture read");
    if (raw.empty())
        return 1;

    const SbemDescriptors::Table baroTable = SbemDescriptors::Table::parse(raw);
    checkInt(static_cast<long long>(baroTable.size()), 265, "9 Baro: descriptors parsed");
    checkInt(baroTable.unknownFormats(), 0, "9 Baro: no unknown format base types");
    // The 9 Baro has a compass and the Race does not, so its table carries
    // a <MOD> expression the Race's never did. It is in the table here; if
    // it were not, this would be 2 rather than 0 and the heading readings
    // would be suppressed rather than mis-scaled.
    checkInt(baroTable.unknownMods(), 0, "9 Baro: no unrecognised <MOD> expressions");

    const SbemLayout::Layout baro = SbemLayout::resolve(baroTable);
    check(baro.valid(), "9 Baro layout resolves");
    // Different ids, identical offsets - which is the finding that made
    // this change small rather than a rewrite of both decoders' arithmetic.
    checkSampleGroups(baro, "9 Baro", 0x0d, 0x15, 0x18, 0x19);
    checkHeader(baro, "9 Baro", 0x1e);

    // The Race's ids must NOT resolve on the 9 Baro - if they did, the two
    // tables would not really differ and this whole exercise would be moot.
    const SbemLayout::Group *raceGps = baro.group(0x0c);
    check(!raceGps || raceGps->latitude < 0,
          "9 Baro: the Race's GPS chunk id carries no latitude");

    // A second GPS group with no altitude field. Resolving by name finds
    // it; a decoder written around one id would not, and "the GPS group"
    // would have been the wrong question to ask.
    const SbemLayout::Group *gpsNoAltitude = baro.group(0x13);
    check(gpsNoAltitude && gpsNoAltitude->latitude == 10
              && gpsNoAltitude->altitudeAbsolute < 0,
          "9 Baro: the second, altitude-less GPS group is found too");

    // ---- end to end, on a real 9 Baro workout ----
    const std::vector<uint8_t> data = readFile("fixtures/logbook_data_9baro.bin");
    check(!data.empty(), "9 Baro workout fixture read");
    if (data.empty())
        return g_failures == 0 ? 0 : 1;

    const Logbook::DecodedWorkout wrong = Logbook::decode(data, race);
    check(wrong.track.empty() && wrong.avgHeartRateBpm == 0 && wrong.totalDistanceMeters == 0,
          "9 Baro workout decoded with the Race's layout yields nothing - the original bug");

    const Logbook::DecodedWorkout right = Logbook::decode(data, baro);
    check(right.track.size() > 500, "9 Baro workout decoded with its own layout has a GPS track ("
           + std::to_string(right.track.size()) + " points)");
    check(right.avgHeartRateBpm > 40 && right.maxHeartRateBpm < 220,
          "heart rate is physiological (" + std::to_string(right.avgHeartRateBpm) + " avg, "
            + std::to_string(right.maxHeartRateBpm) + " max)");
    check(right.totalDistanceMeters > 100, "distance is non-zero ("
           + std::to_string(right.totalDistanceMeters) + " m)");

    // An independent check that does not lean on any value this code
    // produced: a logbook entry's id IS the workout's Unix start time in
    // seconds (docs/logbook-data-format.md, confirmed against three real
    // workouts on a different watch). The fixture is entry 1766684567, so
    // the decoded clock has to land on it.
    const long long startSeconds = static_cast<long long>(right.startTimeMs / 1000);
    check(startSeconds == 1766684567,
          "decoded start time matches the logbook id, which is that same instant ("
            + std::to_string(startSeconds) + ")");

    const std::vector<uint8_t> summaryPayload = readFile("fixtures/logbook_summary_9baro.bin");
    if (!summaryPayload.empty()) {
        check(!Summary::decode(summaryPayload, race).valid,
              "9 Baro summary is not readable with the Race's header chunk id");
        const Summary::DecodedSummary summary = Summary::decode(summaryPayload, baro);
        check(summary.valid, "9 Baro summary decodes with its own layout");
        check(summary.durationSeconds > 60 && summary.durationSeconds < 24 * 3600,
              "summary duration is a plausible workout length ("
                + std::to_string(summary.durationSeconds) + " s)");
        check(summary.stepCount > 0, "summary step count is non-zero ("
               + std::to_string(summary.stepCount) + ")");
        check(static_cast<long long>(summary.startTimeMs / 1000) == 1766684567,
              "summary start time matches the logbook id too");
    }

    // A payload that is not a descriptor table must produce an empty one
    // rather than a table of nonsense.
    check(SbemDescriptors::Table::parse({'n', 'o', 'p', 'e'}).empty(),
          "a non-SBEM payload parses to an empty table");
    check(!SbemLayout::resolve(SbemDescriptors::Table()).valid(),
          "an empty table resolves to an unusable layout");

    if (g_failures == 0) {
        std::printf("\nAll assertions passed.\n");
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED.\n", g_failures);
    return 1;
}
