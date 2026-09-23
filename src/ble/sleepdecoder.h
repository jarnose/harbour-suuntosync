#pragma once

#include "sbemcontainer.h"

#include <cstdint>
#include <string>
#include <vector>

// Decodes the sleep timeline file the watch renders on request - the
// SBEM0102 container that /Daily/Sleep/Timeline/Data + /Dev/FileSystem/
// Stream produce (see docs/watch-push-resources.md).
//
// The field map was not guessed. A real 6 kB file was fetched from the
// watch and every field cross-checked against what the cloud reports for
// the *same night*, which this project already syncs (see
// src/health/healthstore.h): the four sleep-stage totals match to the
// second, and so do heart rate, SpO2, HRV, altitude and the id.
//
// Chunk ids, confirmed:
//
//   14  uint64   entry timestamp, MICROseconds since the epoch
//   13  string   "suunto-247-sleep-<serial>"
//    2  uint64   same timestamp again
//    3  float    total duration, seconds
//    4  float    sleep onset latency, seconds
//    5  uint8    quality, percent (cloud reports it as 0..1)
//    6  float    deep sleep, seconds
//    7  float    minimum heart rate, HERTZ
//    8  float    average heart rate, HERTZ
//    9  float    light sleep, seconds
//   15  float    maximum SpO2, 0..1
//   16  float    altitude, metres
//   17  uint8    average HRV, ms
//   18  uint8    HRV sample count
//   19  uint32   sleep id - unix SECONDS, same convention as a logbook id
//   20  uint8    is-nap flag
//   21  float    REM sleep, seconds
//   22  float    wake before off-bed, seconds
//   23  float    wake after sleep onset, seconds
//
// And a per-night stage series, as repeating triples:
//
//   25  uint8    stage: 0 awake, 1 REM, 2 light, 3 deep
//   26  uint64   start, microseconds
//   27  uint32   duration, seconds
//
// Id 24 (float) appears but was zero in every captured night, so it is
// read and ignored rather than named on a guess.
//
// Qt-free, same discipline as the rest of src/ble.
namespace SleepTimeline {

enum class Stage { Awake = 0, Rem = 1, Light = 2, Deep = 3 };

struct StageSample
{
    Stage stage = Stage::Awake;
    int64_t startMs = 0;      // milliseconds, converted from the file's microseconds
    uint32_t durationSeconds = 0;
};

struct Night
{
    int64_t startMs = 0;      // milliseconds since the epoch, UTC
    uint32_t sleepId = 0;     // unix seconds
    std::string source;       // "suunto-247-sleep-<serial>"

    double durationSeconds = 0;
    double deepSeconds = 0;
    double lightSeconds = 0;
    double remSeconds = 0;
    double onsetLatencySeconds = 0;
    double wakeAfterOnsetSeconds = 0;
    double wakeBeforeOffBedSeconds = 0;

    double heartRateAvgHz = 0;   // hertz, as the watch stores it
    double heartRateMinHz = 0;
    double maxSpo2 = 0;          // 0..1
    double altitudeMetres = 0;
    // -1 means the watch recorded no value. The file uses 0xFF as the
    // "no reading" sentinel for these byte fields - three of twelve
    // captured nights have quality = 255 - and passing that through as a
    // number is what made the cloud reject an upload with "'quality' with
    // value 2.55 is outside of range 0.0...1.0".
    int hrvAverageMs = -1;
    int hrvSampleCount = -1;
    int qualityPercent = -1;
    bool isNap = false;

    std::vector<StageSample> stages;

    bool isValid() const { return startMs != 0 && durationSeconds > 0; }
};

// Newest first. Entries without a usable timestamp are skipped rather than
// placed at the epoch.
std::vector<Night> decode(const std::vector<Sbem::Chunk> &chunks);

} // namespace SleepTimeline
