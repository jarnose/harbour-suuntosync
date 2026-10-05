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
        // Everything starting within the window of this one is the same
        // workout. Chained deliberately: the window is measured from the
        // previous member rather than the first, so three copies a minute
        // apart each collapse into the same one.
        Entry merged = entries[i];
        size_t j = i + 1;
        while (j < entries.size() && entries[j].startMs - merged.startMs <= kSameWorkoutWindowMs) {
            merged.seconds = std::max(merged.seconds, entries[j].seconds);
            merged.tss = std::max(merged.tss, entries[j].tss);
            merged.startMs = entries[j].startMs;
            ++j;
        }
        // The earliest start of the group is the one to keep: it is the
        // workout's own, and a day boundary should fall where it fell.
        merged.startMs = entries[i].startMs;
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
