#include "fitdecoder.h"

#include "fitcrc.h" // the FIT CRC is the same nibble-table CRC-16 as GFDI's

#include <cstring>

namespace Fit {

namespace {

struct BaseInfo
{
    uint8_t size;
    bool isSigned;
    bool isFloat;
    uint64_t invalid; // the value that means "not set", as the raw unsigned bits
};

const BaseInfo kBase[17] = {
    {1, false, false, 0xFF},                    // enum
    {1, true, false, 0x7F},                     // sint8
    {1, false, false, 0xFF},                    // uint8
    {2, true, false, 0x7FFF},                   // sint16
    {2, false, false, 0xFFFF},                  // uint16
    {4, true, false, 0x7FFFFFFFull},            // sint32
    {4, false, false, 0xFFFFFFFFull},           // uint32
    {1, false, false, 0x00},                    // string
    {4, false, true, 0xFFFFFFFFull},            // float32
    {8, false, true, 0xFFFFFFFFFFFFFFFFull},    // float64
    {1, false, false, 0x00},                    // uint8z
    {2, false, false, 0x0000},                  // uint16z
    {4, false, false, 0x00000000},              // uint32z
    {1, false, false, 0xFF},                    // byte
    {8, true, false, 0x7FFFFFFFFFFFFFFFull},    // sint64
    {8, false, false, 0xFFFFFFFFFFFFFFFFull},   // uint64
    {8, false, false, 0x0000000000000000ull},   // uint64z
};

struct FieldDef
{
    uint8_t number;
    uint8_t size;
    uint8_t baseType;
};

struct Definition
{
    bool valid = false;
    bool bigEndian = false;
    uint16_t global = 0;
    std::vector<FieldDef> fields;
    size_t devBytes = 0;     // total size of developer fields, skipped
    size_t dataBytes = 0;    // total size of one data record, headers excluded
};

// One element of `f` as its raw unsigned bits, in host order.
uint64_t rawBits(const Field &f, size_t width)
{
    uint64_t v = 0;
    if (f.bigEndian) {
        for (size_t i = 0; i < width; ++i)
            v = (v << 8) | f.data[i];
    } else {
        for (size_t i = width; i-- > 0;)
            v = (v << 8) | f.data[i];
    }
    return v;
}

bool usable(const Field &f)
{
    if (f.baseType > Uint64z || f.data == nullptr)
        return false;
    return f.size >= kBase[f.baseType].size;
}

} // namespace

const Field *Message::find(uint8_t number) const
{
    for (const Field &f : fields)
        if (f.number == number)
            return &f;
    return nullptr;
}

bool Message::getInt(uint8_t number, int64_t &out) const
{
    const Field *f = find(number);
    if (!f || !usable(*f) || f->baseType == String)
        return false;
    const BaseInfo &b = kBase[f->baseType];
    const uint64_t raw = rawBits(*f, b.size);
    if (b.isFloat) {
        double d;
        if (!getDouble(number, d))
            return false;
        out = static_cast<int64_t>(d);
        return true;
    }
    if (raw == b.invalid)
        return false;
    if (b.isSigned) {
        const unsigned bits = b.size * 8u;
        int64_t v = static_cast<int64_t>(raw);
        if (bits < 64 && (raw & (1ull << (bits - 1))))
            v = static_cast<int64_t>(raw | (~0ull << bits));
        out = v;
    } else {
        out = static_cast<int64_t>(raw);
    }
    return true;
}

bool Message::getUint(uint8_t number, uint32_t &out) const
{
    int64_t v;
    if (!getInt(number, v) || v < 0 || v > 0xFFFFFFFFll)
        return false;
    out = static_cast<uint32_t>(v);
    return true;
}

bool Message::getDouble(uint8_t number, double &out) const
{
    const Field *f = find(number);
    if (!f || !usable(*f) || f->baseType == String)
        return false;
    const BaseInfo &b = kBase[f->baseType];
    if (b.isFloat) {
        const uint64_t raw = rawBits(*f, b.size);
        if (raw == b.invalid)
            return false;
        if (b.size == 4) {
            const uint32_t r32 = static_cast<uint32_t>(raw);
            float fl;
            std::memcpy(&fl, &r32, 4);
            out = fl;
        } else {
            double d;
            std::memcpy(&d, &raw, 8);
            out = d;
        }
        return out == out; // NaN is "not set" too
    }
    int64_t v;
    if (!getInt(number, v))
        return false;
    out = static_cast<double>(v);
    return true;
}

bool Message::getScaled(uint8_t number, double scale, double offset, double &out) const
{
    double d;
    if (!getDouble(number, d))
        return false;
    out = d / scale - offset;
    return true;
}

bool Message::getString(uint8_t number, std::string &out) const
{
    const Field *f = find(number);
    if (!f || f->baseType != String || f->data == nullptr)
        return false;
    size_t n = 0;
    while (n < f->size && f->data[n] != 0)
        ++n;
    if (n == 0)
        return false;
    out.assign(reinterpret_cast<const char *>(f->data), n);
    return true;
}

namespace {

// Decodes the file that starts at `p` and is `avail` bytes long at most.
// Returns the number of bytes it occupied (0 if it is not a FIT file at all).
size_t decodeOne(const uint8_t *p, size_t avail, const std::function<void(const Message &)> &sink,
                 DecodeStats &stats, std::string &error)
{
    if (avail < 12) {
        error = "too short to be a FIT file";
        return 0;
    }
    const size_t headerSize = p[0];
    if (headerSize != 12 && headerSize != 14) {
        error = "not a FIT file (header size " + std::to_string(headerSize) + ")";
        return 0;
    }
    if (std::memcmp(p + 8, ".FIT", 4) != 0) {
        error = "not a FIT file (no .FIT signature)";
        return 0;
    }
    const size_t dataSize = p[4] | (p[5] << 8) | (static_cast<size_t>(p[6]) << 16) | (static_cast<size_t>(p[7]) << 24);

    size_t end = headerSize + dataSize;
    bool truncated = false;
    if (end + 2 > avail) {
        // The header promises more than there is. Decode what is present.
        truncated = true;
        end = avail >= 2 ? avail - 2 : 0;
    }

    ++stats.files;
    if (truncated)
        stats.truncated = true;

    Definition defs[16];
    Message msg;
    uint32_t lastTimestamp = 0;
    bool haveTimestamp = false;

    size_t pos = headerSize;
    while (pos < end) {
        const uint8_t header = p[pos++];
        uint8_t local;
        bool compressed = false;
        uint32_t timeOffset = 0;

        if (header & 0x80) {
            compressed = true;
            local = (header >> 5) & 0x03;
            timeOffset = header & 0x1F;
        } else {
            local = header & 0x0F;
        }

        if (!compressed && (header & 0x40)) {
            // Definition.
            const bool hasDev = (header & 0x20) != 0;
            if (pos + 5 > end) {
                stats.truncated = true;
                break;
            }
            Definition &d = defs[local];
            d = Definition();
            pos++; // reserved
            d.bigEndian = p[pos++] == 1;
            d.global = d.bigEndian ? static_cast<uint16_t>((p[pos] << 8) | p[pos + 1])
                                   : static_cast<uint16_t>(p[pos] | (p[pos + 1] << 8));
            pos += 2;
            const size_t count = p[pos++];
            if (pos + count * 3 > end) {
                stats.truncated = true;
                break;
            }
            for (size_t i = 0; i < count; ++i) {
                FieldDef f;
                f.number = p[pos];
                f.size = p[pos + 1];
                f.baseType = p[pos + 2] & 0x1F;
                pos += 3;
                d.fields.push_back(f);
                d.dataBytes += f.size;
            }
            if (hasDev) {
                if (pos + 1 > end) {
                    stats.truncated = true;
                    break;
                }
                const size_t devCount = p[pos++];
                if (pos + devCount * 3 > end) {
                    stats.truncated = true;
                    break;
                }
                for (size_t i = 0; i < devCount; ++i) {
                    d.devBytes += p[pos + 1];
                    pos += 3;
                }
                d.dataBytes += d.devBytes;
            }
            d.valid = true;
            continue;
        }

        // Data.
        const Definition &d = defs[local];
        if (!d.valid) {
            error = "a data record uses a local type that was never defined";
            if (stats.messages == 0)
                return 0;
            stats.truncated = true;
            break;
        }
        if (pos + d.dataBytes > end) {
            stats.truncated = true;
            break;
        }

        msg.global = d.global;
        msg.fields.clear();
        msg.hasTimestamp = false;
        size_t at = pos;
        for (const FieldDef &fd : d.fields) {
            Field f;
            f.number = fd.number;
            f.size = fd.size;
            f.baseType = fd.baseType;
            f.data = p + at;
            f.bigEndian = d.bigEndian;
            at += fd.size;
            msg.fields.push_back(f);
        }
        pos += d.dataBytes;

        // The timestamp: a field 253, or implied by a compressed header as an
        // offset above the low five bits of the last one seen.
        const Field *ts = msg.find(253);
        if (ts && usable(*ts) && ts->baseType == Uint32) {
            const uint64_t raw = rawBits(*ts, 4);
            if (raw != 0xFFFFFFFFull) {
                lastTimestamp = static_cast<uint32_t>(raw);
                haveTimestamp = true;
                msg.hasTimestamp = true;
            }
        } else if (compressed && haveTimestamp) {
            uint32_t t = (lastTimestamp & ~0x1Fu) + timeOffset;
            if (timeOffset < (lastTimestamp & 0x1Fu))
                t += 0x20;
            lastTimestamp = t;
            msg.hasTimestamp = true;
        }
        msg.timestamp = haveTimestamp ? lastTimestamp : 0;

        ++stats.messages;
        sink(msg);
    }

    if (truncated)
        return avail;

    // The trailing CRC covers the header and the data.
    const uint16_t expected = Gfdi::crc16(p, end);
    const uint16_t actual = static_cast<uint16_t>(p[end] | (p[end + 1] << 8));
    if (expected != actual)
        stats.crcOk = false;
    return end + 2;
}

} // namespace

bool decode(const uint8_t *data, size_t size, const std::function<void(const Message &)> &sink,
            DecodeStats &stats, std::string &error)
{
    stats = DecodeStats();
    error.clear();
    size_t pos = 0;
    while (pos < size) {
        // Chained files: another header may follow. Trailing zero padding is not one.
        if (pos > 0 && (size - pos < 12 || data[pos] == 0))
            break;
        std::string e;
        const size_t used = decodeOne(data + pos, size - pos, sink, stats, e);
        if (used == 0) {
            if (stats.files == 0 && stats.messages == 0) {
                error = e;
                return false;
            }
            break;
        }
        pos += used;
    }
    return stats.messages > 0;
}

} // namespace Fit
