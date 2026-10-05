#include "workoutsummary.h"

#include <algorithm>

namespace WorkoutSummary {

std::vector<Entry> collapseDuplicates(std::vector<Entry> entries)
{
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        return a.startMs < b.startMs;
    });

    std::vector<Entry> out;
    size_t i = 0;
    while (i < entries.size()) {
        // Everything starting within the window of *this* one is the same
        // workout. Measured from the group's first member and not chained
        // from the previous: chaining let a run of entries each 90 seconds
        // from the last collapse into one, however long the run, and a
        // multisport outing recorded as back-to-back legs looks exactly
        // like that. Two copies of one workout differ by seconds, so a
        // window from the first is all a duplicate needs.
        Entry merged = entries[i];
        size_t j = i + 1;
        while (j < entries.size()
                && entries[j].startMs - entries[i].startMs <= kSameWorkoutWindowMs) {
            merged.seconds = std::max(merged.seconds, entries[j].seconds);
            merged.tss = std::max(merged.tss, entries[j].tss);
            ++j;
        }
        out.push_back(merged);
        i = j;
    }
    return out;
}

Totals forRange(std::vector<Entry> entries, int64_t fromMs, int64_t toMs)
{
    Totals totals;
    for (const Entry &entry : collapseDuplicates(std::move(entries))) {
        if (entry.startMs < fromMs || entry.startMs >= toMs)
            continue;
        totals.count += 1;
        totals.seconds += entry.seconds;
    }
    return totals;
}

Progress progressFromDailyLoad(const std::vector<double> &dailyTss)
{
    Progress progress;
    for (double tss : dailyTss) {
        // Yesterday's difference, taken before today's training is folded
        // in: that is what the balance means.
        progress.tsb = progress.ctl - progress.atl;
        progress.ctl += (tss - progress.ctl) / 42.0;
        progress.atl += (tss - progress.atl) / 7.0;
    }
    return progress;
}

} // namespace WorkoutSummary
