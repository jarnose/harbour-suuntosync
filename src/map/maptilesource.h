#pragma once

#include <QHash>
#include <QObject>
#include <QString>

#include <functional>

class QNetworkAccessManager;

// Turns whatever a user put in the tile-URL setting into something a tile can
// actually be fetched from.
//
// Two forms are accepted, and which one it is is decided by looking for
// "{z}" rather than by a second setting:
//
//   a template      https://example.org/tiles/{z}/{x}/{y}.pbf
//   a TileJSON URL  https://tiles.openfreemap.org/planet
//
// The TileJSON form exists because the default needs it: OpenFreeMap's own
// tile path carries the date of the planet build it was generated from
// (".../planet/20260927_080001_pt/{z}/{x}/{y}.pbf"), so a template written
// down today stops being current. Reading tiles[0] out of the TileJSON gets
// the right one, and gets the source's real maximum zoom and attribution
// along with it.
//
// Resolution is cached per URL for the lifetime of this object: it is one
// request that answers for every tile and every workout.
class MapTileSource : public QObject
{
    Q_OBJECT

public:
    explicit MapTileSource(QObject *parent = nullptr);

    struct Resolved
    {
        QString tileTemplate;
        int maxZoom = 14;
        // Plain text, with any markup stripped - TileJSON carries it as HTML
        // and this gets painted onto a canvas. Falls back to OpenFreeMap's
        // required wording when the source states none, because a source
        // that states none still has to be credited.
        QString attribution;
        // Set when the template points at images rather than vector tiles.
        // Raster is not drawn yet, and a URL that would quietly produce a
        // blank map should say so instead - see docs/base-map.md.
        bool raster = false;
    };

    using Callback = std::function<void(bool ok, const Resolved &resolved, const QString &error)>;

    // Calls back exactly once, on this object's thread. A cached answer still
    // arrives through the callback rather than by return, so the caller has
    // one code path.
    void resolve(const QString &url, Callback callback);

    // Substitutes a tile's coordinates into a resolved template. Handles the
    // {z}/{x}/{y} placeholders and nothing else; a source needing a
    // subdomain rotation or a key parameter is out of scope.
    static QString tileUrl(const QString &tileTemplate, int zoom, int x, int y);

    // Whether a template looks like it serves images. Checked on the
    // template rather than on a response, so the first tile is never
    // requested at all when the answer is already visible in the URL.
    static bool looksRaster(const QString &tileTemplate);

private:
    QNetworkAccessManager *m_network;
    QHash<QString, Resolved> m_cache;
};
