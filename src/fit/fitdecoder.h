#pragma once

// Copied from harbour-edgesync (the same author, MIT), src/fit/ and
// src/gfdi/crc.*, where it was written to read a Garmin Edge's own files.
// FIT is FIT: nothing in here is Edge-specific, which is why the copy is
// verbatim apart from the include paths. **Both copies exist** - a fix to
// one belongs in the other, and the two apps are in the same workspace.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// A reader for Garmin's FIT file format: the container only. It knows nothing of
// what the messages mean; fit/activity.* turns them into a workout.
//
// FIT is a stream of records. A *definition* record declares a layout (which
// fields, how wide, in which byte order) for a local message type 0-15, and the
// *data* records that follow use it until it is redefined. That is why a
// message can only be read in the order it was written, and why this decoder
// hands each one to a callback instead of building a list: a five-hour ride at
// one record a second is ~18 000 of them.
namespace Fit {

enum BaseType : uint8_t {
    Enum = 0, Sint8, Uint8, Sint16, Uint16, Sint32, Uint32, String, Float32, Float64,
    Uint8z, Uint16z, Uint32z, Byte, Sint64, Uint64, Uint64z,
};

struct Field
{
    uint8_t number = 0;
    uint8_t size = 0;      // bytes on the wire
    uint8_t baseType = 0;  // BaseType
    const uint8_t *data = nullptr;
    bool bigEndian = false;
};

// One data message. Its fields point into the buffer being decoded and are
// valid only during the callback.
struct Message
{
    uint16_t global = 0;       // the global message number: 20 is a record, 18 a session
    uint32_t timestamp = 0;    // FIT seconds (since 1989-12-31) of the latest timestamp seen, 0 before any
    bool hasTimestamp = false; // this message carried (or inherited, by compressed header) a timestamp
    std::vector<Field> fields;

    const Field *find(uint8_t number) const;

    // The field as a number, if it is there and not the type's "invalid" value.
    // For an array field this is the first element.
    bool getInt(uint8_t number, int64_t &out) const;
    bool getUint(uint8_t number, uint32_t &out) const;
    bool getDouble(uint8_t number, double &out) const;
    // scaled = raw / scale - offset, the profile's usual form.
    bool getScaled(uint8_t number, double scale, double offset, double &out) const;
    bool getString(uint8_t number, std::string &out) const;
};

struct DecodeStats
{
    size_t messages = 0;
    size_t files = 0;       // chained files seen
    bool crcOk = true;      // every file's trailing CRC matched
    bool truncated = false; // the data ended before the header said it would
};

// Decodes every message of every file in the buffer. Returns false, with `error`
// set, only if nothing usable could be read; a damaged tail after good records
// still returns true with `stats.truncated` or `crcOk` telling what happened.
bool decode(const uint8_t *data, size_t size, const std::function<void(const Message &)> &sink,
            DecodeStats &stats, std::string &error);

inline bool decode(const std::vector<uint8_t> &data, const std::function<void(const Message &)> &sink,
                   DecodeStats &stats, std::string &error)
{
    return decode(data.data(), data.size(), sink, stats, error);
}

// FIT seconds to Unix seconds.
constexpr int64_t kEpochOffset = 631065600;
inline int64_t toUnix(uint32_t fitSeconds) { return static_cast<int64_t>(fitSeconds) + kEpochOffset; }

// Semicircles to degrees.
inline double semicirclesToDegrees(int64_t s) { return static_cast<double>(s) * (180.0 / 2147483648.0); }

} // namespace Fit
