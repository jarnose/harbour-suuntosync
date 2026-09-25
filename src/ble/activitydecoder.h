#pragma once

#include <cstdint>
#include <vector>

// Decodes the watch's daily-activity series - what /Activity/TrendData
// returns (docs/watch-push-resources.md).
//
// Like recovery and unlike sleep, this needs no rendered file: the reply
// carries the records directly. Samples are ten minutes apart, where
// recovery's are thirty.
//
// Forty bytes per record, every field below confirmed against what the
// cloud stores for the same ten minutes - ten records, thirty values, no
// disagreement:
//
//   0..3    float32  energy in JOULES, the same unit and the same number
//                    the cloud's energyConsumption carries (fractions
//                    included - 92109.625 came back to the byte). Joules
//                    confirmed against the watch's own screen, not just
//                    inferred: a day summing to 410306 J showed as 98 kcal
//                    on the watch, and 410306/4184 = 98.1.
//   4..5    uint16   step count
//   6..7    uint16   zero in every observed record
//   8..15   uint64   timestamp, unix MILLISECONDS. Recovery counts
//                    seconds and the sleep file counts microseconds;
//                    all three are different, none announces itself.
//                    Not exactly on the ten-minute mark either - the
//                    observed reply had :00.480, :00.030, :00.200 - so
//                    decode() snaps them to the grid, which is where the
//                    cloud puts its own entries for the same buckets.
//                    Without that, one bucket synced from both sources
//                    would become two rows.
//   16      uint8    0x01 in every observed record
//   17      uint8    heart rate, BEATS PER MINUTE. The cloud stores this
//                    divided by sixty, as hertz - which also settles a
//                    question docs/ had left open on purpose: the watch's
//                    own unit is bpm.
//   18..39  ?        vary between records with no discernible pattern,
//                    sometimes all zero, often looking like whatever was
//                    in the buffer before. Not identified, not guessed at.
namespace ActivityTrend {

struct Sample
{
    // Snapped to the ten-minute grid - this is what gets stored.
    int64_t timestampMs = 0;
    // As the watch actually sent it, jitter and all. Only the fetch loop
    // wants this: it advances its cursor past the last record, and doing
    // that with the snapped value would ask for a moment slightly before
    // the record it already has, which comes back again forever.
    int64_t rawTimestampMs = 0;
    int stepCount = 0;
    int heartRateBpm = 0; // 0 means the watch reported none
    float energy = 0.0f; // joules - the cloud's energyConsumption, unconverted
};

// Chronological. Stops at the first record that doesn't fit the pattern
// rather than reading to the end of the buffer - see the .cpp for why that
// distinction cost this project 196 bogus recovery samples once already.
std::vector<Sample> decode(const std::vector<uint8_t> &payload);

// The interval between samples, exposed because the fetch loop needs it to
// advance its cursor.
constexpr int64_t kSampleIntervalMs = 600000;

} // namespace ActivityTrend
