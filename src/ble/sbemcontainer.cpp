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
constexpr char kMagic[] = "SBEM0103";
constexpr size_t kMagicLen = 8;
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

std::vector<Chunk> parseContainer(const std::vector<uint8_t> &decompressed)
{
    std::vector<Chunk> chunks;
    if (decompressed.size() < kMagicLen || std::memcmp(decompressed.data(), kMagic, kMagicLen) != 0)
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
