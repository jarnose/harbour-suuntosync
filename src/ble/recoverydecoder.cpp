#include "recoverydecoder.h"

#include <algorithm>
#include <cstring>

namespace RecoveryMoments {

namespace {

constexpr size_t kRecordSize = 8;

// The watch ships fixed-size arrays with unused entries left as zero, and a
// zero timestamp would decode to 1970. Anything outside a sane window is
// dropped rather than shown - the same rule the sleep decoder uses.
constexpr int64_t kEarliestPlausible = 1500000000; // 2017
constexpr int64_t kLatestPlausible   = 4000000000; // 2096

// Samples are half an hour apart - confirmed across the whole captured
// reply, and used to find where the records begin.
constexpr uint32_t kSampleIntervalSeconds = 1800;

// A run may skip samples, but not by an arbitrary amount. A week is
// generous for "the watch was off"; the bogus records this guards against
// jumped by years.
constexpr uint32_t kMaxGapSeconds = 7 * 24 * 3600;

} // namespace

namespace {

uint32_t timestampAt(const std::vector<uint8_t> &payload, size_t pos)
{
    if (pos + 4 > payload.size())
        return 0;
    uint32_t seconds = 0;
    std::memcpy(&seconds, payload.data() + pos, sizeof(seconds));
    return seconds;
}

bool plausibleTimestamp(uint32_t seconds)
{
    return seconds >= kEarliestPlausible && seconds <= kLatestPlausible;
}

// The reply carries some bytes before the record array - eleven in the
// captured one - and this project has no basis for claiming what they are.
// Rather than hard-code an offset from one sample, find where the records
// start by looking for the data's own rhythm: two consecutive plausible
// timestamps exactly one interval apart.
//
// "Plausible and plausible" alone is not enough. Tried that first, and it
// locked on two bytes early, because the four bytes straddling a record
// boundary happened to read as a number in range. Requiring the spacing as
// well is what makes it unambiguous.
size_t findRecordStart(const std::vector<uint8_t> &payload)
{
    for (size_t pos = 0; pos + 2 * kRecordSize <= payload.size(); ++pos) {
        const uint32_t first = timestampAt(payload, pos);
        const uint32_t second = timestampAt(payload, pos + kRecordSize);
        if (!plausibleTimestamp(first) || !plausibleTimestamp(second))
            continue;
        if (second - first == kSampleIntervalSeconds)
            return pos;
    }
    return payload.size(); // nothing recognisable
}

} // namespace

std::vector<Sample> decode(const std::vector<uint8_t> &payload)
{
    std::vector<Sample> out;
    const size_t start = findRecordStart(payload);

    // Stop at the first record that doesn't fit the pattern, rather than
    // skipping it and carrying on.
    //
    // This matters, and the first version got it wrong: the record array
    // ends before the payload does, and reading to the end produced 196
    // bogus samples alongside 50 real ones - balances of 180 %, stress
    // states of 216, timestamps in 2017 and 2096. Whatever follows the
    // array is not records, so the honest thing is to stop when the run
    // stops looking like one.
    uint32_t previous = 0;
    for (size_t pos = start; pos + kRecordSize <= payload.size(); pos += kRecordSize) {
        const uint32_t seconds = timestampAt(payload, pos);
        if (!plausibleTimestamp(seconds))
            break;

        const int balance = payload[pos + 4];
        if (balance > 100)
            break; // a percentage cannot exceed 100

        if (previous != 0) {
            // Samples are half an hour apart. Gaps happen - the watch is
            // taken off - so a multiple is allowed, but an arbitrary jump
            // means the array has ended.
            const uint32_t gap = seconds - previous;
            if (seconds <= previous || gap % kSampleIntervalSeconds != 0
                    || gap > kMaxGapSeconds) {
                break;
            }
        }

        Sample sample;
        sample.timestampMs = static_cast<int64_t>(seconds) * 1000;
        sample.balancePercent = balance;
        sample.stressState = payload[pos + 5];
        // Bytes 6 and 7 are left alone: they vary between records and this
        // project has no evidence for what they mean.
        out.push_back(sample);
        previous = seconds;
    }

    std::sort(out.begin(), out.end(), [](const Sample &a, const Sample &b) {
        return a.timestampMs < b.timestampMs;
    });
    return out;
}

} // namespace RecoveryMoments
