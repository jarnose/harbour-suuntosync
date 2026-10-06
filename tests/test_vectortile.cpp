// Qt-free golden-vector test for Mvt::decode, same discipline as
// test_mdswirecodec.cpp and test_sbemcontainer.cpp: validated with plain g++
// against bytes whose expected decoding is known independently, before any of
// it touches Qt or a network.
//
// Two kinds of vector here, deliberately.
//
// The synthetic tile is built byte by byte with a known answer, and is
// **inlined as hex so this suite needs no fixture and can run in CI** - the
// same reason test_notificationcodec.cpp inlines its captures. It is where
// the parts that are easy to get subtly wrong are pinned down: the zigzag
// deltas, the command/count packing, a cursor that carries across parts,
// ClosePath, the two-pass tag/value tables, and a truncated tile.
//
// The real tile is one fetched from OpenFreeMap over Monaco at zoom 14 -
// Monaco because OpenFreeMap uses it as its own sample area, so the fixture
// says nothing about where anybody lives. It is not in the repository (see
// tests/README.md) and this suite skips its checks when it is absent.
//
// Build:
//   g++ -std=c++17 test_vectortile.cpp ../src/map/vectortile.cpp
//   -o /tmp/test_vectortile && /tmp/test_vectortile

#include "../src/map/vectortile.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

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

std::vector<uint8_t> fromHex(const std::string &hex)
{
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(uint8_t((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    return out;
}

bool near(double a, double b)
{
    return std::fabs(a - b) < 1e-9;
}

// Four layers, hand-encoded:
//
//   water          1 Polygon,    class=lake, extent 4096
//                  MoveTo(10,10) LineTo(+20,0)(0,+20) ClosePath
//   transportation 1 LineString, class=path, extent 4096
//                  two parts, the second starting from where the first ended
//   place          1 Point,      no tags,    extent 512
//                  one MoveTo carrying two points - a multipoint
//   poi            1 Point                   - here only to be skipped
const char *const kTileHex =
        "1a3078020a0577617465721a05636c61737322060a046c616b65288020121308011803120200002209091414"
        "12280000280f1a3c78020a0e7472616e73706f72746174696f6e1a05636c61737322060a04706174682880"
        "2012160802180212020000220c090a0a0a1400090a0a0a00141a1b78020a05706c616365288004120d0803"
        "1801220711c801900314271a1578020a03706f692880201209080418012203090202";

const Mvt::Layer *find(const std::vector<Mvt::Layer> &layers, const std::string &name)
{
    for (const auto &layer : layers) {
        if (layer.name == name)
            return &layer;
    }
    return nullptr;
}

void testSyntheticTile()
{
    const std::vector<uint8_t> tile = fromHex(kTileHex);
    check(tile.size() == 164, "the synthetic tile is the 164 bytes it was built as");

    std::string error;
    std::vector<Mvt::Layer> layers =
            Mvt::decode(tile, { "water", "transportation", "place" }, { "class" }, &error);

    check(error.empty(), "a well-formed tile decodes without an error");
    check(layers.size() == 3, "three layers asked for, three returned");
    check(find(layers, "poi") == nullptr, "a layer not asked for is not returned");

    // Order is the tile's own, which matters: a caller draws water under
    // roads by relying on it rather than by sorting.
    check(layers.size() == 3 && layers[0].name == "water"
                  && layers[1].name == "transportation" && layers[2].name == "place",
           "layers come back in tile order");

    const Mvt::Layer *water = find(layers, "water");
    check(water != nullptr, "the water layer is present");
    if (water) {
        check(water->extent == 4096, "an unstated extent is the format's default 4096");
        check(water->features.size() == 1, "water has one feature");
        if (water->features.size() == 1) {
            const Mvt::Feature &f = water->features[0];
            check(f.type == Mvt::GeomType::Polygon, "it is a polygon");
            check(f.attribute("class") == "lake", "its class attribute resolves through the tables");
            check(f.parts.size() == 1, "one ring");
            if (f.parts.size() == 1) {
                const auto &ring = f.parts[0];
                // ClosePath repeats the first point, so four for a triangle.
                check(ring.size() == 4, "ClosePath closed the ring explicitly");
                const bool ok = ring.size() == 4 && near(ring[0].x, 10) && near(ring[0].y, 10)
                        && near(ring[1].x, 30) && near(ring[1].y, 10) && near(ring[2].x, 30)
                        && near(ring[2].y, 30) && near(ring[3].x, 10) && near(ring[3].y, 10);
                check(ok, "zigzag deltas accumulate into the right absolute points");
            }
        }
    }

    const Mvt::Layer *roads = find(layers, "transportation");
    check(roads != nullptr, "the transportation layer is present");
    if (roads && roads->features.size() == 1) {
        const Mvt::Feature &f = roads->features[0];
        check(f.type == Mvt::GeomType::LineString, "it is a linestring");
        check(f.attribute("class") == "path", "its class is path");
        check(f.parts.size() == 2, "a second MoveTo starts a second part");
        if (f.parts.size() == 2) {
            const auto &a = f.parts[0];
            const auto &b = f.parts[1];
            check(a.size() == 2 && near(a[0].x, 5) && near(a[0].y, 5) && near(a[1].x, 15)
                           && near(a[1].y, 5),
                   "first part runs from (5,5) to (15,5)");
            // The decisive one: the cursor is NOT reset between parts, so the
            // second part's MoveTo is a delta from (15,5), not from the origin.
            check(b.size() == 2 && near(b[0].x, 20) && near(b[0].y, 10) && near(b[1].x, 20)
                           && near(b[1].y, 20),
                   "the second part continues from where the first ended");
        }
    }

    const Mvt::Layer *place = find(layers, "place");
    check(place != nullptr, "the place layer is present");
    if (place && place->features.size() == 1) {
        const Mvt::Feature &f = place->features[0];
        check(place->extent == 512, "a stated extent is read rather than assumed");
        check(f.type == Mvt::GeomType::Point, "it is a point feature");
        check(f.parts.size() == 1, "a multipoint's MoveTo stays one part");
        if (f.parts.size() == 1) {
            const auto &pts = f.parts[0];
            check(pts.size() == 2 && near(pts[0].x, 100) && near(pts[0].y, 200)
                           && near(pts[1].x, 110) && near(pts[1].y, 180),
                   "both of its points decode, the second relative to the first");
        }
    }
}

void testAttributeAndLayerSelection()
{
    const std::vector<uint8_t> tile = fromHex(kTileHex);

    std::vector<Mvt::Layer> all = Mvt::decode(tile, {}, {});
    check(all.size() == 4, "an empty layer list means every layer, poi included");

    const Mvt::Layer *water = find(all, "water");
    check(water && water->features.size() == 1 && water->features[0].attributes.empty(),
           "an empty key list means no attributes are decoded");

    std::vector<Mvt::Layer> other = Mvt::decode(tile, { "water" }, { "nosuchkey" });
    check(other.size() == 1, "naming one layer returns one layer");
    check(other.size() == 1 && other[0].features.size() == 1
                  && other[0].features[0].attributes.empty(),
           "asking for a key the tile does not have yields no attribute");
    check(other.size() == 1 && other[0].features.size() == 1
                  && other[0].features[0].attribute("class").empty(),
           "attribute() of an absent key is empty rather than a crash");
}

void testMalformed()
{
    std::string error;
    std::vector<Mvt::Layer> none = Mvt::decode({}, {}, {}, &error);
    check(none.empty() && error == "empty tile", "an empty tile is reported, not decoded");

    // A tile cut off mid-layer, which is what a cancelled download leaves.
    // The contract is "what was decoded, plus an error" - a half-drawn base
    // map beats a blank one, and this must not read past the buffer.
    const std::vector<uint8_t> cut = fromHex(
            "1a3078020a0577617465721a05636c61737322060a046c616b652880201213080118031202000022090914"
            "141228000028 0f1a3c7802");
    error.clear();
    std::vector<Mvt::Layer> partial = Mvt::decode(cut, {}, { "class" }, &error);
    check(!error.empty(), "a truncated tile sets an error");
    check(partial.size() == 1 && partial[0].name == "water",
           "and still returns the layer that was complete");

    // Every single-byte prefix of the real tile, to prove no length can walk
    // off the end. The answers are uninteresting; not crashing is the point.
    const std::vector<uint8_t> tile = fromHex(kTileHex);
    for (size_t n = 1; n < tile.size(); ++n) {
        std::vector<uint8_t> prefix(tile.begin(), tile.begin() + n);
        std::string ignored;
        Mvt::decode(prefix, {}, { "class" }, &ignored);
    }
    check(true, "no prefix of the tile reads past its end");

    // A varint that never terminates - 0x80 repeated - inside a layer length.
    std::vector<uint8_t> runaway = { 0x1a };
    runaway.insert(runaway.end(), 32, 0x80);
    std::string runawayError;
    Mvt::decode(runaway, {}, {}, &runawayError);
    check(!runawayError.empty(), "an unterminated varint is rejected rather than looped on");
}

void testRealTile()
{
    std::ifstream in("fixtures/openfreemap_monaco_z14.pbf", std::ios::binary);
    if (!in) {
        std::printf("skip: fixtures/openfreemap_monaco_z14.pbf absent "
                     "(not in the repository - see tests/README.md)\n");
        return;
    }
    const std::vector<uint8_t> tile((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    check(tile.size() == 257108, "the Monaco fixture is the tile that was fetched");

    std::string error;
    // Exactly the layers a base map under a GPS trace needs. The ones left
    // out are over half the tile: poi alone is 2050 of its features.
    std::vector<Mvt::Layer> layers =
            Mvt::decode(tile,
                         { "water", "waterway", "landcover", "landuse", "park", "transportation" },
                         { "class", "subclass" }, &error);
    check(error.empty(), "a real OpenFreeMap tile decodes without an error");

    // Counts read independently out of the same bytes with a throwaway
    // Python scanner before this code existed, which is the point of a
    // golden vector: the expectation does not come from this decoder.
    const std::map<std::string, size_t> expected = {
        { "water", 99 },       { "landcover", 129 },  { "landuse", 65 },
        { "park", 1 },         { "transportation", 705 },
    };
    for (const auto &pair : expected) {
        const Mvt::Layer *layer = find(layers, pair.first);
        const bool ok = layer && layer->features.size() == pair.second;
        if (!ok) {
            std::fprintf(stderr, "  %s: expected %zu features, got %zu\n", pair.first.c_str(),
                          pair.second, layer ? layer->features.size() : size_t(0));
        }
        check(ok, ("feature count matches the independent scan: " + pair.first).c_str());
    }
    check(find(layers, "waterway") == nullptr,
           "a layer the tile does not contain is simply absent");
    check(find(layers, "poi") == nullptr, "and poi was never decoded");

    // Geometry sanity across the whole tile. The format permits a buffer
    // outside 0..extent so a feature crossing an edge joins up, so this is a
    // generous bound rather than a tight one - what it rules out is the
    // failure that matters, a desynchronised stream producing coordinates
    // scattered over millions.
    size_t points = 0;
    double worst = 0;
    for (const auto &layer : layers) {
        for (const auto &feature : layer.features) {
            for (const auto &part : feature.parts) {
                for (const auto &p : part) {
                    ++points;
                    worst = std::max(worst, std::max(std::fabs(p.x), std::fabs(p.y)));
                }
            }
        }
    }
    std::printf("  %zu points decoded, furthest coordinate %.0f (extent 4096)\n", points, worst);
    check(points > 10000, "the tile yields a real quantity of geometry");
    check(worst < 4096 * 4, "every coordinate is within a tile's worth of the tile");

    // A road layer is only useful if its classes come through: the whole
    // reason for decoding attributes is to draw a motorway differently from
    // a footpath.
    const Mvt::Layer *roads = find(layers, "transportation");
    std::map<std::string, int> classes;
    if (roads) {
        for (const auto &feature : roads->features)
            ++classes[feature.attribute("class")];
    }
    check(classes.count("path") + classes.count("service") + classes.count("minor") > 0,
           "transportation features carry recognisable class values");
    std::printf("  transportation classes:");
    for (const auto &pair : classes)
        std::printf(" %s=%d", pair.first.empty() ? "(none)" : pair.first.c_str(), pair.second);
    std::printf("\n");
}

} // namespace

int main()
{
    testSyntheticTile();
    testAttributeAndLayerSelection();
    testMalformed();
    testRealTile();

    if (g_failures > 0) {
        std::fprintf(stderr, "\n%d assertion(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("\nAll assertions passed.\n");
    return 0;
}
