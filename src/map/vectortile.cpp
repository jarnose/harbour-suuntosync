#include "vectortile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace Mvt {
namespace {

// A bounds-checked cursor over the tile. Every read goes through this, so a
// truncated tile - which a cancelled download produces - fails by setting
// `bad` rather than by reading past the buffer.
struct Cursor
{
    const uint8_t *data = nullptr;
    size_t pos = 0;
    size_t end = 0;
    bool bad = false;

    bool atEnd() const { return bad || pos >= end; }

    uint8_t byte()
    {
        if (pos >= end) {
            bad = true;
            return 0;
        }
        return data[pos++];
    }

    // Protobuf base-128 varint. Capped at ten bytes: that is the most a
    // 64-bit value can take, and without the cap a run of 0x80 bytes would
    // spin until it hit the end of the tile.
    uint64_t varint()
    {
        uint64_t result = 0;
        for (int shift = 0; shift < 70; shift += 7) {
            const uint8_t b = byte();
            if (bad)
                return 0;
            result |= uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80))
                return result;
        }
        bad = true;
        return 0;
    }

    void skip(size_t count)
    {
        if (count > end - pos) {
            bad = true;
            pos = end;
            return;
        }
        pos += count;
    }
};

// Zigzag: the format stores signed deltas with the sign in the low bit, so
// small negative numbers stay one byte.
inline int64_t zigzag(uint64_t value)
{
    return int64_t(value >> 1) ^ -int64_t(value & 1);
}

struct Tag
{
    uint32_t field = 0;
    uint8_t wire = 0;
};

bool readTag(Cursor &c, Tag *tag)
{
    if (c.atEnd())
        return false;
    const uint64_t key = c.varint();
    if (c.bad)
        return false;
    tag->field = uint32_t(key >> 3);
    tag->wire = uint8_t(key & 7);
    return true;
}

// Steps over one field's payload, for the fields this decoder does not read.
// Wire types 3 and 4 are the deprecated start/end-group pair, which no
// vector tile uses; treating them as malformed is better than pretending to
// handle them.
bool skipField(Cursor &c, const Tag &tag)
{
    switch (tag.wire) {
    case 0: c.varint(); break;
    case 1: c.skip(8); break;
    case 2: c.skip(size_t(c.varint())); break;
    case 5: c.skip(4); break;
    default: c.bad = true; break;
    }
    return !c.bad;
}

std::string readString(Cursor &c)
{
    const size_t length = size_t(c.varint());
    if (c.bad || length > c.end - c.pos) {
        c.bad = true;
        return std::string();
    }
    std::string out(reinterpret_cast<const char *>(c.data + c.pos), length);
    c.pos += length;
    return out;
}

// A number rendered the way a reader expects rather than the way printf
// defaults: no trailing zeroes on a whole number, since these become display
// strings and "2" reads better than "2.000000".
std::string numberToString(double value)
{
    if (value == std::floor(value) && std::fabs(value) < 1e15) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
        return buffer;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%g", value);
    return buffer;
}

// One Value message. Exactly one of the seven fields is set in practice, and
// the last one wins if a tile sets several, which matches how a protobuf
// reader is expected to behave.
std::string readValue(Cursor &c)
{
    const size_t length = size_t(c.varint());
    if (c.bad || length > c.end - c.pos) {
        c.bad = true;
        return std::string();
    }
    Cursor sub{ c.data, c.pos, c.pos + length, false };
    c.pos += length;

    std::string out;
    Tag tag;
    while (readTag(sub, &tag)) {
        switch (tag.field) {
        case 1: // string
            if (tag.wire != 2) {
                skipField(sub, tag);
                break;
            }
            out = readString(sub);
            break;
        case 2: { // float
            if (tag.wire != 5) {
                skipField(sub, tag);
                break;
            }
            if (sub.end - sub.pos < 4) {
                sub.bad = true;
                break;
            }
            float f = 0;
            std::memcpy(&f, sub.data + sub.pos, sizeof(f));
            sub.pos += 4;
            out = numberToString(double(f));
            break;
        }
        case 3: { // double
            if (tag.wire != 1) {
                skipField(sub, tag);
                break;
            }
            if (sub.end - sub.pos < 8) {
                sub.bad = true;
                break;
            }
            double d = 0;
            std::memcpy(&d, sub.data + sub.pos, sizeof(d));
            sub.pos += 8;
            out = numberToString(d);
            break;
        }
        case 4: // int64
        case 5: // uint64
            if (tag.wire != 0) {
                skipField(sub, tag);
                break;
            }
            out = numberToString(double(sub.varint()));
            break;
        case 6: // sint64
            if (tag.wire != 0) {
                skipField(sub, tag);
                break;
            }
            out = numberToString(double(zigzag(sub.varint())));
            break;
        case 7: // bool
            if (tag.wire != 0) {
                skipField(sub, tag);
                break;
            }
            out = sub.varint() ? "true" : "false";
            break;
        default:
            skipField(sub, tag);
            break;
        }
        if (sub.bad)
            break;
    }
    if (sub.bad)
        c.bad = true;
    return out;
}

// The geometry stream: a sequence of (command, parameters) where the command
// integer packs an id in its low three bits and a repeat count in the rest.
// Parameters are zigzag deltas from a running cursor, which starts at the
// tile's origin and is NOT reset between parts.
void decodeGeometry(Cursor &c, size_t length, GeomType type, Feature *feature)
{
    Cursor geom{ c.data, c.pos, c.pos + length, false };
    c.skip(length);

    double x = 0;
    double y = 0;
    std::vector<Point> part;

    auto flush = [&feature, &part]() {
        if (!part.empty())
            feature->parts.push_back(part);
        part.clear();
    };

    while (!geom.atEnd()) {
        const uint64_t header = geom.varint();
        if (geom.bad)
            break;
        const uint32_t command = uint32_t(header & 0x7);
        uint32_t count = uint32_t(header >> 3);

        if (command == 1) { // MoveTo
            // A point feature writes all of its points as one MoveTo; a line
            // or a ring writes one MoveTo per part. Keeping them in one part
            // for the former means parts.size() counts geometries either way.
            if (type != GeomType::Point)
                flush();
            for (uint32_t i = 0; i < count && !geom.bad; ++i) {
                x += double(zigzag(geom.varint()));
                y += double(zigzag(geom.varint()));
                part.push_back({ x, y });
            }
        } else if (command == 2) { // LineTo
            for (uint32_t i = 0; i < count && !geom.bad; ++i) {
                x += double(zigzag(geom.varint()));
                y += double(zigzag(geom.varint()));
                part.push_back({ x, y });
            }
        } else if (command == 7) { // ClosePath
            // Takes no parameters. Closing explicitly, so a caller drawing a
            // ring does not have to know the first point is implied.
            if (!part.empty())
                part.push_back(part.front());
        } else {
            // An unknown command would desynchronise the stream, since its
            // parameter count is unknowable - stop rather than emit nonsense.
            break;
        }
        if (geom.bad)
            break;
    }
    flush();
}

bool wanted(const std::vector<std::string> &list, const std::string &name)
{
    return std::find(list.begin(), list.end(), name) != list.end();
}

// One Layer message. Decoded in two passes because the format does not
// promise an order: a feature's tags index into `keys` and `values`, and
// those may be written after the features that refer to them.
Layer readLayer(Cursor &c, size_t length, const std::vector<std::string> &wantedKeys, bool *keep,
                 const std::vector<std::string> &wantedLayers)
{
    const size_t start = c.pos;
    const size_t stop = c.pos + length;
    Layer layer;
    *keep = false;

    // Pass one: the name, the extent, and the string tables.
    std::vector<std::string> keys;
    std::vector<std::string> values;
    {
        Cursor sub{ c.data, start, stop, false };
        Tag tag;
        while (readTag(sub, &tag)) {
            if (tag.field == 1 && tag.wire == 2)
                layer.name = readString(sub);
            else if (tag.field == 3 && tag.wire == 2)
                keys.push_back(readString(sub));
            else if (tag.field == 4 && tag.wire == 2)
                values.push_back(readValue(sub));
            else if (tag.field == 5 && tag.wire == 0)
                layer.extent = uint32_t(sub.varint());
            else if (!skipField(sub, tag))
                break;
            if (sub.bad)
                break;
        }
        if (sub.bad) {
            c.bad = true;
            c.pos = stop;
            return layer;
        }
    }

    if (!wantedLayers.empty() && !wanted(wantedLayers, layer.name)) {
        c.pos = stop;
        return layer;
    }
    *keep = true;

    // Pass two: the features, now that a tag index can be resolved.
    Cursor sub{ c.data, start, stop, false };
    Tag tag;
    while (readTag(sub, &tag)) {
        if (tag.field != 2 || tag.wire != 2) {
            if (!skipField(sub, tag))
                break;
            continue;
        }
        const size_t featureLength = size_t(sub.varint());
        if (sub.bad || featureLength > sub.end - sub.pos) {
            sub.bad = true;
            break;
        }
        Cursor fc{ sub.data, sub.pos, sub.pos + featureLength, false };
        sub.pos += featureLength;

        Feature feature;
        std::vector<uint32_t> tags;
        size_t geometryAt = 0;
        size_t geometryLength = 0;

        Tag ftag;
        while (readTag(fc, &ftag)) {
            if (ftag.field == 3 && ftag.wire == 0) {
                const uint64_t t = fc.varint();
                feature.type = (t <= 3) ? GeomType(t) : GeomType::Unknown;
            } else if (ftag.field == 2 && ftag.wire == 2) {
                const size_t n = size_t(fc.varint());
                if (fc.bad || n > fc.end - fc.pos) {
                    fc.bad = true;
                    break;
                }
                Cursor tc{ fc.data, fc.pos, fc.pos + n, false };
                fc.pos += n;
                while (!tc.atEnd())
                    tags.push_back(uint32_t(tc.varint()));
            } else if (ftag.field == 4 && ftag.wire == 2) {
                // Remembered rather than decoded here: the type arrives in
                // field 3 and a tile may write it after the geometry, and the
                // type decides how a MoveTo is grouped.
                geometryLength = size_t(fc.varint());
                if (fc.bad || geometryLength > fc.end - fc.pos) {
                    fc.bad = true;
                    break;
                }
                geometryAt = fc.pos;
                fc.skip(geometryLength);
            } else if (!skipField(fc, ftag)) {
                break;
            }
            if (fc.bad)
                break;
        }
        if (fc.bad) {
            sub.bad = true;
            break;
        }

        if (!wantedKeys.empty()) {
            for (size_t i = 0; i + 1 < tags.size(); i += 2) {
                const uint32_t k = tags[i];
                const uint32_t v = tags[i + 1];
                if (k >= keys.size() || v >= values.size())
                    continue;
                if (wanted(wantedKeys, keys[k]))
                    feature.attributes.push_back({ keys[k], values[v] });
            }
        }

        if (geometryLength > 0) {
            Cursor gc{ fc.data, geometryAt, geometryAt + geometryLength, false };
            decodeGeometry(gc, geometryLength, feature.type, &feature);
        }

        layer.features.push_back(std::move(feature));
    }

    if (sub.bad)
        c.bad = true;
    c.pos = stop;
    return layer;
}

} // namespace

std::string Feature::attribute(const std::string &key) const
{
    for (const auto &pair : attributes) {
        if (pair.first == key)
            return pair.second;
    }
    return std::string();
}

std::vector<Layer> decode(const std::vector<uint8_t> &data,
                           const std::vector<std::string> &wantedLayers,
                           const std::vector<std::string> &wantedKeys, std::string *error)
{
    std::vector<Layer> layers;
    if (data.empty()) {
        if (error)
            *error = "empty tile";
        return layers;
    }

    Cursor c{ data.data(), 0, data.size(), false };
    Tag tag;
    while (readTag(c, &tag)) {
        if (tag.field != 3 || tag.wire != 2) {
            if (!skipField(c, tag))
                break;
            continue;
        }
        const size_t length = size_t(c.varint());
        if (c.bad || length > c.end - c.pos) {
            c.bad = true;
            break;
        }
        bool keep = false;
        Layer layer = readLayer(c, length, wantedKeys, &keep, wantedLayers);
        if (keep)
            layers.push_back(std::move(layer));
        if (c.bad)
            break;
    }

    if (c.bad && error)
        *error = "malformed tile";
    return layers;
}

} // namespace Mvt
