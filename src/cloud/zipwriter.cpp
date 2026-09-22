#include "zipwriter.h"

#include <zlib.h>

#include <cstring>

namespace ZipWriter {

namespace {

void put16(std::vector<uint8_t> *out, uint16_t v)
{
    out->push_back(static_cast<uint8_t>(v & 0xFF));
    out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void put32(std::vector<uint8_t> *out, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void putBytes(std::vector<uint8_t> *out, const std::string &s)
{
    out->insert(out->end(), s.begin(), s.end());
}

// Raw deflate - no zlib header or trailer, which is what ZIP's method 8
// means. That is the negative-windowBits form of deflateInit2.
bool deflateRaw(const std::string &input, std::string *output)
{
    z_stream stream;
    std::memset(&stream, 0, sizeof(stream));
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS,
                      8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }

    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());

    output->clear();
    char buffer[32768];
    int status = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef *>(buffer);
        stream.avail_out = sizeof(buffer);
        status = deflate(&stream, Z_FINISH);
        if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) {
            deflateEnd(&stream);
            return false;
        }
        output->append(buffer, sizeof(buffer) - stream.avail_out);
    } while (status != Z_STREAM_END);

    deflateEnd(&stream);
    return true;
}

} // namespace

std::vector<uint8_t> build(const std::vector<Entry> &entries)
{
    struct Placed
    {
        std::string name;
        uint32_t crc = 0;
        uint32_t compressedSize = 0;
        uint32_t uncompressedSize = 0;
        uint32_t localHeaderOffset = 0;
    };

    std::vector<uint8_t> out;
    std::vector<Placed> placed;
    placed.reserve(entries.size());

    for (const Entry &entry : entries) {
        std::string compressed;
        if (!deflateRaw(entry.content, &compressed))
            return {};

        Placed p;
        p.name = entry.name;
        p.crc = static_cast<uint32_t>(crc32(
                0L, reinterpret_cast<const Bytef *>(entry.content.data()),
                static_cast<uInt>(entry.content.size())));
        p.compressedSize = static_cast<uint32_t>(compressed.size());
        p.uncompressedSize = static_cast<uint32_t>(entry.content.size());
        p.localHeaderOffset = static_cast<uint32_t>(out.size());

        put32(&out, 0x04034B50);           // local file header signature
        put16(&out, 20);                    // version needed (2.0 = deflate)
        put16(&out, 0);                     // flags
        put16(&out, 8);                     // method: deflate
        put16(&out, 0);                     // modification time
        put16(&out, 0);                     // modification date
        put32(&out, p.crc);
        put32(&out, p.compressedSize);
        put32(&out, p.uncompressedSize);
        put16(&out, static_cast<uint16_t>(p.name.size()));
        put16(&out, 0);                     // extra field length
        putBytes(&out, p.name);
        out.insert(out.end(), compressed.begin(), compressed.end());

        placed.push_back(p);
    }

    const uint32_t centralStart = static_cast<uint32_t>(out.size());
    for (const Placed &p : placed) {
        put32(&out, 0x02014B50);           // central directory header signature
        put16(&out, 20);                    // version made by
        put16(&out, 20);                    // version needed
        put16(&out, 0);                     // flags
        put16(&out, 8);                     // method
        put16(&out, 0);                     // time
        put16(&out, 0);                     // date
        put32(&out, p.crc);
        put32(&out, p.compressedSize);
        put32(&out, p.uncompressedSize);
        put16(&out, static_cast<uint16_t>(p.name.size()));
        put16(&out, 0);                     // extra
        put16(&out, 0);                     // comment
        put16(&out, 0);                     // disk number
        put16(&out, 0);                     // internal attributes
        put32(&out, 0);                     // external attributes
        put32(&out, p.localHeaderOffset);
        putBytes(&out, p.name);
    }
    const uint32_t centralSize = static_cast<uint32_t>(out.size()) - centralStart;

    put32(&out, 0x06054B50);               // end of central directory
    put16(&out, 0);                         // this disk
    put16(&out, 0);                         // disk with central directory
    put16(&out, static_cast<uint16_t>(placed.size()));
    put16(&out, static_cast<uint16_t>(placed.size()));
    put32(&out, centralSize);
    put32(&out, centralStart);
    put16(&out, 0);                         // comment length

    return out;
}

} // namespace ZipWriter
