#include "logentriesdecoder.h"

namespace LogEntries {

namespace {

constexpr size_t kPrefixLen = 8;     // 0xF0, 2-byte handle, 0x01 0x80 0x00, 0xC8 0x00
constexpr size_t kHeaderLen = 2;     // protocol_v9 structure header (alignment selector etc.)
constexpr size_t kArrayMetaLen = 16; // element type tag, sub-length, count, 6 undecoded bytes
constexpr size_t kCountOffsetInPayload = 4;
constexpr size_t kRecordsOffsetInPayload = 16;
constexpr size_t kRecordLen = 24;

uint16_t readU16(const std::vector<uint8_t> &b, size_t offset)
{
    return static_cast<uint16_t>(b[offset]) | (static_cast<uint16_t>(b[offset + 1]) << 8);
}

uint32_t readU32(const std::vector<uint8_t> &b, size_t offset)
{
    return static_cast<uint32_t>(b[offset])
            | (static_cast<uint32_t>(b[offset + 1]) << 8)
            | (static_cast<uint32_t>(b[offset + 2]) << 16)
            | (static_cast<uint32_t>(b[offset + 3]) << 24);
}

} // namespace

std::vector<Entry> decode(const std::vector<uint8_t> &responseBody)
{
    std::vector<Entry> result;

    const size_t payloadStart = kPrefixLen + kHeaderLen;
    if (responseBody.size() < payloadStart + kArrayMetaLen)
        return result;

    const uint16_t count = readU16(responseBody, payloadStart + kCountOffsetInPayload);

    const size_t recordsStart = payloadStart + kRecordsOffsetInPayload;
    const size_t needed = recordsStart + static_cast<size_t>(count) * kRecordLen;
    if (responseBody.size() < needed)
        return result;

    result.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        const size_t recOffset = recordsStart + static_cast<size_t>(i) * kRecordLen;
        Entry entry;
        entry.id = readU32(responseBody, recOffset);
        entry.modificationTimestamp = readU32(responseBody, recOffset + 4);
        result.push_back(entry);
    }
    return result;
}

} // namespace LogEntries
