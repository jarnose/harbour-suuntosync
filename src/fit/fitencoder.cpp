#include "fitencoder.h"

#include "fitcrc.h"

#include <cmath>
#include <cstring>

namespace Fit {

namespace {

// Wire base types: the low five bits index the decoder's own table, and
// the top bit says the value is multi-byte and follows the architecture
// byte. Emitted canonically, the way a real file has them.
enum : uint8_t {
    TypeEnum = 0x00,
    TypeSint8 = 0x01,
    TypeUint8 = 0x02,
    TypeUint16 = 0x84,
    TypeSint32 = 0x85,
    TypeUint32 = 0x86,
    TypeUint32z = 0x8C,
};

// The invalid value per type, which is how "the device did not record
// this" is spelled in a fixed-layout message.
constexpr uint8_t kInvalidUint8 = 0xFF;
constexpr uint8_t kInvalidSint8 = 0x7F;
constexpr uint16_t kInvalidUint16 = 0xFFFF;
constexpr uint32_t kInvalidUint32 = 0xFFFFFFFFu;
constexpr int32_t kInvalidSint32 = 0x7FFFFFFF;

// FIT counts seconds from 1989-12-31, and 0 means "no timestamp" rather
// than that moment.
constexpr int64_t kFitEpoch = 631065600;

uint32_t fitTime(int64_t unixSeconds)
{
    if (unixSeconds <= kFitEpoch)
        return kInvalidUint32;
    return static_cast<uint32_t>(unixSeconds - kFitEpoch);
}

int32_t semicircles(double degrees)
{
    if (std::isnan(degrees))
        return kInvalidSint32;
    // 2^31 / 180, the format's own unit for a coordinate.
    return static_cast<int32_t>(std::llround(degrees * (2147483648.0 / 180.0)));
}

void put8(std::vector<uint8_t> *out, uint8_t v) { out->push_back(v); }

void put16(std::vector<uint8_t> *out, uint16_t v)
{
    out->push_back(static_cast<uint8_t>(v & 0xFF));
    out->push_back(static_cast<uint8_t>(v >> 8));
}

void put32(std::vector<uint8_t> *out, uint32_t v)
{
    for (int shift = 0; shift < 32; shift += 8)
        out->push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
}

void putSigned32(std::vector<uint8_t> *out, int32_t v)
{
    uint32_t raw = 0;
    std::memcpy(&raw, &v, sizeof(raw));
    put32(out, raw);
}

// Scaled integers, each clamped into its own type rather than wrapping: a
// speed of 70 m/s in a uint16 at x1000 would otherwise come back as 4 m/s.
uint16_t scaledUint16(double value, double scale, double offset = 0)
{
    if (std::isnan(value))
        return kInvalidUint16;
    const double raw = (value + offset) * scale;
    if (raw < 0 || raw >= 65535.0)
        return kInvalidUint16;
    return static_cast<uint16_t>(std::llround(raw));
}

uint32_t scaledUint32(double value, double scale)
{
    if (std::isnan(value))
        return kInvalidUint32;
    const double raw = value * scale;
    if (raw < 0 || raw >= 4294967295.0)
        return kInvalidUint32;
    return static_cast<uint32_t>(std::llround(raw));
}

uint8_t smallCount(int value)
{
    if (value < 0 || value > 254)
        return kInvalidUint8;
    return static_cast<uint8_t>(value);
}

uint16_t count16(int value)
{
    if (value < 0 || value > 65534)
        return kInvalidUint16;
    return static_cast<uint16_t>(value);
}

uint8_t celsius(double value)
{
    if (std::isnan(value) || value < -127 || value > 126)
        return kInvalidSint8;
    return static_cast<uint8_t>(static_cast<int8_t>(std::llround(value)));
}

struct FieldDef
{
    uint8_t number;
    uint8_t size;
    uint8_t baseType;
};

void writeDefinition(std::vector<uint8_t> *out, uint8_t localType, uint16_t globalNumber,
                      const std::vector<FieldDef> &fields)
{
    put8(out, static_cast<uint8_t>(0x40 | (localType & 0x0F))); // definition, normal header
    put8(out, 0);                                                // reserved
    put8(out, 0);                                                // architecture: little-endian
    put16(out, globalNumber);
    put8(out, static_cast<uint8_t>(fields.size()));
    for (const FieldDef &f : fields) {
        put8(out, f.number);
        put8(out, f.size);
        put8(out, f.baseType);
    }
}

void writeDataHeader(std::vector<uint8_t> *out, uint8_t localType)
{
    put8(out, static_cast<uint8_t>(localType & 0x0F)); // data, normal header
}

// The four local message types this file uses, each with one definition.
enum : uint8_t { LocalFileId = 0, LocalRecord = 1, LocalLap = 2, LocalSession = 3,
                  LocalActivity = 4 };

const std::vector<FieldDef> &fileIdFields()
{
    static const std::vector<FieldDef> f = {
        { 0, 1, TypeEnum },     // type: 4 = activity
        { 1, 2, TypeUint16 },   // manufacturer
        { 2, 2, TypeUint16 },   // product
        { 3, 4, TypeUint32z },  // serial_number
        { 4, 4, TypeUint32 },   // time_created
    };
    return f;
}

const std::vector<FieldDef> &recordFields()
{
    static const std::vector<FieldDef> f = {
        { 253, 4, TypeUint32 }, // timestamp
        { 0, 4, TypeSint32 },   // position_lat
        { 1, 4, TypeSint32 },   // position_long
        { 2, 2, TypeUint16 },   // altitude: (m + 500) * 5
        { 5, 4, TypeUint32 },   // distance: m * 100
        { 6, 2, TypeUint16 },   // speed: m/s * 1000
        { 3, 1, TypeUint8 },    // heart_rate
        { 4, 1, TypeUint8 },    // cadence
        { 7, 2, TypeUint16 },   // power
        { 13, 1, TypeSint8 },   // temperature, degrees C
    };
    return f;
}

const std::vector<FieldDef> &lapFields()
{
    static const std::vector<FieldDef> f = {
        { 253, 4, TypeUint32 }, // timestamp: the lap's end
        { 2, 4, TypeUint32 },   // start_time
        { 7, 4, TypeUint32 },   // total_elapsed_time * 1000
        { 8, 4, TypeUint32 },   // total_timer_time * 1000
        { 9, 4, TypeUint32 },   // total_distance * 100
        { 11, 2, TypeUint16 },  // total_calories
        { 13, 2, TypeUint16 },  // avg_speed * 1000
        { 14, 2, TypeUint16 },  // max_speed * 1000
        { 15, 1, TypeUint8 },   // avg_heart_rate
        { 16, 1, TypeUint8 },   // max_heart_rate
        { 17, 1, TypeUint8 },   // avg_cadence
        { 19, 2, TypeUint16 },  // avg_power
        { 21, 2, TypeUint16 },  // total_ascent
        { 22, 2, TypeUint16 },  // total_descent
    };
    return f;
}

const std::vector<FieldDef> &sessionFields()
{
    static const std::vector<FieldDef> f = {
        { 253, 4, TypeUint32 }, // timestamp: the session's end
        { 2, 4, TypeUint32 },   // start_time
        { 5, 1, TypeEnum },     // sport
        { 6, 1, TypeEnum },     // sub_sport
        { 7, 4, TypeUint32 },   // total_elapsed_time * 1000
        { 8, 4, TypeUint32 },   // total_timer_time * 1000
        { 9, 4, TypeUint32 },   // total_distance * 100
        { 11, 2, TypeUint16 },  // total_calories
        { 14, 2, TypeUint16 },  // avg_speed * 1000
        { 15, 2, TypeUint16 },  // max_speed * 1000
        { 16, 1, TypeUint8 },   // avg_heart_rate
        { 17, 1, TypeUint8 },   // max_heart_rate
        { 18, 1, TypeUint8 },   // avg_cadence
        { 19, 1, TypeUint8 },   // max_cadence
        { 20, 2, TypeUint16 },  // avg_power
        { 21, 2, TypeUint16 },  // max_power
        // 22 and 23 here, where a lap has 21 and 22. The format's own
        // asymmetry; the decoder reads it the same way round.
        { 22, 2, TypeUint16 },  // total_ascent
        { 23, 2, TypeUint16 },  // total_descent
        { 3, 4, TypeSint32 },   // start_position_lat
        { 4, 4, TypeSint32 },   // start_position_long
        { 50, 2, TypeUint16 },  // max_altitude
        { 71, 2, TypeUint16 },  // min_altitude
        { 57, 1, TypeSint8 },   // avg_temperature
    };
    return f;
}

const std::vector<FieldDef> &activityFields()
{
    static const std::vector<FieldDef> f = {
        { 253, 4, TypeUint32 }, // timestamp
        { 0, 4, TypeUint32 },   // total_timer_time * 1000
        { 1, 2, TypeUint16 },   // num_sessions
        { 2, 1, TypeEnum },     // type: 0 = manual
    };
    return f;
}

} // namespace

std::vector<uint8_t> encodeActivity(const Activity &activity)
{
    const Summary &s = activity.summary;
    if (activity.samples.empty() && s.startTime == 0)
        return {};

    const int64_t endTime = !activity.samples.empty() ? activity.samples.back().time
                                                       : s.startTime;

    std::vector<uint8_t> body;

    // file_id first; the format requires it and a reader that does not find
    // one refuses the file.
    writeDefinition(&body, LocalFileId, 0, fileIdFields());
    writeDataHeader(&body, LocalFileId);
    put8(&body, 4); // activity
    put16(&body, s.manufacturer >= 0 ? count16(s.manufacturer) : kInvalidUint16);
    put16(&body, s.product >= 0 ? count16(s.product) : kInvalidUint16);
    put32(&body, s.serial);
    put32(&body, fitTime(s.startTime));

    writeDefinition(&body, LocalRecord, 20, recordFields());
    for (const Sample &sample : activity.samples) {
        writeDataHeader(&body, LocalRecord);
        put32(&body, fitTime(sample.time));
        putSigned32(&body, semicircles(sample.lat));
        putSigned32(&body, semicircles(sample.lon));
        put16(&body, scaledUint16(sample.altitude, 5.0, 500.0));
        put32(&body, scaledUint32(sample.distance, 100.0));
        put16(&body, scaledUint16(sample.speed, 1000.0));
        put8(&body, smallCount(sample.heartRate));
        put8(&body, smallCount(sample.cadence));
        put16(&body, count16(sample.power));
        put8(&body, celsius(sample.temperature));
    }

    if (!activity.laps.empty()) {
        writeDefinition(&body, LocalLap, 19, lapFields());
        for (const Lap &lap : activity.laps) {
            const double span = !std::isnan(lap.elapsed) ? lap.elapsed : lap.timer;
            const int64_t lapEnd = lap.startTime
                    + static_cast<int64_t>(std::isnan(span) ? 0.0 : span);
            writeDataHeader(&body, LocalLap);
            put32(&body, fitTime(lapEnd));
            put32(&body, fitTime(lap.startTime));
            put32(&body, scaledUint32(lap.elapsed, 1000.0));
            put32(&body, scaledUint32(lap.timer, 1000.0));
            put32(&body, scaledUint32(lap.distance, 100.0));
            put16(&body, count16(lap.calories));
            put16(&body, scaledUint16(lap.avgSpeed, 1000.0));
            put16(&body, scaledUint16(lap.maxSpeed, 1000.0));
            put8(&body, smallCount(lap.avgHeartRate));
            put8(&body, smallCount(lap.maxHeartRate));
            put8(&body, smallCount(lap.avgCadence));
            put16(&body, count16(lap.avgPower));
            put16(&body, scaledUint16(lap.ascent, 1.0));
            put16(&body, scaledUint16(lap.descent, 1.0));
        }
    }

    writeDefinition(&body, LocalSession, 18, sessionFields());
    writeDataHeader(&body, LocalSession);
    put32(&body, fitTime(endTime));
    put32(&body, fitTime(s.startTime));
    put8(&body, s.sport >= 0 ? smallCount(s.sport) : kInvalidUint8);
    put8(&body, s.subSport >= 0 ? smallCount(s.subSport) : kInvalidUint8);
    put32(&body, scaledUint32(s.elapsed, 1000.0));
    put32(&body, scaledUint32(s.timer, 1000.0));
    put32(&body, scaledUint32(s.distance, 100.0));
    put16(&body, count16(s.calories));
    put16(&body, scaledUint16(s.avgSpeed, 1000.0));
    put16(&body, scaledUint16(s.maxSpeed, 1000.0));
    put8(&body, smallCount(s.avgHeartRate));
    put8(&body, smallCount(s.maxHeartRate));
    put8(&body, smallCount(s.avgCadence));
    put8(&body, smallCount(s.maxCadence));
    put16(&body, count16(s.avgPower));
    put16(&body, count16(s.maxPower));
    put16(&body, scaledUint16(s.ascent, 1.0));
    put16(&body, scaledUint16(s.descent, 1.0));
    putSigned32(&body, semicircles(s.startLat));
    putSigned32(&body, semicircles(s.startLon));
    put16(&body, scaledUint16(s.maxAltitude, 5.0, 500.0));
    put16(&body, scaledUint16(s.minAltitude, 5.0, 500.0));
    put8(&body, celsius(s.avgTemperature));

    writeDefinition(&body, LocalActivity, 34, activityFields());
    writeDataHeader(&body, LocalActivity);
    put32(&body, fitTime(endTime));
    put32(&body, scaledUint32(s.timer, 1000.0));
    put16(&body, 1); // one session
    put8(&body, 0);  // manual

    // The 14-byte header, whose own CRC covers its first twelve bytes.
    std::vector<uint8_t> out;
    out.reserve(body.size() + 16);
    put8(&out, 14);
    put8(&out, 0x20);   // protocol version 2.0
    put16(&out, 2140);  // profile version, as the files here carry
    put32(&out, static_cast<uint32_t>(body.size()));
    out.push_back('.');
    out.push_back('F');
    out.push_back('I');
    out.push_back('T');
    put16(&out, Gfdi::crc16(out.data(), 12));

    out.insert(out.end(), body.begin(), body.end());

    // The file's own CRC. muktihari/fit resets its running sum after a
    // 14-byte header and checksums the data alone; this project's decoder
    // runs it over the header and the data together. **Both give the same
    // answer**, measured on five real files - appending a block's CRC
    // leaves the running value at zero, which is where the data section
    // would have started anyway. Computed over everything here, which is
    // the form the decoder checks.
    put16(&out, Gfdi::crc16(out.data(), out.size()));
    return out;
}

} // namespace Fit
