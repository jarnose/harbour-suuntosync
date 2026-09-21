#include "logbookdecoder.h"
#include "sbemcontainer.h"

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
