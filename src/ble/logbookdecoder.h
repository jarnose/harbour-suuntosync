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
//   - minAltitudeMeters/maxAltitudeMeters: Sample.Altitude (descriptor 87
//     in docs/sbem-chunk-map.md) - an absolute uint16 in chunk 0x15 and
//     int8 deltas against it in chunk 0x16, decoded as raw/5 - 1000 (the
//     schema's own <MOD> expression). Exact; the earlier "altitude isn't
//     in this resource" conclusion was wrong, it was just delta-encoded
//     and needed that transform. An Altitude calibration event (chunk
//     0x04, descriptor 41) carries a metre offset which is applied to
//     every sample recorded *before* the event - without that, a workout
//     whose barometer hadn't settled at the start reads hundreds of
//     metres off (confirmed against a real capture where the offset was
//     -208 m).
//   - totalAscentMeters/totalDescentMeters: derived from that altitude
//     series with a 2 m hysteresis, since summing every 0.2 m step would
//     count barometric noise as climb (the raw sum over-reads by 3-4x).
//     **Approximate**: validated against three real workouts with known
//     app-reported values, mean error ~10% (worst case ~18%) - noticeably
//     rougher than this decoder's distance (0.4%) or step count (2%).
//     /Logbook/byId/<id>/Summary carries the watch's own exact
//     Header.Ascent/Header.Descent and should be preferred when available.
//   - energyConsumptionKcal is still left out entirely (not even a
//     zero-valued field): it's Header.Energy, which lives in chunk 0x1b,
//     a header group that does not appear in this resource at all - see
//     docs/logbook-data-format.md.
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
    bool hasAltitude = false;   // false = no usable altitude samples in this workout
    double minAltitudeMeters = 0;
    double maxAltitudeMeters = 0;
    double totalAscentMeters = 0;
    double totalDescentMeters = 0;
};

// Throws std::runtime_error if the Heatshrink stream or SBEM0103 container
// doesn't parse (see Sbem::heatshrinkDecompress/parseContainer).
DecodedWorkout decode(const std::vector<uint8_t> &mdsStrippedCompressed);

} // namespace Logbook
