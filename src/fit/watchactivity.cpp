#include "watchactivity.h"

#include "../ble/sbemcontainer.h"
#include "../ble/smldecoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace Fit {

namespace {

// The watch's canonical units are not the file's: heart rate and cadence
// arrive in hertz and temperature in kelvin, the same conversions
// toDisplayUnits() makes for the screen. Position arrives in degrees -
// it is the *cloud* format that wants radians, which cost this project
// five rounds once and is worth not confusing with this direction.
void applyField(const char *name, double value, Sample *sample)
{
    if (std::strcmp(name, "Sample.Latitude") == 0)
        sample->lat = value;
    else if (std::strcmp(name, "Sample.Longitude") == 0)
        sample->lon = value;
    else if (std::strcmp(name, "Sample.Altitude") == 0)
        sample->altitude = value;
    else if (std::strcmp(name, "Sample.Distance") == 0)
        sample->distance = value;
    else if (std::strcmp(name, "Sample.Speed") == 0)
        sample->speed = value;
    else if (std::strcmp(name, "Sample.Temperature") == 0)
        sample->temperature = value - 273.15;
    else if (std::strcmp(name, "Sample.HR") == 0)
        sample->heartRate = static_cast<int>(std::lround(value * 60.0));
    else if (std::strcmp(name, "Sample.Cadence") == 0)
        sample->cadence = static_cast<int>(std::lround(value * 60.0));
    else if (std::strcmp(name, "Sample.Power") == 0)
        sample->power = static_cast<int>(std::lround(value));
}

bool worthKeeping(const Sample &s)
{
    return s.hasPosition() || !std::isnan(s.altitude) || !std::isnan(s.distance)
            || !std::isnan(s.speed) || s.heartRate > 0 || s.cadence >= 0 || s.power >= 0;
}

} // namespace

Activity activityFromWatchData(const std::vector<uint8_t> &compressedData,
                                const SbemDescriptors::Table &table)
{
    Activity out;

    // One sample per chunk, which is how the payload is laid out and how
    // the cloud's own JSON is built from it - a GPS fix and the sensor
    // readings of the same second are separate chunks, so they become
    // separate records. A FIT reader is content with that: each carries
    // its own timestamp.
    //
    // **Do not merge them by timestamp.** It is tempting - it would give
    // the one-record-per-second shape a FIT file usually has, and a
    // quarter the records - and it loses readings. Measured on the two
    // real payloads here: 827 and 219 cases where the same field appears
    // twice inside one second *with different values*, because the watch
    // samples some fields faster than it stamps them. Merging would throw
    // those away silently.
    std::map<size_t, Sample> byChunk;
    Sml::decode(Sbem::parseContainer(Sbem::heatshrinkDecompress(compressedData)), table,
                [&byChunk](const Sml::Reading &reading) {
        if (!reading.descriptor || reading.isText)
            return;
        const char *name = reading.descriptor->name;
        if (!name || name[0] == '\0')
            return;
        Sample &sample = byChunk[reading.chunkIndex];
        if (reading.timeMs != 0)
            sample.time = reading.timeMs / 1000;
        applyField(name, reading.value, &sample);
    });

    out.samples.reserve(byChunk.size());
    for (const std::pair<const size_t, Sample> &entry : byChunk) {
        if (entry.second.time == 0 || !worthKeeping(entry.second))
            continue;
        out.samples.push_back(entry.second);
    }
    std::sort(out.samples.begin(), out.samples.end(),
              [](const Sample &a, const Sample &b) { return a.time < b.time; });

    if (!out.samples.empty()) {
        out.summary.startTime = out.samples.front().time;
        double lowest = Fit::kNaN;
        double highest = Fit::kNaN;
        for (const Sample &sample : out.samples) {
            if (std::isnan(sample.altitude))
                continue;
            if (std::isnan(lowest) || sample.altitude < lowest)
                lowest = sample.altitude;
            if (std::isnan(highest) || sample.altitude > highest)
                highest = sample.altitude;
        }
        out.summary.minAltitude = lowest;
        out.summary.maxAltitude = highest;
    }
    return out;
}

} // namespace Fit
