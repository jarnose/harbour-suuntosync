#include "summarydecoder.h"
#include "sbemcontainer.h"
#include "sbemlayout.h"

#include <cstring>

namespace Summary {

namespace {

// Which chunk is the header, and where each field sits inside it, now
// come from the watch's own descriptor table rather than from a Race's
// (see SbemLayout). The offsets happen to be identical on the two watches
// measured - a 9 Baro's header group is shorter, but only because the
// Race appends fields after everything read here - and the chunk id does
// not: 0x1b on a Race, 0x1e on a 9 Baro.

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
    return decode(payload, SbemLayout::builtin());
}

DecodedSummary decode(const std::vector<uint8_t> &payload, const SbemLayout::Layout &layout)
{
    DecodedSummary result;

    const SbemLayout::Header &h = layout.header;
    if (layout.headerGroup == 0 || h.minimumSize == 0)
        return result;

    // A field this build doesn't find in the watch's table stays absent
    // rather than being read from a guessed offset.
    auto has = [](int offset) { return offset >= 0; };

    for (const Sbem::Chunk &chunk : Sbem::parseContainer(payload)) {
        if (chunk.id != layout.headerGroup
                || chunk.value.size() < static_cast<size_t>(h.minimumSize)) {
            continue;
        }

        const std::vector<uint8_t> &v = chunk.value;
        result.valid = true;
        // local64, not plain epoch milliseconds - see Sbem::decodeLocal64().
        if (has(h.dateTime))
            result.startTimeMs = Sbem::decodeLocal64(readAt<uint64_t>(v, h.dateTime));
        if (has(h.duration))
            result.durationSeconds = readAt<uint32_t>(v, h.duration) / 1000.0;
        if (has(h.pauseDuration))
            result.pauseDurationSeconds = readAt<uint32_t>(v, h.pauseDuration) / 1000.0;
        result.movingTimeSeconds = result.durationSeconds - result.pauseDurationSeconds;
        if (has(h.distance))
            result.distanceMeters = readAt<uint32_t>(v, h.distance);
        if (has(h.stepCount))
            result.stepCount = static_cast<int>(readAt<uint32_t>(v, h.stepCount));
        if (has(h.activityType))
            result.activityId = readAt<int32_t>(v, h.activityType);

        const float ascent = has(h.ascent) ? readAt<float>(v, h.ascent) : kAbsent;
        const float descent = has(h.descent) ? readAt<float>(v, h.descent) : kAbsent;
        if (ascent != kAbsent || descent != kAbsent) {
            result.hasAscent = true;
            result.ascentMeters = ascent;
            result.descentMeters = descent;
        }

        const float altMax = has(h.altitudeMax) ? readAt<float>(v, h.altitudeMax) : kAbsent;
        const float altMin = has(h.altitudeMin) ? readAt<float>(v, h.altitudeMin) : kAbsent;
        if (altMax != kAbsent || altMin != kAbsent) {
            result.hasAltitude = true;
            result.maxAltitudeMeters = altMax;
            result.minAltitudeMeters = altMin;
        }

        const float energy = has(h.energy) ? readAt<float>(v, h.energy) : kAbsent;
        if (energy != kAbsent) {
            result.hasEnergy = true;
            result.energyKcal = energy / kJoulesPerKcal;
        }

        const float epoc = has(h.epoc) ? readAt<float>(v, h.epoc) : kAbsent;
        if (epoc != kAbsent) {
            result.hasEpoc = true;
            result.epoc = epoc;
        }
        const float pte = has(h.peakTrainingEffect) ? readAt<float>(v, h.peakTrainingEffect) : kAbsent;
        if (pte != kAbsent) {
            result.hasPeakTrainingEffect = true;
            result.peakTrainingEffect = pte;
        }
        const uint32_t recovery = has(h.recoveryTime) ? readAt<uint32_t>(v, h.recoveryTime) : 0;
        if (recovery != 0) {
            result.hasRecoveryTime = true;
            result.recoveryTimeSeconds = recovery;
        }
        const float vo2 = has(h.maxVo2) ? readAt<float>(v, h.maxVo2) : kAbsent;
        if (vo2 != kAbsent) {
            result.hasMaxVo2 = true;
            result.maxVo2 = vo2;
        }
        const float load = has(h.trainingLoad) ? readAt<float>(v, h.trainingLoad) : kAbsent;
        if (load != kAbsent) {
            result.hasTrainingLoad = true;
            result.trainingLoad = load;
        }
        break;
    }

    return result;
}

} // namespace Summary
