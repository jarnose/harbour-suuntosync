#pragma once

#include <cstdint>
#include <vector>

// Decodes the watch's recovery series - what /Activity/Moments/Sync/Data
// returns (docs/watch-push-resources.md).
//
// Unlike sleep, this needs no rendered file: the reply carries the data
// directly, in the same paged framing /Summary uses.
//
// The record layout is eight bytes, confirmed against what the cloud
// reports for the same half-hours:
//
//   0..3  uint32  timestamp, unix SECONDS (not milliseconds, not the
//                 microseconds the sleep file uses)
//   4     uint8   resource balance, percent - the cloud's 0..1 x 100
//   5     uint8   stress state, the watch's own enum
//   6..7  ?       vary between records; not identified, not guessed at
//
// Samples are half an hour apart.
namespace RecoveryMoments {

struct Sample
{
    int64_t timestampMs = 0;  // converted to milliseconds for the rest of the app
    int balancePercent = 0;   // 0..100
    int stressState = 0;
};

// Chronological. Records whose timestamp is outside a plausible range are
// skipped: the reply is a fixed-size array and a partially filled tail
// would otherwise decode into garbage dated 1970.
std::vector<Sample> decode(const std::vector<uint8_t> &payload);

} // namespace RecoveryMoments
