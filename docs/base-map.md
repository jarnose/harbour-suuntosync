# A base map under the GPS track

The workout page draws a route as a bare polyline. Putting a map under it on
Sailfish OS is harder than it sounds, and the reason is worth stating first
because it shaped every decision here.

## There is no `Map` element

Sailfish ships `qt5-qtpositioning` but **not** QtLocation's QML module, and no
geoservices plugins: `qt5-qtlocation` in the Jolla repository contains only
`libQt5Location.so*`. So there is no `Map`, no `MapPolyline`, no tile cache
and no provider plugin. Tiles have to be fetched and drawn by hand.

That turns the question into "which tile source", and that is a licensing
question rather than a technical one.

## Why OpenFreeMap

| source | key | can it be the default in a public repo |
|---|---|---|
| **OpenFreeMap** | none | **yes** |
| Maanmittauslaitos (Finland, CC BY) | free, per user | no |
| MapTiler / Thunderforest / Stadia | free tier, per user | no |
| Mapbox - what the official Suunto app uses | commercial | no |
| `tile.openstreetmap.org` | none | no: the usage policy does not welcome app use |

[OpenFreeMap](https://openfreemap.org/) is the only one that can ship as a
default: no API key, no registration, no request limit, commercial use
explicitly allowed, MIT. Attribution is required, and the terms are specific
that a client which is not MapLibre must add it itself:

```
OpenFreeMap © OpenMapTiles Data from OpenStreetMap
```

So that string gets drawn on the map. Its honest weakness is that it is one
person's donation-funded servers with no SLA - which is also why the tile URL
stays a setting rather than a constant, and why self-hosting exists.

**Not an option, to be clear about it:** the capture of the official Suunto
app contains Suunto's own Mapbox token. Using it would be spending their quota.

## What OpenFreeMap actually serves

Two sources in its style, and only one of them is useful here:

- `ne2_shaded` - **raster PNG**, `maxzoom 6`. Natural Earth relief at
  continent scale. Useless for a five-kilometre run.
- `openmaptiles` - **vector**, `maxzoom 14`, TileJSON at
  `https://tiles.openfreemap.org/planet`.

So the simple implementation - an `Image` grid - is not available, and the
tiles have to be decoded. That is the whole cost of the licensing win.

Two details that a hardcoded URL would get wrong:

1. The tile template carries a **dated path**:
   `https://tiles.openfreemap.org/planet/20260927_080001_pt/{z}/{x}/{y}.pbf`.
   It changes when the planet is rebuilt, so the TileJSON must be fetched and
   `tiles[0]` read from it rather than the template being assumed.
2. Responses are `application/vnd.mapbox-vector-tile`, uncompressed unless
   `Accept-Encoding` asks - 262866 bytes becomes 162691 when it does, which
   `QNetworkAccessManager` requests by default. `Cache-Control` is ten years
   and the path is versioned, so a disk cache is worth having.

### What is in a tile

Measured, not estimated - one tile over Tampere and one over Monaco, both z14:

| layer | Tampere features | bytes | Monaco features |
|---|---|---|---|
| poi | 2722 | 137565 | 2050 |
| building | 150 | 40206 | 43 |
| transportation | 613 | 39692 | 705 |
| landcover | 348 | 16070 | 129 |
| transportation_name | 120 | 9285 | 365 |
| landuse | 70 | 5438 | 65 |
| housenumber | 108 | 4697 | 108 |
| place | 30 | 3883 | 16 |
| waterway | 17 | 2073 | - |
| water | 9 | 913 | 99 |

**Over half of a tile is `poi`**, and a base map under a GPS trace needs none
of it - nor `housenumber`, nor `building`. Naming the layers wanted skips
their geometry entirely, which leaves roughly a thousand features per tile to
draw: mostly short polylines, well within what a `Canvas` handles.

## The decoder

`src/map/vectortile.h`/`.cpp`, Qt-free and STL-only like every other decoder
here, tested by `tests/test_vectortile.cpp`.

The wire format is protocol buffers. The field numbers are from the published
`vector_tile.proto` v2, each confirmed against a real tile rather than taken
on trust. The parts that are easy to get subtly wrong, and what the test pins
down about each:

- **Zigzag deltas.** Coordinates are signed deltas with the sign in the low
  bit, accumulated from a running cursor.
- **Command packing.** One integer carries a command id in its low three bits
  and a repeat count in the rest: `MoveTo` 1, `LineTo` 2, `ClosePath` 7. An
  unknown command cannot be skipped, because its parameter count is
  unknowable, so decoding stops rather than desynchronising.
- **The cursor is not reset between parts.** A second `MoveTo` is a delta from
  where the previous part *ended*, not from the origin. This one has its own
  assertion because getting it wrong produces a plausible-looking map with
  every feature after the first in the wrong place.
- **`ClosePath` is made explicit.** The first point is repeated at the end of
  a ring, so no caller has to know it was implied.
- **A multipoint is one `MoveTo` with a count above one.** Those points stay
  in a single part, so `parts.size()` counts geometries rather than points.
- **Two passes per layer.** A feature's tags index into the layer's key and
  value tables, and the format does not promise those come first.
- **`extent` defaults to 4096**, and geometry may fall *outside* 0..extent:
  the format allows a buffer so a road crossing an edge joins up cleanly. The
  Monaco tile's furthest coordinate is 4160.

Malformed input is a first-class case rather than an afterthought, because a
cancelled download produces it: every read is bounds-checked, an unterminated
varint is rejected instead of looped on, and a truncated tile returns the
layers that were complete plus an error. The test decodes **every single-byte
prefix** of a real tile to prove none of them reads past the end. A base map
is decoration; half of one beats a blank page, and neither is worth a crash.

### What the test checks against

A synthetic tile built byte by byte with a known answer, **inlined as hex so
the suite needs no fixture and runs in CI**, plus the real Monaco tile for
scale. Monaco deliberately: OpenFreeMap uses it as its own sample area, so
the fixture discloses nothing about where anybody lives.

The real tile's feature counts were read out of the same bytes by a throwaway
Python scanner *before this decoder existed*, which is what makes them a
golden vector rather than this code agreeing with itself. 15857 points decode,
and the `transportation` classes come out as the OpenMapTiles vocabulary -
`minor`, `path`, `primary`, `secondary`, `service`, `track`, `rail`, `pier`,
`ferry` - which is a stronger signal than any count: a decoder that was
subtly wrong would not produce a plausible histogram of real road types.

## How it is put together

- **`src/map/mercator.{h,cpp}`** - Web Mercator and the tile arithmetic,
  Qt-free. `chooseTiles` picks the largest zoom whose grid covers a box in at
  most three tiles a side, by *trying* each zoom rather than dividing the
  span: where a box falls decides how many tile boundaries it crosses, so the
  same span needs one tile or two. Three a side is a bandwidth ceiling, not a
  drawing one - nine tiles is about a megabyte and a half the first time an
  area is seen.
- **`src/map/vectortile.{h,cpp}`** - the MVT decoder above.
- **`src/map/maptilesource.{h,cpp}`** - turns the setting into a tile
  template, fetching TileJSON when the setting is not already one.
- **`src/map/mapcanvas.{h,cpp}`** - a `QQuickPaintedItem` that fetches,
  decodes and paints. In C++ rather than QML for a measured reason: the layers
  worth drawing come to roughly four thousand features and sixty thousand
  points across a four-tile view, and a QML `Canvas` would need a
  `QVariantMap` per point to get there.
- **The route's own projection changed with it.**
  `AppController::workoutRoute` fitted its bounding box with a
  `cos(latitude)` correction, which is equirectangular. That is invisible at
  five kilometres and still wrong once there are roads underneath, so both the
  map and the plain polyline now use Mercator - and the y inversion the old
  code needed went away, because Mercator's y already increases southwards
  like a canvas's.

The view is the track's own bounding box, padded and then **widened** to the
item's aspect ratio rather than cropped, so the whole outing is always
visible and the tiles under it are the things that get cut off.

Drawn bottom to top: background, land cover and land use, water, roads by
class, the track with a dark casing under it so it stays readable over water,
then the attribution. There is no panning or zooming: the view is the
workout, which is the question this answers.

## Confirmed on hardware, 2026-10-06

The map draws under the track, the attribution shows, both the light and the
dark palette were looked at, and the zoom the three-tile ceiling picks was
judged about right for a real outing. Turning the switch off brings back the
bare polyline, now Mercator-projected.

**Handing a workout to Pure Maps works, and needed no permission.** The
concern was that `Qt.openUrlExternally` would need
`dbus-user.talk org.sailfishos.maps`, which only `Contacts.permission` grants
and which would be absurd to request for a map link. What actually happens is
that the system asks the user to confirm launching the other application, and
Pure Maps opens on the area. It shows the place, not the track - which is what
`geo:lat,lon` can express and all it can.

## Still open

- **Raster tiles.** A URL that serves images is detected and reported rather
  than drawn, which is the honest half of supporting one source properly.
  Adding it is the easier of the two renderers.
- **Labels.** None are drawn. The `place` layer carries names as attributes,
  so Qt's own fonts could draw them without the glyph endpoint's SDF fonts -
  but a name every few hundred metres under a GPS trace may be clutter rather
  than information, which is a judgement better made while looking at it.

Two decisions taken rather than left to the user:

- **The field accepts either form.** If the string contains `{z}` it is a tile
  template; otherwise it is fetched as TileJSON and `tiles[0]` read from it.
  One rule, no second setting, and OpenFreeMap's dated path handled for free.
- **Vector first; a raster URL is reported, not silently blank.** OpenFreeMap
  is vector and Maanmittauslaitos is raster, and those are two renderers. The
  default is the one being built; a URL that answers with an image says so in
  an error rather than drawing nothing. Raster is the easier of the two to add
  afterwards.

The switch is deliberately **not** being added before the map draws. This
project has already learned once that a settings switch which does nothing
looks exactly like an unbuilt feature.
