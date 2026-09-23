#include "sbemcontainer.h"

// heatshrink is a vendored C library (src/ble/heatshrink/, unmodified from
// upstream) - wrap its header here rather than editing it, so its symbols
// link correctly when called from this C++ translation unit.
extern "C" {
#include "heatshrink/heatshrink_decoder.h"
}

#include <cstring>
#include <stdexcept>

namespace Sbem {

namespace {
constexpr uint8_t kWindowSz2 = 7;
constexpr uint8_t kLookaheadSz2 = 5;
constexpr uint16_t kInputBufferSize = 256;
// Two container versions are known. A workout's /Data and /Summary are
// SBEM0103; the sleep and activity timeline files the watch renders are
// SBEM0102 (confirmed 2026-09-23, docs/watch-push-resources.md). The TLV
// framing is identical in both - id, length, value, with 0xFF escapes -
// which is why one parser serves them. What differs is the meaning of the
// ids: the descriptor table in sbemdescriptors.h describes 0103's workout
// schema and says nothing about 0102's.
constexpr char kMagicPrefix[] = "SBEM01";
constexpr size_t kMagicPrefixLen = 6;
constexpr size_t kMagicLen = 8;

bool hasKnownMagic(const std::vector<uint8_t> &data)
{
    if (data.size() < kMagicLen)
        return false;
    if (std::memcmp(data.data(), kMagicPrefix, kMagicPrefixLen) != 0)
        return false;
    const char v0 = static_cast<char>(data[kMagicPrefixLen]);
    const char v1 = static_cast<char>(data[kMagicPrefixLen + 1]);
    return v0 == '0' && (v1 == '2' || v1 == '3');
}
}

std::vector<uint8_t> heatshrinkDecompress(const std::vector<uint8_t> &compressed)
{
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(kInputBufferSize, kWindowSz2, kLookaheadSz2);
    if (!hsd)
        throw std::runtime_error("heatshrink_decoder_alloc failed");

    std::vector<uint8_t> out;
    uint8_t outbuf[kInputBufferSize];

    auto drain = [&]() {
        HSD_poll_res pres;
        do {
            size_t polled = 0;
            pres = heatshrink_decoder_poll(hsd, outbuf, sizeof(outbuf), &polled);
            if (pres < 0) {
                heatshrink_decoder_free(hsd);
                throw std::runtime_error("heatshrink_decoder_poll failed");
            }
            if (polled)
                out.insert(out.end(), outbuf, outbuf + polled);
        } while (pres == HSDR_POLL_MORE);
    };

    size_t sunkTotal = 0;
    while (sunkTotal < compressed.size()) {
        size_t sunk = 0;
        HSD_sink_res sres = heatshrink_decoder_sink(hsd,
            const_cast<uint8_t *>(compressed.data() + sunkTotal), compressed.size() - sunkTotal, &sunk);
        if (sres < 0) {
            heatshrink_decoder_free(hsd);
            throw std::runtime_error("heatshrink_decoder_sink failed");
        }
        sunkTotal += sunk;
        drain();
    }

    HSD_finish_res fres = heatshrink_decoder_finish(hsd);
    while (fres == HSDR_FINISH_MORE) {
        drain();
        fres = heatshrink_decoder_finish(hsd);
    }

    heatshrink_decoder_free(hsd);
    return out;
}

int64_t decodeLocal64(uint64_t raw)
{
    const int64_t localMs = static_cast<int64_t>(raw & 0x00FFFFFFFFFFFFFFull);
    const int offsetQuarterHours = static_cast<int8_t>(raw >> 56);
    return localMs - static_cast<int64_t>(offsetQuarterHours) * 15 * 60 * 1000;
}

std::vector<Chunk> parseContainer(const std::vector<uint8_t> &decompressed)
{
    std::vector<Chunk> chunks;
    if (!hasKnownMagic(decompressed))
        return chunks;

    size_t pos = kMagicLen;
    while (pos + 2 <= decompressed.size()) {
        uint16_t id = decompressed[pos];
        size_t p = pos + 1;
        if (id == 0xFF) {
            if (p + 2 > decompressed.size())
                break;
            id = static_cast<uint16_t>(decompressed[p])
                | (static_cast<uint16_t>(decompressed[p + 1]) << 8);
            p += 2;
        }
        if (p >= decompressed.size())
            break;
        uint32_t length = decompressed[p];
        ++p;
        if (length == 0xFF) {
            if (p + 4 > decompressed.size())
                break;
            length = static_cast<uint32_t>(decompressed[p])
                | (static_cast<uint32_t>(decompressed[p + 1]) << 8)
                | (static_cast<uint32_t>(decompressed[p + 2]) << 16)
                | (static_cast<uint32_t>(decompressed[p + 3]) << 24);
            p += 4;
        }
        if (p + length > decompressed.size())
            break;

        Chunk chunk;
        chunk.id = id;
        chunk.value.assign(decompressed.begin() + p, decompressed.begin() + p + length);
        chunks.push_back(std::move(chunk));

        pos = p + length;
    }
    return chunks;
}

} // namespace Sbem
