#pragma once

#include <cstdint>
#include <vector>

// Totals over a span of workouts - what a month's training adds up to.
//
// Qt-free, and separated out for one reason: the same real workout can be in
// the database twice. A workout read off the watch is keyed `ble_<id>` and a
// cloud record of the same outing has the cloud's own key, deliberately in
// its own namespace rather than merged (see AppController's sync). Summing
// the rows would count that morning twice, and a monthly total that is
// visibly double is worse than no monthly total.
//
// So entries whose start times are close together are treated as one
// workout. "Close" is a window rather than a rounding, because the two
// sources do not agree to the second: the watch's start comes from its own
// first sample and the cloud's from its record, and rounding to the minute
// would miss a pair that straddles one.
namespace WorkoutSummary {

struct Entry
{
    int64_t startMs = 0;
    double seconds = 0;   // the workout's own duration
};

struct Totals
{
    int count = 0;
    double seconds = 0;
};

// Two starts within this of each other are the same workout seen twice.
// Wide enough to cover the two sources disagreeing, narrow enough not to
// swallow a second workout somebody started right after the first.
constexpr int64_t kSameWorkoutWindowMs = 90000;

// Entries need not be sorted. `fromMs` is inclusive, `toMs` exclusive.
// Where two entries collapse, the longer duration wins: a truncated copy of
// a workout should not shorten the month.
Totals forRange(std::vector<Entry> entries, int64_t fromMs, int64_t toMs);

} // namespace WorkoutSummary
