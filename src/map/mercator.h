#pragma once

#include <cstdint>

// Web Mercator, the projection every slippy-map tile scheme uses, and the
// tile arithmetic that goes with it.
//
// Qt-free and STL-free, so it compiles and runs with plain g++ and is tested
// against values computed independently - see tests/test_mercator.cpp.
//
// This exists because the route drawing already had a projection and it was
// the wrong one: AppController::workoutRoute fitted a bounding box with a
// cos(latitude) correction, which is equirectangular. That is fine for a bare
// polyline - at five kilometres the difference from Mercator is invisible -
// but tiles are drawn in Mercator, so a route projected any other way would
// not sit on the roads underneath it.
namespace Mercator {

// A position in the global tile grid, 0..1 over the whole world, with (0,0)
// at the north-west corner and y increasing southwards - the same direction
// tile numbering and canvas pixels both use.
struct Global
{
    double x = 0;
    double y = 0;
};

// Latitude is clamped to the projection's own limits (+-85.051129 degrees,
// where y would run to infinity). Nothing a watch records comes near them,
// but a corrupt sample reading 1e9 should produce an edge rather than a NaN
// that silently poisons every later comparison.
Global fromLatLon(double latitude, double longitude);

// The tiles covering a bounding box, and the zoom chosen for it.
//
// `zoom` is the largest one no greater than `maxZoom` whose tile grid covers
// the box in at most `maxTilesPerSide` tiles each way - computed by trying
// each zoom rather than estimated from the span, because the box's position
// decides how many tile boundaries it crosses: the same span needs one tile
// or two depending on where it falls.
//
// The range is inclusive, so the grid is (x1 - x0 + 1) by (y1 - y0 + 1).
// A degenerate box - a workout recorded standing still - yields a single
// tile at `maxZoom` rather than a division by zero.
struct TileRange
{
    int zoom = 0;
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    int columns() const { return x1 - x0 + 1; }
    int rows() const { return y1 - y0 + 1; }
};

TileRange chooseTiles(double minLatitude, double minLongitude, double maxLatitude,
                       double maxLongitude, int maxZoom, int maxTilesPerSide);

// The same, for a box already in global coordinates - which is what the map
// item has, because the view it draws is the route's own box widened to the
// item's aspect ratio, and that widening happens after projection.
TileRange chooseTilesGlobal(double minX, double minY, double maxX, double maxY, int maxZoom,
                             int maxTilesPerSide);

// Where a position falls inside a tile range, as a fraction of the whole
// grid: (0,0) is the grid's north-west corner and (1,1) its south-east one.
// Outside 0..1 is possible and fine - a route point can sit in the buffer
// just beyond a tile's edge.
Global withinRange(const Global &position, const TileRange &range);

} // namespace Mercator
