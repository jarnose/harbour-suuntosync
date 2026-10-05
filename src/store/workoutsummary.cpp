#include "workoutsummary.h"

#include <algorithm>

namespace WorkoutSummary {

Totals forRange(std::vector<Entry> entries, int64_t fromMs, int64_t toMs)
{
    std::vector<Entry> inRange;
    for (const Entry &entry : entries) {
        if (entry.startMs >= fromMs && entry.startMs < toMs)
            inRange.push_back(entry);
    }

    std::sort(inRange.begin(), inRange.end(), [](const Entry &a, const Entry &b) {
        return a.startMs < b.startMs;
    });

    Totals totals;
    size_t i = 0;
    while (i < inRange.size()) {
        // Everything starting within the window of this one is the same
        // workout. Chained deliberately: the window is measured from the
        // previous member rather than the first, so three copies a minute
        // apart each collapse into the same one.
        double longest = inRange[i].seconds;
        int64_t previous = inRange[i].startMs;
        size_t j = i + 1;
        while (j < inRange.size() && inRange[j].startMs - previous <= kSameWorkoutWindowMs) {
            longest = std::max(longest, inRange[j].seconds);
            previous = inRange[j].startMs;
            ++j;
        }
        totals.count += 1;
        totals.seconds += longest;
        i = j;
    }
    return totals;
}

} // namespace WorkoutSummary
