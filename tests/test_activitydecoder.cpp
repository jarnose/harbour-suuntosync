// Qt-free test for the daily-activity (/Activity/TrendData) decoder.
//
//   g++ -std=c++17 ../src/ble/activitydecoder.cpp ../src/cloud/iso8601.cpp
//       test_activitydecoder.cpp -o /tmp/test_activitydecoder
//   /tmp/test_activitydecoder
//
// The fixture is a real reply from Jarno's Suunto Race, fetched by
// AppController::testActivityTrendFetch() on 2026-09-25.
//
// The expected values are NOT typed into this file. They are read out of
// tests/fixtures/cloud_247_v1_activity_trend.json - the Suunto cloud's own
// entries for the same ten-minute buckets, which reached the cloud through
// the official Android app and therefore know nothing about this project's
// decoder. Every field the decoder claims is checked against that.

#include "../src/ble/activitydecoder.h"
#include "../src/cloud/iso8601.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
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

struct CloudEntry
{
    double hrHz = 0;
    int stepCount = 0;
    double energy = 0;
};

// A deliberately small reader for one known file shape, rather than a JSON
// library this project doesn't otherwise need. If the fixture stops
// matching this shape the map comes back empty and the test says so.
std::map<int64_t, CloudEntry> readCloudFixture(const std::string &path)
{
    std::map<int64_t, CloudEntry> out;
    std::ifstream in(path);
    const std::string text((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());

    size_t pos = 0;
    while (true) {
        const size_t tsKey = text.find("\"timestamp\"", pos);
        if (tsKey == std::string::npos)
            break;
        const size_t open = text.find('"', text.find(':', tsKey));
        const size_t close = text.find('"', open + 1);
        if (open == std::string::npos || close == std::string::npos)
            break;
        int64_t ms = 0;
        if (!Iso8601::parseToUnixMs(text.substr(open + 1, close - open - 1), &ms))
            break;

        const size_t blockEnd = text.find("\"timestamp\"", close);
        const std::string block = text.substr(close, (blockEnd == std::string::npos)
                                                       ? std::string::npos
                                                       : blockEnd - close);
        auto number = [&block](const char *key, double fallback) {
            const size_t k = block.find(key);
            if (k == std::string::npos)
                return fallback;
            return std::atof(block.c_str() + block.find(':', k) + 1);
        };

        CloudEntry entry;
        entry.hrHz = number("\"hr\"", -1);
        entry.stepCount = static_cast<int>(number("\"stepCount\"", -1));
        entry.energy = number("\"energyConsumption\"", -1);
        out[ms] = entry;

        pos = close;
    }
    return out;
}

} // namespace

// A reply holding one usable record, built from a real one: a 9 Baro that
// had been flat and was just charged answered 106 bytes - the 26-byte
// header and room for two 40-byte records, of which only the first is a
// record. The decoder found nothing at all, because its rhythm rule needs
// two timestamps an interval apart. The recovery decoder had the same bug
// and the same fix; this one was written from the same shape and missed it.
//
// The first 64 bytes are the real reply, as the journal printed them. The
// rest is zero, which is the worst case for the fallback and no weaker
// than the real tail.
void checkSingleRecordReply()
{
    static const char kHead[] =
            "f0050e018000c80001055e240000020000000c00000000000000000000000100"
            "00004c14cc8da101000000000000000000001500000000910040100000000000";

    std::vector<uint8_t> body(106, 0);
    for (size_t i = 0; i * 2 + 1 < sizeof(kHead) - 1 && i < body.size(); ++i) {
        body[i] = static_cast<uint8_t>(
                std::stoi(std::string(kHead + i * 2, 2), nullptr, 16));
    }

    const std::vector<ActivityTrend::Sample> one = ActivityTrend::decode(body);
    check(one.size() == 1, "a reply with one usable record decodes to one sample ("
                             + std::to_string(one.size()) + ")");
    if (one.size() == 1) {
        // 2026-10-30 17:12:04.428 UTC, snapped to the ten-minute grid.
        check(one[0].rawTimestampMs == 1793380324428,
              "its raw timestamp is the one in the bytes");
        check(one[0].timestampMs == 1793380200000, "snapped to the grid");
        check(one[0].stepCount == 1, "one step");
    }
}

int main()
{
    // Before the fixture gate, so it runs even where the private captures
    // are not checked out - this one carries its own bytes.
    checkSingleRecordReply();

    std::ifstream in("fixtures/activity_trend.bin", std::ios::binary);
    const std::vector<uint8_t> payload((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
    check(!payload.empty(), "fixture read");
    if (payload.empty())
        return 1;

    const std::map<int64_t, CloudEntry> cloud =
            readCloudFixture("fixtures/cloud_247_v1_activity_trend.json");
    check(cloud.size() == 10, "cloud fixture read (" + std::to_string(cloud.size())
                                + " entries, expected 10)");
    if (cloud.empty())
        return 1;

    const std::vector<ActivityTrend::Sample> samples = ActivityTrend::decode(payload);

    // Bounded on both sides, deliberately. An earlier decoder in this
    // project asserted only "more than 40 samples" and happily overran its
    // record array for weeks. The reply is 426 bytes with a 26-byte
    // header, so ten 40-byte records is not merely what was expected - it
    // is every byte the reply contains, and eleven would mean the decoder
    // read past the end.
    check(samples.size() == 10, "exactly 10 samples decoded ("
           + std::to_string(samples.size()) + ")");
    if (samples.empty())
        return 1;

    // Every decoded sample must correspond to a cloud entry, field for
    // field. This is the whole point of the test: heart rate, steps and
    // energy all come from a source that has never seen this code.
    int matched = 0;
    for (const ActivityTrend::Sample &s : samples) {
        const auto it = cloud.find(s.timestampMs);
        if (it == cloud.end()) {
            check(false, "sample at " + std::to_string(s.timestampMs)
                          + " has no cloud counterpart");
            continue;
        }
        const CloudEntry &c = it->second;

        // The cloud stores heart rate in hertz; the watch's byte is bpm.
        const int cloudBpm = static_cast<int>(std::lround(c.hrHz * 60.0));
        check(s.heartRateBpm == cloudBpm,
              "hr " + std::to_string(s.heartRateBpm) + " bpm = cloud "
                + std::to_string(cloudBpm) + " bpm at " + std::to_string(s.timestampMs));
        check(s.stepCount == c.stepCount,
              "steps " + std::to_string(s.stepCount) + " = cloud "
                + std::to_string(c.stepCount));
        check(std::fabs(static_cast<double>(s.energy) - c.energy) < 0.01,
              "energy " + std::to_string(s.energy) + " = cloud "
                + std::to_string(c.energy));
        ++matched;
    }
    check(matched == 10, "all ten samples cross-checked against the cloud");

    // The series is chronological and evenly spaced.
    for (size_t i = 1; i < samples.size(); ++i) {
        check(samples[i].timestampMs - samples[i - 1].timestampMs
                      == ActivityTrend::kSampleIntervalMs,
              "sample " + std::to_string(i) + " is ten minutes after the last");
    }

    // A zero-filled or misaligned tail must not surface as 1970 or 2096.
    for (const ActivityTrend::Sample &s : samples) {
        if (s.timestampMs < 1700000000000LL || s.timestampMs > 1900000000000LL) {
            check(false, "sample dated outside a plausible range");
            break;
        }
    }

    // Truncating the payload mid-record must drop that record rather than
    // read past the end of the buffer.
    std::vector<uint8_t> shortened(payload.begin(), payload.end() - 20);
    const std::vector<ActivityTrend::Sample> partial = ActivityTrend::decode(shortened);
    check(partial.size() == 9, "a truncated final record is dropped, not half-read ("
           + std::to_string(partial.size()) + ")");

    // And a payload with nothing recognisable in it yields nothing, rather
    // than whatever the first forty bytes happen to look like.
    const std::vector<uint8_t> noise(200, 0xAB);
    check(ActivityTrend::decode(noise).empty(), "unrecognisable payload decodes to nothing");

    if (g_failures == 0) {
        std::printf("\nAll assertions passed.\n");
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED.\n", g_failures);
    return 1;
}
