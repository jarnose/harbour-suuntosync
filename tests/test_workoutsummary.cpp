// Qt-free test for the monthly totals, and in particular for the one thing
// that is not arithmetic: the same workout being in the database twice.
//
//   g++ -std=c++17 ../src/store/workoutsummary.cpp test_workoutsummary.cpp -o /tmp/t && /tmp/t

#include "../src/store/workoutsummary.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

bool close(double a, double b, double tolerance = 1e-6)
{
    const double d = a - b;
    return (d < 0 ? -d : d) <= tolerance;
}

void check(bool ok, const std::string &what)
{
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok)
        ++g_failures;
}

// 2026-10-01T00:00:00Z and a month later, which is the shape of the real
// call.
const int64_t kOct = 1790812800000LL;
const int64_t kNov = 1793491200000LL;

WorkoutSummary::Totals run(std::vector<WorkoutSummary::Entry> entries)
{
    return WorkoutSummary::forRange(std::move(entries), kOct, kNov);
}

} // namespace

int main()
{
    check(run({}).count == 0 && run({}).seconds == 0, "an empty month is zero, not a crash");

    // Plain arithmetic.
    const WorkoutSummary::Totals three =
            run({ { kOct + 1000, 600 }, { kOct + 100000000, 1200 }, { kOct + 200000000, 300 } });
    check(three.count == 3, "three workouts count as three (" + std::to_string(three.count) + ")");
    check(three.seconds == 2100, "and their durations add up");

    // The reason this is a function and not a SUM(): the same outing from
    // the watch and from the cloud, a few seconds apart.
    const WorkoutSummary::Totals pair = run({ { kOct + 1000, 2663 }, { kOct + 4000, 2663 } });
    check(pair.count == 1, "the same workout from two sources counts once ("
                                   + std::to_string(pair.count) + ")");
    check(pair.seconds == 2663, "and once only in the total");

    // Order must not matter - the rows arrive newest first.
    const WorkoutSummary::Totals reversed = run({ { kOct + 4000, 2663 }, { kOct + 1000, 2663 } });
    check(reversed.count == 1 && reversed.seconds == 2663, "and the order they arrive in does not");

    // A truncated copy must not shorten the month.
    const WorkoutSummary::Totals truncated = run({ { kOct + 1000, 2663 }, { kOct + 2000, 0 } });
    check(truncated.seconds == 2663, "the longer of two copies is the one that counts");

    // But two workouts genuinely minutes apart are two workouts.
    const WorkoutSummary::Totals back2back = run({ { kOct + 1000, 600 }, { kOct + 121000, 600 } });
    check(back2back.count == 2, "two minutes apart is two workouts ("
                                        + std::to_string(back2back.count) + ")");

    // A run of entries each a minute from the last is NOT one workout. The
    // window is measured from the group's first member, so this is two: the
    // first two collapse and the third is beyond ninety seconds of the
    // first. Chaining instead would swallow a multisport outing recorded as
    // back-to-back legs, which is what the figures looked like against
    // Suunto's own app.
    const WorkoutSummary::Totals run3 =
            run({ { kOct + 1000, 10 }, { kOct + 61000, 20 }, { kOct + 121000, 30 } });
    check(run3.count == 2, "a minute-apart run is not collapsed without limit ("
                                   + std::to_string(run3.count) + ")");
    check(close(run3.seconds, 50.0), "and keeps 20 + 30 rather than 30 alone ("
                                              + std::to_string(run3.seconds) + ")");

    // Two copies seconds apart, which is what a duplicate actually looks
    // like, still collapse.
    const WorkoutSummary::Totals seconds =
            run({ { kOct + 1000, 2663 }, { kOct + 31000, 2663 } });
    check(seconds.count == 1, "copies seconds apart still collapse ("
                                      + std::to_string(seconds.count) + ")");

    // The range is half-open, and anything outside it is ignored. Spaced
    // hours apart so the dedupe window has no say in it.
    const WorkoutSummary::Totals edges =
            run({ { kOct - 3600000, 600 }, { kOct + 3600000, 600 },
                  { kNov - 3600000, 600 }, { kNov + 3600000, 600 } });
    check(edges.count == 2, "the range includes its start and excludes its end ("
                                    + std::to_string(edges.count) + ")");

    // A duplicated pair that straddles the boundary is one workout, and it
    // falls on the side its earliest copy started on. Which side is a
    // choice rather than a fact - the two copies disagree about when the
    // workout began - and this is the one that is made.
    const WorkoutSummary::Totals straddling = run({ { kOct - 1000, 600 }, { kOct + 1000, 600 } });
    check(straddling.count == 0,
          "a pair straddling the start counts on the side its first copy began ("
                  + std::to_string(straddling.count) + ")");

    // collapseDuplicates is used by the day bucketing too, so it is worth
    // testing on its own - and in particular that a day's other workout
    // survives a duplicated pair, which an earlier version got wrong.
    {
        std::vector<WorkoutSummary::Entry> day = {
            { kOct + 0, 600, 10 },           // a workout
            { kOct + 3600000, 600, 5 },      // another, an hour later
            { kOct + 3630000, 600, 30 },     // ...and its duplicate, richer
        };
        const std::vector<WorkoutSummary::Entry> merged =
                WorkoutSummary::collapseDuplicates(day);
        check(merged.size() == 2, "three rows with one duplicate collapse to two ("
                                          + std::to_string(merged.size()) + ")");
        double sum = 0;
        for (const WorkoutSummary::Entry &e : merged)
            sum += e.tss;
        check(close(sum, 40.0), "and the day's stress is 10 + 30, not 30 ("
                                        + std::to_string(sum) + ")");
        check(merged.front().startMs == kOct + 0,
              "the group keeps its earliest start, so a day boundary falls where it fell");
    }

    // ---- CTL, ATL and TSB ----
    //
    // Hand-checkable: one day of 42 moves CTL by exactly 42/42 and ATL by
    // 42/7, and the balance is still zero because yesterday had neither.
    const WorkoutSummary::Progress one = WorkoutSummary::progressFromDailyLoad({ 42 });
    check(close(one.ctl, 1.0), "one day of 42 gives CTL 1 (" + std::to_string(one.ctl) + ")");
    check(close(one.atl, 6.0), "and ATL 6 (" + std::to_string(one.atl) + ")");
    check(close(one.tsb, 0.0), "and a balance of zero, because yesterday was empty");

    const WorkoutSummary::Progress two = WorkoutSummary::progressFromDailyLoad({ 42, 42 });
    check(close(two.ctl, 1.976190) && close(two.atl, 11.142857),
          "a second day of 42 compounds both");
    check(close(two.tsb, -5.0),
          "and the balance is yesterday's difference, 1 - 6 (" + std::to_string(two.tsb) + ")");

    // A rest day: ATL falls six times faster than CTL, which is the whole
    // point of keeping two of them.
    const WorkoutSummary::Progress rest = WorkoutSummary::progressFromDailyLoad({ 42, 0 });
    check(rest.ctl > 0.9 && rest.atl < 5.2, "a rest day drops ATL much further than CTL");
    check(close(rest.tsb, -5.0), "and its balance is still yesterday's");

    // Held constant for long enough, both averages converge on the load and
    // the balance goes to nothing. That is the property that says the time
    // constants are the right way round.
    std::vector<double> steady(300, 50.0);
    const WorkoutSummary::Progress converged = WorkoutSummary::progressFromDailyLoad(steady);
    check(close(converged.ctl, 50.0, 0.05) && close(converged.atl, 50.0, 0.05),
          "a constant load converges both averages on it");
    check(close(converged.tsb, 0.0, 0.05), "and leaves the balance at zero");

    check(WorkoutSummary::progressFromDailyLoad({}).ctl == 0,
          "no history is zero rather than undefined");

    if (g_failures == 0) {
        std::printf("\nAll assertions passed.\n");
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED.\n", g_failures);
    return 1;
}
