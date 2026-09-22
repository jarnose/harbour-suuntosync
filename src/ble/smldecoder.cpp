#include "smldecoder.h"

#include <cstring>
#include <unordered_map>

namespace Sml {

namespace {

using SbemDescriptors::Descriptor;
using SbemDescriptors::Format;

// Descriptor 33 is TimeISO8601, the payload's time base; every group's
// leading int16 is a delta against it, so its running value is the clock.
constexpr uint16_t kTimeDescriptor = 33;

// Reads one fixed-size field as a raw number, before any scaling. Returns
// false if the format isn't a fixed-size numeric one.
bool readRaw(const uint8_t *data, Format format, double *out)
{
    switch (format) {
    case Format::Bool:
    case Format::Enum:
    case Format::UInt8:
        *out = *data;
        return true;
    case Format::Int8:
    case Format::DeltaInt8:
        *out = static_cast<int8_t>(*data);
        return true;
    case Format::DeltaUInt8:
        *out = *data;
        return true;
    case Format::UInt16: {
        uint16_t v = 0;
        std::memcpy(&v, data, sizeof(v));
        *out = v;
        return true;
    }
    case Format::Int16:
    case Format::DeltaInt16: {
        int16_t v = 0;
        std::memcpy(&v, data, sizeof(v));
        *out = v;
        return true;
    }
    case Format::UInt32: {
        uint32_t v = 0;
        std::memcpy(&v, data, sizeof(v));
        *out = v;
        return true;
    }
    case Format::Int32: {
        int32_t v = 0;
        std::memcpy(&v, data, sizeof(v));
        *out = v;
        return true;
    }
    case Format::Float32: {
        float v = 0;
        std::memcpy(&v, data, sizeof(v));
        *out = v;
        return true;
    }
    case Format::Float64: {
        double v = 0;
        std::memcpy(&v, data, sizeof(v));
        *out = v;
        return true;
    }
    case Format::Local64: {
        uint64_t v = 0;
        std::memcpy(&v, data, sizeof(v));
        *out = static_cast<double>(Sbem::decodeLocal64(v));
        return true;
    }
    default:
        return false;
    }
}

// How many bytes a field occupies. utf8 is NUL-terminated, so it has to be
// measured against the data; everything else is fixed.
size_t fieldSize(const Descriptor &d, const uint8_t *data, size_t available)
{
    if (d.format == Format::Utf8) {
        size_t n = 0;
        while (n < available && data[n] != '\0')
            ++n;
        return n < available ? n + 1 : available;
    }
    return d.size > 0 ? static_cast<size_t>(d.size) : 0;
}

} // namespace

void decode(const std::vector<Sbem::Chunk> &chunks, const ReadingCallback &callback)
{
    // Running value per descriptor, in raw (pre-scaling) units - what a
    // differential field accumulates against.
    std::unordered_map<uint16_t, double> running;

    for (size_t chunkIndex = 0; chunkIndex < chunks.size(); ++chunkIndex) {
        const Sbem::Chunk &chunk = chunks[chunkIndex];
        const Descriptor *group = SbemDescriptors::find(chunk.id);
        if (!group || group->childCount == 0)
            continue;

        size_t offset = 0;
        for (uint16_t i = 0; i < group->childCount; ++i) {
            const Descriptor *field = SbemDescriptors::find(group->children[i]);
            if (!field)
                break; // the rest of this chunk's layout is unknowable

            const size_t remaining = chunk.value.size() - offset;
            if (offset > chunk.value.size())
                break;
            const size_t size = fieldSize(*field, chunk.value.data() + offset, remaining);
            if (size > remaining)
                break;
            if (size == 0) // Unchanged, or an empty string
                continue;

            double raw = 0;
            if (!readRaw(chunk.value.data() + offset, field->format, &raw)) {
                // utf8 is the only non-numeric format in the table. Report
                // it as text rather than dropping it: the JSON the cloud
                // wants carries these verbatim.
                if (field->format == Format::Utf8) {
                    Reading reading;
                    reading.descriptorId = field->id;
                    reading.descriptor = field;
                    reading.chunkIndex = chunkIndex;
                    reading.isText = true;
                    const char *begin = reinterpret_cast<const char *>(
                            chunk.value.data() + offset);
                    // size includes the NUL when one was found.
                    size_t textLen = size;
                    while (textLen > 0 && begin[textLen - 1] == '\0')
                        --textLen;
                    reading.text.assign(begin, textLen);
                    const auto textClock = running.find(kTimeDescriptor);
                    reading.timeMs = textClock == running.end()
                            ? 0 : static_cast<int64_t>(textClock->second);
                    callback(reading);
                }
                offset += size;
                continue;
            }
            offset += size;

            // A differential reading belongs to whatever it's a delta of.
            const Descriptor *target = field;
            if (field->deltaOf != 0) {
                const Descriptor *base = SbemDescriptors::find(field->deltaOf);
                if (!base)
                    continue;
                auto it = running.find(base->id);
                if (it == running.end())
                    continue; // no absolute reading to accumulate against yet
                it->second += raw;
                raw = it->second;
                target = base;
            } else {
                // A "no reading" sentinel must not become the base that
                // later deltas accumulate from: doing so drifts the whole
                // chain by the sentinel's value (speed's 65535 turned a
                // 7 m/s ride into 1317 m/s before this check existed).
                if (field->hasNil && raw == field->nil)
                    continue;
                running[field->id] = raw;
            }

            if (target->hasNil && raw == target->nil)
                continue;

            Reading reading;
            reading.descriptorId = target->id;
            reading.descriptor = target;
            reading.chunkIndex = chunkIndex;
            reading.value = raw * target->scale + target->offset;
            const auto clock = running.find(kTimeDescriptor);
            reading.timeMs = clock == running.end() ? 0 : static_cast<int64_t>(clock->second);
            callback(reading);
        }
    }
}

} // namespace Sml
