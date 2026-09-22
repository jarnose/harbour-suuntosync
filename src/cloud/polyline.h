#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Decodes Google's "encoded polyline" format, which is what the Suunto
// cloud's workout list returns in each workout's `polyline` field - so a
// cloud-synced workout can show the same route a BLE-synced one does,
// without any extra request.
//
// Qt-free (STL only) so it can be tested against the format's own published
// example with plain g++, same discipline as the rest of this project's
// decoders.
//
// The algorithm: each coordinate is a signed offset from the previous one,
// scaled by 1e5, zigzag-encoded, then split into 5-bit groups written
// little-endian with the high bit set on every group but the last, each
// group offset by 63 to land in printable ASCII.
namespace Polyline {

struct Point
{
    double latitude = 0;
    double longitude = 0;
};

// Returns an empty vector for malformed input rather than throwing - a
// workout with an unparseable polyline should lose its map, not its row.
//
// precision is the number of decimal places the encoder used. Google's
// format and every Suunto polyline this project has seen use 5; the
// parameter exists so a 6-decimal polyline (some providers use it) doesn't
// silently decode to a route a tenth of its real size.
std::vector<Point> decode(const std::string &encoded, int precision = 5);

} // namespace Polyline
