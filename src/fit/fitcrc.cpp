#include "fitcrc.h"

namespace Gfdi {

namespace {

const uint16_t kTable[16] = {
    0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
    0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400,
};

} // namespace

uint16_t crc16(const uint8_t *data, size_t length, uint16_t seed)
{
    uint32_t crc = seed;
    for (size_t i = 0; i < length; ++i) {
        const uint32_t b = data[i];
        crc = (((crc >> 4) & 0x0FFF) ^ kTable[crc & 0x0F]) ^ kTable[b & 0x0F];
        crc = (((crc >> 4) & 0x0FFF) ^ kTable[crc & 0x0F]) ^ kTable[(b >> 4) & 0x0F];
    }
    return static_cast<uint16_t>(crc);
}

} // namespace Gfdi
