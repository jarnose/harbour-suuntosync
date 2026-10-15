#include "activity.h"

#include <algorithm>

namespace Fit {

namespace {

enum : uint16_t { MsgFileId = 0, MsgSession = 18, MsgLap = 19, MsgRecord = 20 };

int intField(const Message &m, uint8_t number)
{
    int64_t v;
    return m.getInt(number, v) ? static_cast<int>(v) : -1;
}

double scaled(const Message &m, uint8_t number, double scale, double offset = 0)
{
    double v;
    return m.getScaled(number, scale, offset, v) ? v : kNaN;
}

// The "enhanced" 32-bit field when the file has it, the 16-bit one otherwise.
double pick(const Message &m, uint8_t enhanced, uint8_t plain, double scale, double offset = 0)
{
    const double e = scaled(m, enhanced, scale, offset);
    return std::isnan(e) ? scaled(m, plain, scale, offset) : e;
}

void readRecord(const Message &m, Sample &s)
{
    if (m.hasTimestamp)
        s.time = toUnix(m.timestamp);
    int64_t lat, lon;
    if (m.getInt(0, lat) && m.getInt(1, lon)) {
        s.lat = semicirclesToDegrees(lat);
        s.lon = semicirclesToDegrees(lon);
    }
    s.altitude = pick(m, 78, 2, 5, 500);
    s.distance = scaled(m, 5, 100);
    s.speed = pick(m, 73, 6, 1000);
    s.temperature = scaled(m, 13, 1);
    s.heartRate = intField(m, 3);
    s.cadence = intField(m, 4);
    s.power = intField(m, 7);
}

void readLap(const Message &m, Lap &l)
{
    uint32_t t;
    if (m.getUint(2, t))
        l.startTime = toUnix(t);
    else if (m.hasTimestamp)
        l.startTime = toUnix(m.timestamp);
    l.elapsed = scaled(m, 7, 1000);
    l.timer = scaled(m, 8, 1000);
    l.distance = scaled(m, 9, 100);
    l.avgSpeed = pick(m, 110, 13, 1000);
    l.maxSpeed = pick(m, 111, 14, 1000);
    l.avgHeartRate = intField(m, 15);
    l.maxHeartRate = intField(m, 16);
    l.avgCadence = intField(m, 17);
    l.avgPower = intField(m, 19);
    l.ascent = scaled(m, 21, 1);
    l.descent = scaled(m, 22, 1);
    l.calories = intField(m, 11);
}

void addTo(double &total, double more)
{
    if (std::isnan(more))
        return;
    total = std::isnan(total) ? more : total + more;
}

void readSession(const Message &m, Summary &s, int &sessions)
{
    ++sessions;
    const bool first = sessions == 1;

    if (first) {
        uint32_t t;
        if (m.getUint(2, t))
            s.startTime = toUnix(t);
        s.sport = intField(m, 5);
        s.subSport = intField(m, 6);
        s.avgSpeed = pick(m, 124, 14, 1000);
        s.maxSpeed = pick(m, 125, 15, 1000);
        s.avgHeartRate = intField(m, 16);
        s.maxHeartRate = intField(m, 17);
        s.avgCadence = intField(m, 18);
        s.maxCadence = intField(m, 19);
        s.avgPower = intField(m, 20);
        s.maxPower = intField(m, 21);
        s.normalizedPower = intField(m, 34);
        s.trainingStress = scaled(m, 35, 10);
        s.intensityFactor = scaled(m, 36, 1000);
        s.minAltitude = pick(m, 127, 71, 5, 500);
        s.maxAltitude = pick(m, 128, 50, 5, 500);
        s.avgTemperature = scaled(m, 57, 1);
        int64_t lat, lon;
        if (m.getInt(3, lat) && m.getInt(4, lon)) {
            s.startLat = semicirclesToDegrees(lat);
            s.startLon = semicirclesToDegrees(lon);
        }
        s.elapsed = scaled(m, 7, 1000);
        s.timer = scaled(m, 8, 1000);
        s.distance = scaled(m, 9, 100);
        s.ascent = scaled(m, 22, 1);
        s.descent = scaled(m, 23, 1);
        s.calories = intField(m, 11);
        return;
    }

    // Further sessions (a triathlon, a ride with a stop recorded apart) add to
    // the totals and leave the rest, which does not add, with the first.
    addTo(s.elapsed, scaled(m, 7, 1000));
    addTo(s.timer, scaled(m, 8, 1000));
    addTo(s.distance, scaled(m, 9, 100));
    addTo(s.ascent, scaled(m, 22, 1));
    addTo(s.descent, scaled(m, 23, 1));
    const int cal = intField(m, 11);
    if (cal >= 0)
        s.calories = (s.calories < 0 ? 0 : s.calories) + cal;
    s.maxSpeed = std::isnan(s.maxSpeed) ? pick(m, 125, 15, 1000) : std::max(s.maxSpeed, pick(m, 125, 15, 1000));
}

// Fills what the session message left out from the records.
void deriveMissing(Summary &s, const std::vector<Sample> &samples)
{
    if (samples.empty())
        return;
    if (s.startTime == 0 && samples.front().time != 0) {
        s.startTime = samples.front().time;
        s.derived = true;
    }
    if (std::isnan(s.elapsed) && samples.back().time > samples.front().time) {
        s.elapsed = static_cast<double>(samples.back().time - samples.front().time);
        s.derived = true;
    }
    if (std::isnan(s.timer) && !std::isnan(s.elapsed)) {
        s.timer = s.elapsed;
        s.derived = true;
    }
    if (std::isnan(s.distance)) {
        for (size_t i = samples.size(); i-- > 0;) {
            if (!std::isnan(samples[i].distance)) {
                s.distance = samples[i].distance;
                s.derived = true;
                break;
            }
        }
    }
    if (std::isnan(s.avgSpeed) && !std::isnan(s.distance) && !std::isnan(s.timer) && s.timer > 0) {
        s.avgSpeed = s.distance / s.timer;
        s.derived = true;
    }

    long hrSum = 0, hrN = 0, pwSum = 0, pwN = 0, cadSum = 0, cadN = 0;
    int hrMax = -1, pwMax = -1, cadMax = -1;
    double altMin = kNaN, altMax = kNaN, spMax = kNaN;
    for (const Sample &x : samples) {
        if (x.heartRate > 0) {
            hrSum += x.heartRate;
            ++hrN;
            hrMax = std::max(hrMax, x.heartRate);
        }
        if (x.power >= 0) {
            pwSum += x.power;
            ++pwN;
            pwMax = std::max(pwMax, x.power);
        }
        if (x.cadence >= 0) {
            cadSum += x.cadence;
            ++cadN;
            cadMax = std::max(cadMax, x.cadence);
        }
        if (!std::isnan(x.altitude)) {
            altMin = std::isnan(altMin) ? x.altitude : std::min(altMin, x.altitude);
            altMax = std::isnan(altMax) ? x.altitude : std::max(altMax, x.altitude);
        }
        if (!std::isnan(x.speed))
            spMax = std::isnan(spMax) ? x.speed : std::max(spMax, x.speed);
    }
    if (s.avgHeartRate < 0 && hrN) {
        s.avgHeartRate = static_cast<int>(hrSum / hrN);
        s.derived = true;
    }
    if (s.maxHeartRate < 0 && hrMax >= 0) {
        s.maxHeartRate = hrMax;
        s.derived = true;
    }
    if (s.avgPower < 0 && pwN) {
        s.avgPower = static_cast<int>(pwSum / pwN);
        s.derived = true;
    }
    if (s.maxPower < 0 && pwMax >= 0) {
        s.maxPower = pwMax;
        s.derived = true;
    }
    if (s.avgCadence < 0 && cadN) {
        s.avgCadence = static_cast<int>(cadSum / cadN);
        s.derived = true;
    }
    if (s.maxCadence < 0 && cadMax >= 0) {
        s.maxCadence = cadMax;
        s.derived = true;
    }
    if (std::isnan(s.minAltitude) && !std::isnan(altMin)) {
        s.minAltitude = altMin;
        s.derived = true;
    }
    if (std::isnan(s.maxAltitude) && !std::isnan(altMax)) {
        s.maxAltitude = altMax;
        s.derived = true;
    }
    if (std::isnan(s.maxSpeed) && !std::isnan(spMax)) {
        s.maxSpeed = spMax;
        s.derived = true;
    }
    if (std::isnan(s.startLat)) {
        for (const Sample &x : samples) {
            if (x.hasPosition()) {
                s.startLat = x.lat;
                s.startLon = x.lon;
                break;
            }
        }
    }
}

bool run(const std::vector<uint8_t> &bytes, Activity &out, bool keepRecords, std::string &error)
{
    out = Activity();
    bool sawSession = false;

    const auto sink = [&](const Message &m) {
        switch (m.global) {
        case MsgFileId: {
            int64_t v;
            if (m.getInt(1, v))
                out.summary.manufacturer = static_cast<int>(v);
            if (m.getInt(2, v))
                out.summary.product = static_cast<int>(v);
            uint32_t serial;
            if (m.getUint(3, serial))
                out.summary.serial = serial;
            std::string name;
            if (m.getString(8, name))
                out.summary.productName = name;
            break;
        }
        case MsgSession:
            readSession(m, out.summary, out.sessions);
            sawSession = true;
            break;
        case MsgLap:
            if (keepRecords) {
                Lap l;
                readLap(m, l);
                out.laps.push_back(l);
            }
            break;
        case MsgRecord: {
            Sample s;
            readRecord(m, s);
            if (keepRecords)
                out.samples.push_back(s);
            else if (out.summary.startTime == 0 && s.time != 0) {
                // Without a session message the first record dates the ride.
                out.summary.startTime = s.time;
            }
            break;
        }
        default:
            break;
        }
    };

    if (!decode(bytes, sink, out.stats, error))
        return false;
    if (!sawSession && out.samples.empty() && keepRecords) {
        error = "the file holds neither a session nor any records";
        return false;
    }
    if (keepRecords)
        deriveMissing(out.summary, out.samples);
    return true;
}

} // namespace

bool parseActivity(const std::vector<uint8_t> &fileBytes, Activity &out, std::string &error)
{
    return run(fileBytes, out, true, error);
}

bool parseSummary(const std::vector<uint8_t> &fileBytes, Summary &out, std::string &error)
{
    // The summary of a file without a session message needs its records, so it
    // takes the whole pass; the saving is only that the result is small.
    Activity a;
    if (!run(fileBytes, a, true, error))
        return false;
    out = a.summary;
    return true;
}

std::string sportKey(int sport, int subSport)
{
    switch (sport) {
    case 1:
        return subSport == 1 ? "treadmill_running" : subSport == 3 ? "trail_running" : "running";
    case 2:
        switch (subSport) {
        case 5: // spin
        case 6:
            return "indoor_cycling";
        case 8:
        case 9:
            return "mountain_biking";
        case 11:
            return "cyclocross";
        case 12:
            return "hand_cycling";
        case 13:
            return "track_cycling";
        case 28:
        case 47:
            return "e_biking";
        case 46:
            return "gravel_cycling";
        default:
            return "cycling";
        }
    case 5:
        return "swimming";
    case 10:
        return "training";
    case 11:
        return "walking";
    case 12:
        return "cross_country_skiing";
    case 13:
        return "alpine_skiing";
    case 17:
        return "hiking";
    case 21:
        return "e_biking";
    case 4:
        return "fitness_equipment";
    case 15:
        return "rowing";
    default:
        return "other";
    }
}

std::vector<Sample> thin(const std::vector<Sample> &samples, size_t target)
{
    if (target < 2 || samples.size() <= target)
        return samples;
    std::vector<Sample> out;
    out.reserve(target + 1);
    const double step = static_cast<double>(samples.size() - 1) / static_cast<double>(target - 1);
    size_t last = samples.size();
    for (size_t i = 0; i < target; ++i) {
        const size_t idx = static_cast<size_t>(static_cast<double>(i) * step + 0.5);
        if (idx != last && idx < samples.size()) {
            out.push_back(samples[idx]);
            last = idx;
        }
    }
    if (out.empty() || out.back().time != samples.back().time)
        out.push_back(samples.back());
    return out;
}

} // namespace Fit
