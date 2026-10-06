#include "mercator.h"

#include <cmath>

namespace Mercator {
namespace {

// The latitude where Mercator's y reaches +-1. Spelled out rather than
// written as a magic 85.05112878, which is a rounding of it.
const double kMaxLatitude = 85.0511287798066;

double clamp(double value, double low, double high)
{
    if (!(value == value)) // NaN
        return low;
    return value < low ? low : (value > high ? high : value);
}

} // namespace

Global fromLatLon(double latitude, double longitude)
{
    const double lat = clamp(latitude, -kMaxLatitude, kMaxLatitude);
    const double lon = clamp(longitude, -180.0, 180.0);

    Global out;
    out.x = (lon + 180.0) / 360.0;
    // asinh(tan(lat)) is the same as ln(tan(pi/4 + lat/2)) and avoids the
    // tangent blowing up near the poles being squared off by a logarithm.
    const double radians = lat * M_PI / 180.0;
    out.y = (1.0 - std::asinh(std::tan(radians)) / M_PI) / 2.0;
    return out;
}

TileRange chooseTiles(double minLatitude, double minLongitude, double maxLatitude,
                       double maxLongitude, int maxZoom, int maxTilesPerSide)
{
    if (maxZoom < 0)
        maxZoom = 0;
    if (maxTilesPerSide < 1)
        maxTilesPerSide = 1;

    // North-west and south-east corners. Latitude inverts on the way into
    // Mercator - a larger latitude is a smaller y - so the maximum latitude
    // gives the top edge.
    const Global topLeft = fromLatLon(maxLatitude, minLongitude);
    const Global bottomRight = fromLatLon(minLatitude, maxLongitude);
    return chooseTilesGlobal(topLeft.x, topLeft.y, bottomRight.x, bottomRight.y, maxZoom,
                              maxTilesPerSide);
}

TileRange chooseTilesGlobal(double minX, double minY, double maxX, double maxY, int maxZoom,
                             int maxTilesPerSide)
{
    if (maxZoom < 0)
        maxZoom = 0;
    if (maxTilesPerSide < 1)
        maxTilesPerSide = 1;

    const Global topLeft{ minX, minY };
    const Global bottomRight{ maxX, maxY };

    TileRange best;
    for (int zoom = maxZoom; zoom >= 0; --zoom) {
        const double scale = double(int64_t(1) << zoom);
        int x0 = int(std::floor(topLeft.x * scale));
        int y0 = int(std::floor(topLeft.y * scale));
        int x1 = int(std::floor(bottomRight.x * scale));
        int y1 = int(std::floor(bottomRight.y * scale));

        // A position exactly on the world's eastern or southern edge floors
        // onto a tile index that does not exist.
        const int last = int(scale) - 1;
        x0 = x0 < 0 ? 0 : (x0 > last ? last : x0);
        y0 = y0 < 0 ? 0 : (y0 > last ? last : y0);
        x1 = x1 < 0 ? 0 : (x1 > last ? last : x1);
        y1 = y1 < 0 ? 0 : (y1 > last ? last : y1);
        if (x1 < x0)
            x1 = x0;
        if (y1 < y0)
            y1 = y0;

        best = TileRange{ zoom, x0, y0, x1, y1 };
        if (best.columns() <= maxTilesPerSide && best.rows() <= maxTilesPerSide)
            return best;
    }
    // Zoom 0 is one tile for the whole world, so the loop above always
    // returns before here unless maxTilesPerSide is nonsense.
    return best;
}

Global withinRange(const Global &position, const TileRange &range)
{
    const double scale = double(int64_t(1) << range.zoom);
    const double originX = double(range.x0) / scale;
    const double originY = double(range.y0) / scale;
    const double width = double(range.columns()) / scale;
    const double height = double(range.rows()) / scale;

    Global out;
    out.x = width > 0 ? (position.x - originX) / width : 0.5;
    out.y = height > 0 ? (position.y - originY) / height : 0.5;
    return out;
}

} // namespace Mercator
