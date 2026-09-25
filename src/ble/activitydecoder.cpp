#include "activitydecoder.h"

#include <algorithm>
#include <cstring>

namespace ActivityTrend {

namespace {

constexpr size_t kRecordSize = 40;
constexpr size_t kTimestampOffset = 8;

// Milliseconds, so the window is the same one the other decoders use with
// three more zeros on it.
constexpr int64_t kEarliestPlausible = 1500000000000LL; // 2017
constexpr int64_t kLatestPlausible   = 4000000000000LL; // 2096

// The watch was not worn, or was off. A week is generous; the point of the
// limit is that an arbitrary jump means the record array has ended, not
// that somebody took a long holiday.
constexpr int64_t kMaxGapMs = 7LL * 24 * 3600 * 1000;

// The watch does not sample exactly on the ten-minute mark. The observed
// reply had timestamps at :00.480, :00.030, :00.200 and so on - always
// within a second, never exact. So "one interval apart" has to mean
// approximately, and two seconds is comfortably wider than anything seen
// while being far narrower than the interval itself.
constexpr int64_t kJitterMs = 2000;

// How many whole intervals a gap spans, or 0 if it isn't a whole number of
// them within the jitter allowance.
int64_t intervalsInGap(int64_t gap)
{
    if (gap <= 0)
        return 0;
    const int64_t n = (gap + kSampleIntervalMs / 2) / kSampleIntervalMs;
    if (n < 1)
        return 0;
    const int64_t error = gap - n * kSampleIntervalMs;
    return (error <= kJitterMs && error >= -kJitterMs) ? n : 0;
}

// Snapped to the ten-minute grid, which is what the cloud stores: its own
// entries for these same buckets are all exactly on the mark. Keeping the
// watch's jitter would be more faithful to the bytes and worse for the
// app, because a bucket synced from the watch and the same bucket synced
// from the cloud would then land in two different rows of health_entries
// and show up twice.
int64_t snapToGrid(int64_t ms)
{
    const int64_t half = kSampleIntervalMs / 2;
    return ((ms + half) / kSampleIntervalMs) * kSampleIntervalMs;
}

// A fragment held ten records in the one reply observed, and the reply is
// capped well below this. The limit exists so a malformed payload cannot
// turn into an enormous vector, not because 4096 means anything.
constexpr size_t kMaxRecords = 4096;

int64_t timestampAt(const std::vector<uint8_t> &payload, size_t pos)
{
    if (pos + kTimestampOffset + 8 > payload.size())
        return 0;
    uint64_t ms = 0;
    std::memcpy(&ms, payload.data() + pos + kTimestampOffset, sizeof(ms));
    return static_cast<int64_t>(ms);
}

bool plausibleTimestamp(int64_t ms)
{
    return ms >= kEarliestPlausible && ms <= kLatestPlausible;
}

// The reply carries a header before the record array - twenty-six bytes in
// the one that has been seen - and this project has no basis for claiming
// what those bytes are or that the length is fixed. So rather than
// hard-coding an offset from a single sample, find the array by its own
// rhythm: two records one stride apart whose timestamps differ by exactly
// one sample interval.
//
// Both conditions matter. Plausibility alone is not enough; the recovery
// decoder tried that and locked on two bytes early, because bytes
// straddling a record boundary happened to read as a number in range.
size_t findRecordStart(const std::vector<uint8_t> &payload)
{
    for (size_t pos = 0; pos + 2 * kRecordSize <= payload.size(); ++pos) {
        const int64_t first = timestampAt(payload, pos);
        const int64_t second = timestampAt(payload, pos + kRecordSize);
        if (!plausibleTimestamp(first) || !plausibleTimestamp(second))
            continue;
        if (intervalsInGap(second - first) == 1)
            return pos;
    }
    return payload.size(); // nothing recognisable
}

} // namespace

std::vector<Sample> decode(const std::vector<uint8_t> &payload)
{
    std::vector<Sample> out;
    const size_t start = findRecordStart(payload);

    // Stop at the first record that doesn't fit, rather than skipping it
    // and carrying on. The recovery decoder learned this the hard way: its
    // record array ended before the payload did, and reading to the end
    // produced 196 bogus samples alongside 50 real ones. Whatever follows
    // the array is not records.
    int64_t previous = 0;
    for (size_t pos = start; pos + kRecordSize <= payload.size(); pos += kRecordSize) {
        const int64_t ms = timestampAt(payload, pos);
        if (!plausibleTimestamp(ms))
            break;

        if (previous != 0) {
            // Ten minutes apart. A gap is allowed - the watch comes off -
            // but only a whole number of intervals, and not an unbounded
            // one. Anything else means the array has ended.
            const int64_t gap = ms - previous;
            if (gap > kMaxGapMs || intervalsInGap(gap) == 0)
                break;
        }

        Sample sample;
        sample.timestampMs = snapToGrid(ms);
        sample.rawTimestampMs = ms;
        std::memcpy(&sample.energy, payload.data() + pos, sizeof(sample.energy));
        uint16_t steps = 0;
        std::memcpy(&steps, payload.data() + pos + 4, sizeof(steps));
        sample.stepCount = steps;
        sample.heartRateBpm = payload[pos + 17];
        // Bytes 18..39 are left alone: they vary between records and there
        // is no evidence here for what they mean.
        out.push_back(sample);
        previous = ms;

        if (out.size() >= kMaxRecords)
            break;
    }

    std::sort(out.begin(), out.end(), [](const Sample &a, const Sample &b) {
        return a.timestampMs < b.timestampMs;
    });
    return out;
}

} // namespace ActivityTrend
