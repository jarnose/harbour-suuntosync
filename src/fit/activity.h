#pragma once

// Copied from harbour-edgesync (the same author, MIT), src/fit/ and
// src/gfdi/crc.*, where it was written to read a Garmin Edge's own files.
// FIT is FIT: nothing in here is Edge-specific, which is why the copy is
// verbatim apart from the include paths. **Both copies exist** - a fix to
// one belongs in the other, and the two apps are in the same workspace.
#include "fitdecoder.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace Fit {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// One second of the ride. A reading the device did not record is NaN (or -1 for
// the integer ones), never 0: a rider can really have 0 W, and a stretch with no
// power meter must not look like one.
struct Sample
{
    int64_t time = 0;     // Unix seconds
    double lat = kNaN;    // degrees
    double lon = kNaN;
    double altitude = kNaN;  // metres
    double distance = kNaN;  // metres since the start
    double speed = kNaN;     // m/s
    double temperature = kNaN; // degrees C
    int heartRate = -1;      // bpm
    int cadence = -1;        // rpm
    int power = -1;          // watts

    bool hasPosition() const { return !std::isnan(lat) && !std::isnan(lon); }
};

struct Lap
{
    int64_t startTime = 0;
    double elapsed = kNaN;   // seconds
    double timer = kNaN;
    double distance = kNaN;
    double avgSpeed = kNaN;
    double maxSpeed = kNaN;
    int avgHeartRate = -1;
    int maxHeartRate = -1;
    int avgPower = -1;
    int avgCadence = -1;
    double ascent = kNaN;
    double descent = kNaN;
    int calories = -1;
};

struct Summary
{
    int64_t startTime = 0;  // Unix seconds; 0 if the file had none
    int sport = -1;
    int subSport = -1;
    double elapsed = kNaN;  // seconds from start to stop
    double timer = kNaN;    // seconds the clock was running (excludes pauses)
    double distance = kNaN; // metres
    double avgSpeed = kNaN; // m/s
    double maxSpeed = kNaN;
    int avgHeartRate = -1;
    int maxHeartRate = -1;
    int avgCadence = -1;
    int maxCadence = -1;
    int avgPower = -1;
    int maxPower = -1;
    int normalizedPower = -1;
    double trainingStress = kNaN;
    double intensityFactor = kNaN;
    double ascent = kNaN;
    double descent = kNaN;
    double minAltitude = kNaN;
    double maxAltitude = kNaN;
    double avgTemperature = kNaN;
    int calories = -1;
    double startLat = kNaN;
    double startLon = kNaN;

    int manufacturer = -1;
    int product = -1;
    uint32_t serial = 0;
    std::string productName; // the device's own name for itself, if the file has one

    // True where a value was worked out from the records because the session
    // message did not carry it.
    bool derived = false;
};

struct Activity
{
    Summary summary;
    std::vector<Sample> samples;
    std::vector<Lap> laps;
    DecodeStats stats;
    int sessions = 0; // a multisport file has several; the summary is the first's, extended by the rest
};

// Reads a FIT activity file. False, with `error`, if it is not one or holds no
// records and no session.
bool parseActivity(const std::vector<uint8_t> &fileBytes, Activity &out, std::string &error);

// Reads only the summary: the same pass, but the records are not kept. For
// indexing a file that was just downloaded.
bool parseSummary(const std::vector<uint8_t> &fileBytes, Summary &out, std::string &error);

// A stable, translation-free name for a sport: "cycling", "mountain_biking",
// "indoor_cycling", "running", ... "other". The UI translates it.
std::string sportKey(int sport, int subSport);

// Down-samples to roughly `target` points by keeping every n-th one (and always
// the last), for drawing.
std::vector<Sample> thin(const std::vector<Sample> &samples, size_t target);

} // namespace Fit
