#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Mapbox Vector Tile reader - the format OpenFreeMap serves, which is what
// makes it usable here at all: it needs no API key, sets no request limit and
// permits commercial use, where every raster provider that would have slotted
// into a plain Image grid wants a key that cannot live in a public
// repository. The cost of that is this file.
//
// Qt-free and STL-only on purpose, like every other decoder in this project,
// so it compiles and runs with plain g++ and is tested against real captured
// bytes before anything Qt-side exists. See tests/test_vectortile.cpp.
//
// The wire format is protocol-buffer encoded. The field numbers below are
// from the published vector_tile.proto v2 and were each confirmed against a
// real tile fetched from OpenFreeMap rather than taken on trust:
//
//   Tile    layers = 3
//   Layer   name = 1, features = 2, keys = 3, values = 4, extent = 5,
//           version = 15
//   Feature id = 1, tags = 2 (packed), type = 3, geometry = 4 (packed)
//   Value   string = 1, float = 2, double = 3, int64 = 4, uint64 = 5,
//           sint64 = 6, bool = 7
//
// Not implemented, deliberately: nothing writes tiles, and no styling lives
// here. This turns bytes into coordinates and attribute strings; what is
// drawn and in what colour is the caller's business.
namespace Mvt {

// Tile-local coordinates, 0..extent, with the origin at the tile's top-left
// and y increasing downwards - the same direction a canvas uses, so no flip
// is needed when drawing. Geometry may fall slightly outside the tile: the
// format allows a buffer so a road crossing an edge joins up cleanly, and
// clipping is the caller's choice.
struct Point
{
    double x = 0;
    double y = 0;
};

enum class GeomType {
    Unknown = 0,
    Point = 1,
    LineString = 2,
    Polygon = 3,
};

struct Feature
{
    GeomType type = GeomType::Unknown;

    // One entry per part. A LineString feature has one part per line and a
    // Polygon one per ring - the first ring of a polygon is its outline and
    // any further ones are holes, which the format distinguishes by winding
    // order rather than by position, so a caller that cares must check.
    // Polygon rings are closed explicitly here: the first point is repeated
    // at the end, because ClosePath says to and leaving it implicit only
    // moves the job to every caller.
    //
    // A Point feature is the exception: a single MoveTo may carry many
    // points (that is how the format writes a multipoint), and they all land
    // in one part rather than one each, so `parts.size()` stays the number of
    // geometries rather than becoming the number of points.
    std::vector<std::vector<Point>> parts;

    // Only the keys the caller asked for, in the order they appear in the
    // feature. Values are rendered as strings whatever their wire type -
    // every key this project reads (`class`, `subclass`, `name`) is a string
    // already, and a number formatted as one beats a variant nobody unpacks.
    std::vector<std::pair<std::string, std::string>> attributes;

    // The value for `key`, or an empty string when the feature has no such
    // attribute. An empty string is also a legitimate value, so a caller
    // that must tell the two apart should walk `attributes` instead.
    std::string attribute(const std::string &key) const;
};

struct Layer
{
    std::string name;
    // 4096 unless the tile says otherwise, which is the default the format
    // specifies rather than a guess.
    uint32_t extent = 4096;
    std::vector<Feature> features;
};

// Decodes `data`, keeping only what was asked for.
//
// `wantedLayers` empty means every layer, which is rarely what anyone wants:
// over half of a real OpenFreeMap tile is the `poi` layer (137 kB of 262 kB
// on a measured one), and a base map under a GPS trace needs none of it.
// Naming the layers skips the rest without decoding their geometry at all.
//
// `wantedKeys` empty means no attributes, which is the right default for
// drawing water. Roads need `class` to be drawn at sensible widths.
//
// Returns the layers it kept, in tile order. On a malformed tile it returns
// what it had decoded so far and sets `error` if given one - a half-drawn
// base map is better than a blank page, and a tile is advisory decoration
// rather than data anybody relies on.
std::vector<Layer> decode(const std::vector<uint8_t> &data,
                           const std::vector<std::string> &wantedLayers,
                           const std::vector<std::string> &wantedKeys,
                           std::string *error = nullptr);

} // namespace Mvt
