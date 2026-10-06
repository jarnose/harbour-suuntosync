#pragma once

#include "mercator.h"
#include "vectortile.h"

#include <QColor>
#include <QHash>
#include <QQuickPaintedItem>
#include <QVariantList>

#include <vector>

class MapTileSource;
class QNetworkAccessManager;

// Draws a base map under a workout's track, from vector tiles.
//
// A C++ item rather than more QML for one measured reason: the layers worth
// drawing come to roughly four thousand features and sixty thousand points
// across a four-tile view, and handing that through QVariant to a QML Canvas
// would mean a QVariantMap per point. Decoding, projecting and painting all
// happen here, and QML places the item and tells it the colours.
//
// What it draws, bottom to top: a background, land cover and land use, water,
// roads by class, then the track, then the attribution the tile source's
// terms require. What it does not do is pan or zoom: the view is the
// workout's own bounding box, which is the whole question this answers.
//
// See docs/base-map.md for why the tiles are vector, which is the thing that
// makes this file necessary.
class MapCanvas : public QQuickPaintedItem
{
    Q_OBJECT

    // One { latitude, longitude } per point, in real degrees -
    // AppController::workoutTrack(). Fewer than two points draws nothing.
    Q_PROPERTY(QVariantList track READ track WRITE setTrack NOTIFY trackChanged)
    // Either a {z}/{x}/{y} template or a TileJSON endpoint; MapTileSource
    // decides. Empty draws the track alone, with no map and no requests.
    Q_PROPERTY(QString tileUrl READ tileUrl WRITE setTileUrl NOTIFY tileUrlChanged)
    // The map's own palette follows the theme rather than being themed by it:
    // a map wants map colours, and a dozen colour properties would be worse
    // than one switch.
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    Q_PROPERTY(QColor routeColor READ routeColor WRITE setRouteColor NOTIFY routeColorChanged)
    // True while any tile of the current view is still outstanding.
    Q_PROPERTY(bool loading READ loading NOTIFY statusChanged)
    // Set when the map could not be drawn and the user should be told why -
    // a raster source, a dead address, a refused request. The track is still
    // drawn; this is about the map underneath it.
    Q_PROPERTY(QString error READ error NOTIFY statusChanged)

public:
    explicit MapCanvas(QQuickItem *parent = nullptr);

    QVariantList track() const { return m_track; }
    void setTrack(const QVariantList &track);
    QString tileUrl() const { return m_tileUrl; }
    void setTileUrl(const QString &url);
    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool dark);
    QColor routeColor() const { return m_routeColor; }
    void setRouteColor(const QColor &color);
    bool loading() const { return m_outstanding > 0; }
    QString error() const { return m_error; }

    void paint(QPainter *painter) override;

signals:
    void trackChanged();
    void tileUrlChanged();
    void darkModeChanged();
    void routeColorChanged();
    void statusChanged();

protected:
    void geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    struct Tile
    {
        int zoom = 0;
        int x = 0;
        int y = 0;
        std::vector<Mvt::Layer> layers;
    };

    // The projected track, cached so a repaint does not reproject.
    void reproject();
    // Works out the view and the tiles it needs, and starts fetching any that
    // are missing. Cheap to call repeatedly: it returns early unless the tile
    // range actually changed.
    void refreshTiles();
    void fetchTile(int zoom, int x, int y);
    void setError(const QString &error);

    QVariantList m_track;
    QString m_tileUrl;
    bool m_darkMode = true;
    QColor m_routeColor = QColor(0xf0, 0x80, 0x20);

    std::vector<Mercator::Global> m_projected;
    double m_minX = 0, m_minY = 0, m_maxX = 0, m_maxY = 0;

    // The view, in global Mercator coordinates: the track's box padded and
    // then widened to the item's own aspect ratio, so the track fills the
    // item and the tiles under it are cropped rather than letterboxed.
    double m_viewX = 0, m_viewY = 0, m_viewW = 0, m_viewH = 0;

    Mercator::TileRange m_range;
    bool m_haveRange = false;
    QHash<QString, Tile> m_tiles;
    int m_outstanding = 0;

    MapTileSource *m_source;
    QNetworkAccessManager *m_network;
    QString m_resolvedTemplate;
    QString m_attribution;
    int m_maxZoom = 14;
    QString m_error;
};
