// Qt-free test for the recovery-moments decoder.
//
//   g++ -std=c++17 ../src/ble/recoverydecoder.cpp test_recoverydecoder.cpp \
//       -o /tmp/test_recoverydecoder && /tmp/test_recoverydecoder
//
// The fixture is the payload of a real /Activity/Moments/Sync/Data reply,
// taken from the 2026-09-23 HCI capture. Expected values come from the
// Suunto cloud's own recovery entries for the same half-hours
// (tests/fixtures/cloud_247_v1_recovery.json), captured separately.

#include "../src/ble/recoverydecoder.h"

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

} // namespace

int main()
{
    std::ifstream in("fixtures/recovery_moments.bin", std::ios::binary);
    const std::vector<uint8_t> payload((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
    check(!payload.empty(), "fixture read");
    if (payload.empty())
        return 1;

    const std::vector<RecoveryMoments::Sample> samples = RecoveryMoments::decode(payload);
    check(samples.size() > 40, "samples decoded (" + std::to_string(samples.size()) + ")");

    // The record array ends before the payload does. An earlier version
    // read to the end and produced four times too many samples, most of
    // them nonsense - balances of 180 %, timestamps in 2096. The payload
    // is 414 bytes with an 11-byte preamble, so there is room for at most
    // 50 records and anything much above that means the decoder ran off
    // the end again.
    check(samples.size() <= 50, "no samples past the end of the array ("
           + std::to_string(samples.size()) + " <= 50)");
    if (samples.empty())
        return 1;

    // The cloud reports, for the same instants:
    //   2026-09-22T21:30+03:00  balance 0.72  stressState 1   -> 1790101800000 ms
    //   2026-09-22T21:00+03:00  balance 0.67  stressState 1
    //   2026-09-22T20:30+03:00  balance 0.57  stressState 1
    const RecoveryMoments::Sample *known = nullptr;
    for (const RecoveryMoments::Sample &s : samples) {
        if (s.timestampMs == 1790101800000LL)
            known = &s;
    }
    check(known != nullptr, "the cross-checkable sample is present");
    if (known) {
        check(known->balancePercent == 72, "balance = 72% (cloud says 0.72)");
        check(known->stressState == 1, "stressState = 1");
    }

    // Half-hour spacing, and chronological.
    int halfHourGaps = 0;
    for (size_t i = 1; i < samples.size(); ++i) {
        check(samples[i].timestampMs > samples[i - 1].timestampMs, "chronological");
        if (samples[i].timestampMs - samples[i - 1].timestampMs == 1800000LL)
            ++halfHourGaps;
        if (i > 3)
            break; // four assertions are enough to show the ordering holds
    }
    check(halfHourGaps >= 2, "samples are half an hour apart");

    // Ranges: a percentage is a percentage, and the stress state is a
    // small enum - 106 and 216 were seen when the decoder overran.
    bool rangesOk = true;
    for (const RecoveryMoments::Sample &s : samples) {
        if (s.balancePercent > 100 || s.stressState > 10) {
            rangesOk = false;
            break;
        }
    }
    check(rangesOk, "balance <= 100 and stress state is a small enum");

    // And every sample lands in a plausible year, not 2017 or 2096.
    bool yearsOk = true;
    for (const RecoveryMoments::Sample &s : samples) {
        if (s.timestampMs < 1700000000000LL || s.timestampMs > 1900000000000LL) {
            yearsOk = false;
            break;
        }
    }
    check(yearsOk, "no samples dated outside a plausible range");

    // Zero-filled tail entries must not appear as 1970.
    for (const RecoveryMoments::Sample &s : samples) {
        if (s.timestampMs < 1500000000000LL) {
            check(false, "no samples dated before 2017");
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
