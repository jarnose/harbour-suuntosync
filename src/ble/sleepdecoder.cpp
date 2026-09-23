#include "sleepdecoder.h"

#include <algorithm>
#include <cstring>

namespace SleepTimeline {

namespace {

// Chunk ids, all confirmed against a real file - see the header.
constexpr uint16_t kEntryTimestamp = 14;
constexpr uint16_t kSource = 13;
constexpr uint16_t kTimestamp2 = 2;
constexpr uint16_t kDuration = 3;
constexpr uint16_t kOnsetLatency = 4;
constexpr uint16_t kQualityPercent = 5;
constexpr uint16_t kDeep = 6;
constexpr uint16_t kHeartRateMin = 7;
constexpr uint16_t kHeartRateAvg = 8;
constexpr uint16_t kLight = 9;
constexpr uint16_t kMaxSpo2 = 15;
constexpr uint16_t kAltitude = 16;
constexpr uint16_t kHrvAverage = 17;
constexpr uint16_t kHrvSamples = 18;
constexpr uint16_t kSleepId = 19;
constexpr uint16_t kIsNap = 20;
constexpr uint16_t kRem = 21;
constexpr uint16_t kWakeBeforeOffBed = 22;
constexpr uint16_t kWakeAfterOnset = 23;
constexpr uint16_t kStageKind = 25;
constexpr uint16_t kStageStart = 26;
constexpr uint16_t kStageDuration = 27;

float readFloat(const std::vector<uint8_t> &v)
{
    if (v.size() != 4)
        return 0;
    float out = 0;
    std::memcpy(&out, v.data(), sizeof(out));
    return out;
}

uint32_t readU32(const std::vector<uint8_t> &v)
{
    if (v.size() != 4)
        return 0;
    uint32_t out = 0;
    std::memcpy(&out, v.data(), sizeof(out));
    return out;
}

// The file stamps times in microseconds, not the milliseconds the rest of
// this project uses, and not the watch's local64 either.
int64_t readMicrosAsMs(const std::vector<uint8_t> &v)
{
    if (v.size() != 8)
        return 0;
    uint64_t raw = 0;
    std::memcpy(&raw, v.data(), sizeof(raw));
    return static_cast<int64_t>(raw / 1000);
}

// 0xFF is the "no reading" sentinel for the byte-sized fields; -1 carries
// that through to the caller instead of a plausible-looking 255.
int readByteOrMissing(const std::vector<uint8_t> &v)
{
    if (v.empty() || v[0] == 0xFF)
        return -1;
    return v[0];
}

std::string readString(const std::vector<uint8_t> &v)
{
    size_t n = 0;
    while (n < v.size() && v[n] != '\0')
        ++n;
    return std::string(v.begin(), v.begin() + n);
}

} // namespace

std::vector<Night> decode(const std::vector<Sbem::Chunk> &chunks)
{
    std::vector<Night> nights;

    // A chunk with id 14 starts a new entry. Stage triples arrive after the
    // night they belong to, and are assembled as their three ids complete.
    std::vector<StageSample> loose;
    Night current;
    bool haveCurrent = false;
    Stage pendingStage = Stage::Awake;
    int64_t pendingStart = 0;
    int pendingParts = 0;

    auto flush = [&]() {
        if (haveCurrent && current.isValid())
            nights.push_back(current);
        current = Night();
        haveCurrent = false;
        pendingParts = 0;
    };

    for (const Sbem::Chunk &chunk : chunks) {
        switch (chunk.id) {
        case kEntryTimestamp:
            flush();
            current.startMs = readMicrosAsMs(chunk.value);
            haveCurrent = true;
            break;
        case kSource:
            current.source = readString(chunk.value);
            break;
        case kTimestamp2:
            if (current.startMs == 0)
                current.startMs = readMicrosAsMs(chunk.value);
            break;
        case kDuration:        current.durationSeconds = readFloat(chunk.value); break;
        case kOnsetLatency:    current.onsetLatencySeconds = readFloat(chunk.value); break;
        case kDeep:            current.deepSeconds = readFloat(chunk.value); break;
        case kLight:           current.lightSeconds = readFloat(chunk.value); break;
        case kRem:             current.remSeconds = readFloat(chunk.value); break;
        case kWakeBeforeOffBed: current.wakeBeforeOffBedSeconds = readFloat(chunk.value); break;
        case kWakeAfterOnset:  current.wakeAfterOnsetSeconds = readFloat(chunk.value); break;
        case kHeartRateMin:    current.heartRateMinHz = readFloat(chunk.value); break;
        case kHeartRateAvg:    current.heartRateAvgHz = readFloat(chunk.value); break;
        case kMaxSpo2:         current.maxSpo2 = readFloat(chunk.value); break;
        case kAltitude:        current.altitudeMetres = readFloat(chunk.value); break;
        case kSleepId:         current.sleepId = readU32(chunk.value); break;
        case kQualityPercent:
            current.qualityPercent = readByteOrMissing(chunk.value);
            break;
        case kHrvAverage:
            current.hrvAverageMs = readByteOrMissing(chunk.value);
            break;
        case kHrvSamples:
            current.hrvSampleCount = readByteOrMissing(chunk.value);
            break;
        case kIsNap:
            if (!chunk.value.empty())
                current.isNap = chunk.value[0] != 0;
            break;

        case kStageKind:
            if (!chunk.value.empty()) {
                const uint8_t raw = chunk.value[0];
                // Anything outside the confirmed 0-3 is left as Awake
                // rather than cast blindly into the enum.
                pendingStage = raw <= 3 ? static_cast<Stage>(raw) : Stage::Awake;
                pendingParts |= 1;
            }
            break;
        case kStageStart:
            pendingStart = readMicrosAsMs(chunk.value);
            pendingParts |= 2;
            break;
        case kStageDuration:
            if ((pendingParts & 3) == 3) {
                StageSample sample;
                sample.stage = pendingStage;
                sample.startMs = pendingStart;
                sample.durationSeconds = readU32(chunk.value);
                // Stage samples arrive in their own run rather than inside
                // the night they belong to, so they are collected here and
                // matched by time once every night is known.
                loose.push_back(sample);
            }
            pendingParts = 0;
            break;

        default:
            break; // id 24 and anything the table doesn't name
        }
    }
    flush();

    // Attach each stage sample to the night whose span contains its start.
    for (const StageSample &sample : loose) {
        for (Night &night : nights) {
            const int64_t end =
                    night.startMs + static_cast<int64_t>(night.durationSeconds * 1000.0);
            if (sample.startMs >= night.startMs && sample.startMs <= end) {
                night.stages.push_back(sample);
                break;
            }
        }
    }
    for (Night &night : nights) {
        std::sort(night.stages.begin(), night.stages.end(),
                  [](const StageSample &a, const StageSample &b) {
            return a.startMs < b.startMs;
        });
    }

    std::sort(nights.begin(), nights.end(), [](const Night &a, const Night &b) {
        return a.startMs > b.startMs;
    });
    return nights;
}

} // namespace SleepTimeline
