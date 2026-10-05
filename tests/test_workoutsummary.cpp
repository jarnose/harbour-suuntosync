// Qt-free test for the monthly totals, and in particular for the one thing
// that is not arithmetic: the same workout being in the database twice.
//
//   g++ -std=c++17 ../src/store/workoutsummary.cpp test_workoutsummary.cpp -o /tmp/t && /tmp/t

#include "../src/store/workoutsummary.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

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

    // Three copies a minute apart each - chained, so still one workout.
    const WorkoutSummary::Totals chained =
            run({ { kOct + 1000, 10 }, { kOct + 61000, 20 }, { kOct + 121000, 30 } });
    check(chained.count == 1 && chained.seconds == 30,
          "copies chained a minute apart collapse into one ("
                  + std::to_string(chained.count) + ")");

    // The range is half-open, and anything outside it is ignored.
    const WorkoutSummary::Totals edges =
            run({ { kOct - 1, 600 }, { kOct, 600 }, { kNov - 1, 600 }, { kNov, 600 } });
    check(edges.count == 2, "the range includes its start and excludes its end ("
                                    + std::to_string(edges.count) + ")");

    if (g_failures == 0) {
        std::printf("\nAll assertions passed.\n");
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED.\n", g_failures);
    return 1;
}
