// Qt-free test for Mercator::fromLatLon/chooseTiles/withinRange.
//
// The expected values come from an independent Python re-implementation of
// the same standard formulae, not from this code - the tile numbers for
// Monaco and Tampere at zoom 14 were computed there first, and the Monaco one
// is corroborated a second way: it is the tile that was actually fetched from
// OpenFreeMap to make tests/fixtures/openfreemap_monaco_z14.pbf, so if the
// arithmetic here were wrong the fixture would be of somewhere else.
//
// No fixture needed; this suite runs in CI.
//
// Build:
//   g++ -std=c++17 test_mercator.cpp ../src/map/mercator.cpp
//   -o /tmp/test_mercator && /tmp/test_mercator

#include "../src/map/mercator.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void check(bool condition, const char *what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

bool near(double a, double b, double tolerance = 1e-12)
{
    return std::fabs(a - b) <= tolerance;
}

void testProjection()
{
    Mercator::Global origin = Mercator::fromLatLon(0.0, 0.0);
    check(near(origin.x, 0.5) && near(origin.y, 0.5),
           "null island is the centre of the world square");

    // Tampere, against the Python value 0.5660027777777777, 0.2818832548427405.
    Mercator::Global tampere = Mercator::fromLatLon(61.4978, 23.7610);
    check(near(tampere.x, 0.5660027777777777, 1e-15), "longitude matches to the last digit");
    check(near(tampere.y, 0.2818832548427405, 1e-15), "latitude matches to the last digit");

    // North is up: a larger latitude is a smaller y.
    check(Mercator::fromLatLon(62.0, 0.0).y < Mercator::fromLatLon(61.0, 0.0).y,
           "y increases southwards, as tile numbering does");

    Mercator::Global ne = Mercator::fromLatLon(85.0511287798066, 180.0);
    check(near(ne.x, 1.0) && near(ne.y, 0.0, 1e-9), "the north-east corner is (1,0)");
    Mercator::Global sw = Mercator::fromLatLon(-85.0511287798066, -180.0);
    check(near(sw.x, 0.0) && near(sw.y, 1.0, 1e-9), "the south-west corner is (0,1)");

    // A corrupt sample must not produce a NaN that poisons every comparison
    // downstream - every bound here is clamped.
    Mercator::Global silly = Mercator::fromLatLon(1e9, 1e9);
    check(near(silly.x, 1.0) && near(silly.y, 0.0, 1e-9), "absurd input clamps to an edge");
    Mercator::Global nan = Mercator::fromLatLon(std::nan(""), std::nan(""));
    check(nan.x == nan.x && nan.y == nan.y, "a NaN latitude does not come back out as NaN");
}

void testTileNumbers()
{
    // A degenerate box: one point. The tile it falls in, at the top zoom.
    Mercator::TileRange monaco = Mercator::chooseTiles(43.7384, 7.4246, 43.7384, 7.4246, 14, 3);
    check(monaco.zoom == 14 && monaco.x0 == 8529 && monaco.y0 == 5974,
           "Monaco at zoom 14 is tile 8529/5974 - the fixture's own tile");
    check(monaco.columns() == 1 && monaco.rows() == 1,
           "a workout recorded standing still is one tile, not a division by zero");

    Mercator::TileRange tampere = Mercator::chooseTiles(61.4978, 23.7610, 61.4978, 23.7610, 14, 3);
    check(tampere.zoom == 14 && tampere.x0 == 9273 && tampere.y0 == 4618,
           "Tampere at zoom 14 is tile 9273/4618");
}

void testZoomChoice()
{
    // A box of about five kilometres, the size of a real outing. The Python
    // scan of the same box gives, per zoom: 14 -> 4x4, 13 -> 2x3, 12 -> 1x2.
    // So a three-tile limit must land on 13, and a two-tile limit on 12.
    const double minLat = 61.4800, maxLat = 61.5100;
    const double minLon = 23.7400, maxLon = 23.8000;

    Mercator::TileRange three = Mercator::chooseTiles(minLat, minLon, maxLat, maxLon, 14, 3);
    check(three.zoom == 13, "with three tiles a side allowed, zoom 13 is the closest that fits");
    check(three.columns() == 2 && three.rows() == 3, "and it is the 2x3 grid Python predicted");
    check(three.x0 == 4636 && three.x1 == 4637 && three.y0 == 2308 && three.y1 == 2310,
           "down to the tile numbers");

    Mercator::TileRange two = Mercator::chooseTiles(minLat, minLon, maxLat, maxLon, 14, 2);
    check(two.zoom == 12 && two.columns() == 1 && two.rows() == 2,
           "a tighter tile budget drops a zoom level rather than cropping");

    // The choice is made by trying each zoom, not by dividing the span,
    // because where the box falls decides how many boundaries it crosses. A
    // span narrower than a tile still needs two when it straddles an edge.
    Mercator::TileRange straddle = Mercator::chooseTiles(0.0, -0.001, 0.001, 0.001, 14, 1);
    check(straddle.columns() == 1 && straddle.rows() == 1,
           "a box straddling the meridian is given a zoom where it fits in one tile");

    // Never above what the source offers: OpenFreeMap stops at 14.
    Mercator::TileRange capped = Mercator::chooseTiles(43.7384, 7.4246, 43.7385, 7.4247, 14, 3);
    check(capped.zoom <= 14, "the zoom never exceeds the source's maximum");
}

void testWithinRange()
{
    Mercator::TileRange range = Mercator::chooseTiles(61.4800, 23.7400, 61.5100, 23.8000, 14, 3);

    // The grid's own corners, by construction.
    const double scale = double(1 << range.zoom);
    Mercator::Global topLeft{ double(range.x0) / scale, double(range.y0) / scale };
    Mercator::Global atTopLeft = Mercator::withinRange(topLeft, range);
    check(near(atTopLeft.x, 0.0) && near(atTopLeft.y, 0.0),
           "the grid's north-west corner is (0,0)");

    Mercator::Global bottomRight{ double(range.x1 + 1) / scale, double(range.y1 + 1) / scale };
    Mercator::Global atBottomRight = Mercator::withinRange(bottomRight, range);
    check(near(atBottomRight.x, 1.0) && near(atBottomRight.y, 1.0),
           "and its south-east corner is (1,1)");

    // Every corner of the box that chose this grid must land inside it -
    // which is the property the whole thing exists for, and the one that
    // would break if latitude's inversion were handled the wrong way round.
    const double lats[] = { 61.4800, 61.5100 };
    const double lons[] = { 23.7400, 23.8000 };
    bool inside = true;
    for (double lat : lats) {
        for (double lon : lons) {
            Mercator::Global p = Mercator::withinRange(Mercator::fromLatLon(lat, lon), range);
            if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0)
                inside = false;
        }
    }
    check(inside, "every corner of the bounding box falls inside the grid chosen for it");
}

} // namespace

int main()
{
    testProjection();
    testTileNumbers();
    testZoomChoice();
    testWithinRange();

    if (g_failures > 0) {
        std::fprintf(stderr, "\n%d assertion(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("\nAll assertions passed.\n");
    return 0;
}
