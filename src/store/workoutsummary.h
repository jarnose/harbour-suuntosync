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
    double tss = 0;       // Suunto's own trainingStressScore, 0 when absent
};

struct Totals
{
    int count = 0;
    double seconds = 0;
};

// Chronic and acute training load and the balance between them - what
// Suunto's own app calls Progress, and what cycling software has called
// CTL, ATL and TSB for twenty years.
//
// They are exponentially weighted moving averages of daily training stress
// with time constants of 42 and 7 days:
//
//   CTL(today) = CTL(yesterday) + (TSS(today) - CTL(yesterday)) / 42
//   ATL(today) = ATL(yesterday) + (TSS(today) - ATL(yesterday)) / 7
//   TSB(today) = CTL(yesterday) - ATL(yesterday)
//
// TSB is deliberately yesterday's difference, which is the convention
// everywhere: today's training has not made you fresher.
//
// **The stress figures are Suunto's own.** Their cloud returns a
// trainingStressScore per workout and this project stores it; nothing here
// invents a load from duration or heart rate. A number that looked like
// Suunto's and was not would be worse than no number.
struct Progress
{
    double ctl = 0;
    double atl = 0;
    double tsb = 0;
};

// `dailyTss` is one entry per calendar day, oldest first, with zeroes for
// rest days - the averages are over days, so the gaps have to be there.
// The last entry is today. Day bucketing is the caller's job because it
// needs a timezone, and this file has no Qt in it.
Progress progressFromDailyLoad(const std::vector<double> &dailyTss);

// Two starts within this of each other are the same workout seen twice.
// Wide enough to cover the two sources disagreeing, narrow enough not to
// swallow a second workout somebody started right after the first.
constexpr int64_t kSameWorkoutWindowMs = 90000;

// Sorts by start time and merges entries that are the same workout seen
// twice, keeping the longer duration and the larger stress score: a
// truncated copy must not shorten the month, and a watch copy with no score
// must not erase the cloud's.
//
// Separate from the totals because the day bucketing on the Qt side needs
// it too, and doing it there by hand got it wrong - replacing a day's
// running total with one entry's score instead of merging the pair first.
std::vector<Entry> collapseDuplicates(std::vector<Entry> entries);

// Entries need not be sorted. `fromMs` is inclusive, `toMs` exclusive.
Totals forRange(std::vector<Entry> entries, int64_t fromMs, int64_t toMs);

} // namespace WorkoutSummary
