#include "logbookdecoder.h"
#include "sbemcontainer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Logbook {

namespace {

constexpr double kEarthRadiusMeters = 6371000.0;
// Gaps longer than this between consecutive chunk-timeline samples are
// treated as an auto-pause (the watch keeps logging wall-clock time through
// a pause, but the app's reported duration/distance/steps don't count it) -
// see logbookdecoder.h and docs/logbook-data-format.md for how this
// threshold was chosen and validated.
constexpr int64_t kActiveGapMaxMs = 3000;
// Max speed specifically uses a tighter window than distance/duration
// above: confirmed on a real captured cycling workout (2026-09-21) that a
// single longer (~3s) gap between GPS fixes can cover an implausible
// distance (a GPS position glitch, not real motion) and dominate the
// point-to-point "max speed" figure - excluding pairs above ~1.5s (GPS
// fixes land close to 1s apart in every capture this project has seen)
// dropped the one outlier and landed on a value matching the real
// reported max speed almost exactly. Distance/duration aren't as
// sensitive to one outlier pair the same way (they sum many gaps rather
// than taking a max), so they keep the wider, already-validated window.
constexpr int64_t kMaxSpeedGapMaxMs = 1500;
// The watch reports this exact byte for "cadence" (chunk 0x16 byte 10)
// when no foot-pod/cadence sensor is present (confirmed on a real
// captured cycling workout with no cadence sensor paired: every single
// sample read exactly 255, not 0 or something plausible-looking) - same
// "sentinel means absent" pattern as heart rate's 0, just a different
// sentinel value.
constexpr uint8_t kNoCadenceSentinel = 255;

// Altitude, per docs/sbem-chunk-map.md (generated from the watch's own
// descriptor table). Chunk 0x15 carries Sample.Altitude as an absolute
// uint16 at byte 18; chunk 0x16 carries int8 deltas against it at byte 8.
// Both decode with the schema's own <MOD> expression, raw/5 - 1000, so the
// stored unit is 0.2 m steps offset by -1000 m.
constexpr uint8_t kAltitudeAbsoluteChunk = 0x15;
constexpr size_t kAltitudeAbsoluteOffset = 18;
constexpr uint8_t kAltitudeDeltaChunk = 0x16;
constexpr size_t kAltitudeDeltaOffset = 8;
constexpr uint16_t kNoAltitudeSentinel = 65535; // the schema's nillable value

double altitudeRawToMeters(int32_t raw) { return raw / 5.0 - 1000.0; }

// Chunk 0x04 is an Altitude calibration event; byte 4 is a little-endian
// int32 metre offset (descriptor 41, Altitude.AltitudeOffset). It corrects
// the samples recorded *before* it - a workout whose barometer hadn't
// settled yet starts hundreds of metres out and this is what pulls it back
// (one real capture had -208 m).
constexpr uint8_t kAltitudeEventChunk = 0x04;
constexpr size_t kAltitudeEventOffsetField = 4;
constexpr size_t kAltitudeEventMinSize = 12;

// Summing every 0.2 m step counts barometric noise as climb (3-4x over the
// real totals on every workout checked). Only commit a rise or fall once it
// exceeds this much, which brought three real workouts to within ~10% of
// their app-reported ascent/descent on average.
constexpr double kAltitudeHysteresisMeters = 2.0;

double toRadians(double degrees)
{
    return degrees * M_PI / 180.0;
}

double haversineMeters(double lat1, double lon1, double lat2, double lon2)
{
    const double p1 = toRadians(lat1);
    const double p2 = toRadians(lat2);
    const double dPhi = toRadians(lat2 - lat1);
    const double dLambda = toRadians(lon2 - lon1);
    const double a = std::sin(dPhi / 2) * std::sin(dPhi / 2)
        + std::cos(p1) * std::cos(p2) * std::sin(dLambda / 2) * std::sin(dLambda / 2);
    return 2 * kEarthRadiusMeters * std::asin(std::sqrt(a));
}

int16_t leadingDelta(const std::vector<uint8_t> &value)
{
    return static_cast<int16_t>(value[0] | (value[1] << 8));
}

struct GpsPoint
{
    uint64_t timeMs;
    double lat;
    double lon;
};

struct CadencePoint
{
    uint64_t timeMs;
    uint8_t cadence;
};

} // namespace

DecodedWorkout decode(const std::vector<uint8_t> &mdsStrippedCompressed)
{
    const std::vector<uint8_t> decompressed = Sbem::heatshrinkDecompress(mdsStrippedCompressed);
    const std::vector<Sbem::Chunk> chunks = Sbem::parseContainer(decompressed);

    DecodedWorkout result;

    bool haveCurrentMs = false;
    uint64_t currentMs = 0;
    bool haveFirstMs = false;
    uint64_t firstMs = 0;
    uint64_t lastMs = 0;

    std::vector<GpsPoint> gpsPoints;
    std::vector<CadencePoint> cadencePoints;
    std::vector<uint8_t> heartRateSamples;

    // Altitude, in raw units, kept unconverted until the calibration offset
    // (if any) has been applied - see kAltitudeEventChunk.
    std::vector<int32_t> altitudeRaw;
    bool haveAltitudeRaw = false;
    int32_t currentAltitudeRaw = 0;
    bool haveAltitudeEvent = false;
    int32_t altitudeEventOffsetMeters = 0;
    size_t altitudeSamplesBeforeEvent = 0;

    for (const Sbem::Chunk &chunk : chunks) {
        if (chunk.id == 0x0c && chunk.value.size() == 20) {
            currentMs = 0;
            std::memcpy(&currentMs, chunk.value.data() + 2, sizeof(uint64_t));
            haveCurrentMs = true;
        } else if (chunk.id != 0x01 && chunk.value.size() >= 2 && haveCurrentMs) {
            currentMs = static_cast<uint64_t>(static_cast<int64_t>(currentMs) + leadingDelta(chunk.value));
        }

        if (haveCurrentMs) {
            if (!haveFirstMs) {
                firstMs = currentMs;
                haveFirstMs = true;
            }
            lastMs = currentMs;
        }

        if (chunk.id == 0x0c && chunk.value.size() == 20) {
            int32_t latRaw = 0, lonRaw = 0;
            std::memcpy(&latRaw, chunk.value.data() + 10, sizeof(int32_t));
            std::memcpy(&lonRaw, chunk.value.data() + 14, sizeof(int32_t));
            gpsPoints.push_back({currentMs, latRaw / 1e7, lonRaw / 1e7});
        } else if (chunk.id == 0x12 && chunk.value.size() == 3) {
            const uint8_t hr = chunk.value[2];
            if (hr > 0)
                heartRateSamples.push_back(hr);
        } else if (chunk.id == 0x08 && chunk.value.size() >= 3) {
            result.activityId = chunk.value[2];
        } else if (chunk.id == 0x16 && chunk.value.size() == 17) {
            cadencePoints.push_back({currentMs, chunk.value[10]});
        }

        if (chunk.id == kAltitudeAbsoluteChunk
                && chunk.value.size() >= kAltitudeAbsoluteOffset + sizeof(uint16_t)) {
            uint16_t raw = 0;
            std::memcpy(&raw, chunk.value.data() + kAltitudeAbsoluteOffset, sizeof(uint16_t));
            if (raw != kNoAltitudeSentinel) {
                currentAltitudeRaw = raw;
                haveAltitudeRaw = true;
                altitudeRaw.push_back(currentAltitudeRaw);
            }
        } else if (chunk.id == kAltitudeDeltaChunk && haveAltitudeRaw
                && chunk.value.size() > kAltitudeDeltaOffset) {
            currentAltitudeRaw += static_cast<int8_t>(chunk.value[kAltitudeDeltaOffset]);
            altitudeRaw.push_back(currentAltitudeRaw);
        } else if (chunk.id == kAltitudeEventChunk && !haveAltitudeEvent
                && chunk.value.size() >= kAltitudeEventMinSize) {
            std::memcpy(&altitudeEventOffsetMeters,
                        chunk.value.data() + kAltitudeEventOffsetField, sizeof(int32_t));
            haveAltitudeEvent = true;
            altitudeSamplesBeforeEvent = altitudeRaw.size();
        }
    }

    if (!altitudeRaw.empty()) {
        std::vector<double> altitude;
        altitude.reserve(altitudeRaw.size());
        for (size_t i = 0; i < altitudeRaw.size(); ++i) {
            double meters = altitudeRawToMeters(altitudeRaw[i]);
            if (haveAltitudeEvent && i < altitudeSamplesBeforeEvent)
                meters += altitudeEventOffsetMeters;
            altitude.push_back(meters);
        }

        result.hasAltitude = true;
        result.minAltitudeMeters = *std::min_element(altitude.begin(), altitude.end());
        result.maxAltitudeMeters = *std::max_element(altitude.begin(), altitude.end());

        double reference = altitude.front();
        for (double meters : altitude) {
            const double change = meters - reference;
            if (change >= kAltitudeHysteresisMeters) {
                result.totalAscentMeters += change;
                reference = meters;
            } else if (-change >= kAltitudeHysteresisMeters) {
                result.totalDescentMeters += -change;
                reference = meters;
            }
        }
    }

    result.startTimeMs = firstMs;
    result.stopTimeMs = lastMs;

    int64_t activeTimeMs = 0;
    for (size_t i = 1; i < gpsPoints.size(); ++i) {
        const int64_t dt = static_cast<int64_t>(gpsPoints[i].timeMs) - static_cast<int64_t>(gpsPoints[i - 1].timeMs);
        if (dt > 0 && dt <= kActiveGapMaxMs)
            activeTimeMs += dt;
    }
    result.totalTimeSeconds = activeTimeMs / 1000.0;

    double totalDistance = 0.0;
    double maxSpeed = 0.0;
    for (size_t i = 1; i < gpsPoints.size(); ++i) {
        const int64_t dtMs = static_cast<int64_t>(gpsPoints[i].timeMs) - static_cast<int64_t>(gpsPoints[i - 1].timeMs);
        if (dtMs > 0 && dtMs <= kActiveGapMaxMs) {
            const double dtSeconds = dtMs / 1000.0;
            const double d = haversineMeters(gpsPoints[i - 1].lat, gpsPoints[i - 1].lon,
                gpsPoints[i].lat, gpsPoints[i].lon);
            totalDistance += d;
            if (dtMs <= kMaxSpeedGapMaxMs) {
                const double speed = d / dtSeconds;
                if (speed > maxSpeed)
                    maxSpeed = speed;
            }
        }
    }
    result.totalDistanceMeters = totalDistance;
    result.maxSpeedMs = maxSpeed;

    if (!heartRateSamples.empty()) {
        double sum = 0;
        uint8_t maxHr = 0;
        for (uint8_t hr : heartRateSamples) {
            sum += hr;
            if (hr > maxHr)
                maxHr = hr;
        }
        result.avgHeartRateBpm = sum / heartRateSamples.size();
        result.maxHeartRateBpm = maxHr;
    }

    double steps = 0.0;
    for (size_t i = 1; i < cadencePoints.size(); ++i) {
        const int64_t dtMs = static_cast<int64_t>(cadencePoints[i].timeMs) - static_cast<int64_t>(cadencePoints[i - 1].timeMs);
        if (dtMs > 0 && dtMs <= kActiveGapMaxMs
                && cadencePoints[i].cadence != kNoCadenceSentinel
                && cadencePoints[i - 1].cadence != kNoCadenceSentinel) {
            const double dtSeconds = dtMs / 1000.0;
            steps += cadencePoints[i].cadence * 2.0 / 60.0 * dtSeconds;
        }
    }
    result.stepCount = static_cast<int>(steps + 0.5);

    return result;
}

} // namespace Logbook
