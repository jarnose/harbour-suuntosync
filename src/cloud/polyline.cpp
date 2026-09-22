#include "polyline.h"

#include <cmath>
#include <cstdint>

namespace Polyline {

namespace {

// Reads one zigzag-encoded varint. Returns false if the input ends
// mid-value or a group is outside the printable range the format uses.
bool readValue(const std::string &encoded, size_t *pos, int32_t *out)
{
    uint32_t result = 0;
    int shift = 0;
    while (true) {
        if (*pos >= encoded.size())
            return false;
        const int c = static_cast<unsigned char>(encoded[*pos]) - 63;
        ++*pos;
        if (c < 0)
            return false;
        // Five bits per group, at most six groups for a 32-bit value.
        if (shift > 30)
            return false;
        result |= static_cast<uint32_t>(c & 0x1F) << shift;
        shift += 5;
        if ((c & 0x20) == 0)
            break;
    }
    // Zigzag: the low bit is the sign.
    *out = (result & 1) ? ~static_cast<int32_t>(result >> 1)
                         : static_cast<int32_t>(result >> 1);
    return true;
}

} // namespace

std::vector<Point> decode(const std::string &encoded, int precision)
{
    std::vector<Point> points;
    const double scale = std::pow(10.0, precision);

    int32_t lat = 0;
    int32_t lon = 0;
    size_t pos = 0;
    while (pos < encoded.size()) {
        int32_t dLat = 0;
        int32_t dLon = 0;
        if (!readValue(encoded, &pos, &dLat) || !readValue(encoded, &pos, &dLon))
            return {}; // truncated or malformed - see the header
        lat += dLat;
        lon += dLon;
        points.push_back({ lat / scale, lon / scale });
    }
    return points;
}

} // namespace Polyline
