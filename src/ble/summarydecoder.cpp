#include "summarydecoder.h"
#include "sbemcontainer.h"

#include <cstring>

namespace Summary {

namespace {

// The Header group, chunk 0x1b (descriptor 27). Its first 448 bytes are all
// fixed-size fields, so the offsets below are just the running sum of the
// field sizes listed in docs/sbem-chunk-map.md - every one of them was
// checked against a real captured Summary before being written down.
constexpr uint16_t kHeaderChunk = 0x1b;

constexpr size_t kOffsetDateTime = 8;       // local64, ms since epoch
constexpr size_t kOffsetDuration = 16;      // uint32, milliseconds
constexpr size_t kOffsetPauseDuration = 20; // uint32, milliseconds
constexpr size_t kOffsetDistance = 24;      // uint32, metres
constexpr size_t kOffsetStepCount = 28;     // uint32
constexpr size_t kOffsetActivityType = 36;  // int32
constexpr size_t kOffsetAscent = 40;        // float32, metres
constexpr size_t kOffsetDescent = 48;       // float32, metres
constexpr size_t kOffsetAltitudeMax = 60;   // float32, metres
constexpr size_t kOffsetAltitudeMin = 64;   // float32, metres
constexpr size_t kOffsetEnergy = 68;        // float32, joules

// Everything this decoder reads sits within the first 72 bytes; refuse a
// chunk too short to hold them rather than reading past the end.
constexpr size_t kMinHeaderSize = kOffsetEnergy + sizeof(float);

// 1 kcal = 4184 J (thermochemical), the same constant the cloud API's
// energyConsumption figures line up with.
constexpr double kJoulesPerKcal = 4184.0;

// The schema marks ascent/descent/energy nillable=0, i.e. zero means "not
// recorded" rather than "genuinely zero".
constexpr float kAbsent = 0.0f;

template <typename T>
T readAt(const std::vector<uint8_t> &v, size_t offset)
{
    T out{};
    std::memcpy(&out, v.data() + offset, sizeof(T));
    return out;
}

} // namespace

DecodedSummary decode(const std::vector<uint8_t> &payload)
{
    DecodedSummary result;

    for (const Sbem::Chunk &chunk : Sbem::parseContainer(payload)) {
        if (chunk.id != kHeaderChunk || chunk.value.size() < kMinHeaderSize)
            continue;

        const std::vector<uint8_t> &v = chunk.value;
        result.valid = true;
        result.startTimeMs = readAt<uint64_t>(v, kOffsetDateTime);
        result.durationSeconds = readAt<uint32_t>(v, kOffsetDuration) / 1000.0;
        result.pauseDurationSeconds = readAt<uint32_t>(v, kOffsetPauseDuration) / 1000.0;
        result.movingTimeSeconds = result.durationSeconds - result.pauseDurationSeconds;
        result.distanceMeters = readAt<uint32_t>(v, kOffsetDistance);
        result.stepCount = static_cast<int>(readAt<uint32_t>(v, kOffsetStepCount));
        result.activityId = readAt<int32_t>(v, kOffsetActivityType);

        const float ascent = readAt<float>(v, kOffsetAscent);
        const float descent = readAt<float>(v, kOffsetDescent);
        if (ascent != kAbsent || descent != kAbsent) {
            result.hasAscent = true;
            result.ascentMeters = ascent;
            result.descentMeters = descent;
        }

        const float altMax = readAt<float>(v, kOffsetAltitudeMax);
        const float altMin = readAt<float>(v, kOffsetAltitudeMin);
        if (altMax != kAbsent || altMin != kAbsent) {
            result.hasAltitude = true;
            result.maxAltitudeMeters = altMax;
            result.minAltitudeMeters = altMin;
        }

        const float energy = readAt<float>(v, kOffsetEnergy);
        if (energy != kAbsent) {
            result.hasEnergy = true;
            result.energyKcal = energy / kJoulesPerKcal;
        }
        break;
    }

    return result;
}

} // namespace Summary
