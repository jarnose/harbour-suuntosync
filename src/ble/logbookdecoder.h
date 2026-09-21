#pragma once

#include <cstdint>
#include <vector>

// Turns a fully-reassembled /Logbook/byId/<id>/Data payload into workout
// summary fields, Qt-free (STL only) so it can be golden-vector-tested with
// plain g++ before it's wired into a BLE-facing (Qt) class - same discipline
// as mdswirecodec.cpp/suuntoauth.cpp. Builds on Sbem::heatshrinkDecompress/
// parseContainer (sbemcontainer.h) - see docs/logbook-data-format.md for the
// full pipeline this sits at the end of, including exactly which fields
// below are confirmed against real captures vs. still open.
//
// Only fields actually confirmed there are populated:
//   - activityId: CHUNK_ACTIVITY (0x08)'s sportId byte.
//   - startTimeMs/stopTimeMs: chunk 0x0c's absolute UTC timestamp (bytes
//     2-8) - the first/last value seen. Confirmed to match real per-second
//     GPS-app timestamps on every sample checked (100% match rate across
//     three real workouts).
//   - totalTimeSeconds: sum of gaps between consecutive 0x0c samples that
//     are 3s or shorter - an approximation of the auto-pause-excluded
//     "active" duration the app itself reports (matching how this project
//     validated distance/steps against real totals), not an exact
//     reproduction of whatever boundary rule the watch/app actually uses.
//   - totalDistanceMeters/maxSpeedMs: derived (not stored) from consecutive
//     GPS fixes in chunk 0x0c - haversine distance summed across the same
//     "active" gaps, confirmed within 0.4% of real reported totals; speed
//     as distance/time between fixes, confirmed to correlate strongly with
//     real speed for cycling-paced movement, noisier at walking pace (GPS's
//     few-metre noise floor is a larger fraction of the true per-second
//     movement) - see doc for the full explanation.
//   - avgHeartRateBpm/maxHeartRateBpm: chunk 0x12 byte 2 (uint8 bpm,
//     samples of 0 excluded as "no reading") - confirmed exact (the real
//     max bpm matched byte-for-byte on all three real workouts).
//   - stepCount: integrated from chunk 0x16 byte 10 (cadence, uint8 rpm) -
//     steps = cadence x2 (cadence is per-foot) / 60 x dt, summed across the
//     same "active" gaps - confirmed within ~2% of the real total, not
//     exact (see doc for why the residual is believed to be this project's
//     own pause-boundary heuristic rather than a wrong idea).
//   - totalAscentMeters/totalDescentMeters/energyConsumptionKcal are
//     deliberately left out entirely (not even a zero-valued field) -
//     extensively searched for across every chunk type in this resource
//     and not found; see docs/logbook-data-format.md's altitude section
//     before spending more time looking for them here.
namespace Logbook {

struct DecodedWorkout
{
    int activityId = 0;
    uint64_t startTimeMs = 0;
    uint64_t stopTimeMs = 0;
    double totalTimeSeconds = 0;
    double totalDistanceMeters = 0;
    double maxSpeedMs = 0;
    double avgHeartRateBpm = 0; // 0 = no heart rate samples seen
    double maxHeartRateBpm = 0;
    int stepCount = 0;
};

// Throws std::runtime_error if the Heatshrink stream or SBEM0103 container
// doesn't parse (see Sbem::heatshrinkDecompress/parseContainer).
DecodedWorkout decode(const std::vector<uint8_t> &mdsStrippedCompressed);

} // namespace Logbook
