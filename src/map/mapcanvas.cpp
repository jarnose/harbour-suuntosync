#include "mapcanvas.h"

#include "maptilesource.h"

#include <QFont>
#include <QFontMetricsF>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QStandardPaths>
#include <QUrl>
#include <QtMath>

#include <algorithm>

namespace {

// At most this many tiles each way. Nine tiles is the ceiling, and it is a
// bandwidth decision rather than a drawing one: a measured OpenFreeMap tile is
// 160 kB compressed, so a 3x3 view is about one and a half megabytes the first
// time an area is seen. Allowing more would buy detail nobody asked for at a
// cost somebody pays for.
const int kMaxTilesPerSide = 3;

// Room around the track so it does not touch the edges, as a fraction of its
// own longer side.
const double kPadding = 0.08;

// Only what a base map under a track needs. Everything left out is more than
// half a real tile: `poi` alone was 137 kB of a measured 262 kB, and
// `housenumber` and `building` add nothing at the zooms a workout is seen at.
const char *const kLayers[] = {
    "landcover", "landuse", "park", "water", "waterway", "transportation",
};

// `class` is what decides a road's width and colour; nothing else is read.
const char *const kKeys[] = { "class" };

struct Palette
{
    QColor background;
    QColor land;
    QColor park;
    QColor water;
    QColor majorRoad;
    QColor minorRoad;
    QColor path;
    QColor rail;
    QColor attribution;
};

// Map colours, not theme colours. Two sets so a map does not glare out of a
// dark page, both kept dull on purpose: the track is the subject and the map
// is underneath it.
Palette palette(bool dark)
{
    Palette p;
    if (dark) {
        p.background = QColor(0x1b, 0x1f, 0x23);
        p.land = QColor(0x24, 0x2b, 0x26);
        p.park = QColor(0x25, 0x33, 0x27);
        p.water = QColor(0x24, 0x3b, 0x55);
        p.majorRoad = QColor(0x5c, 0x55, 0x45);
        p.minorRoad = QColor(0x3d, 0x42, 0x47);
        p.path = QColor(0x4a, 0x42, 0x38);
        p.rail = QColor(0x33, 0x37, 0x3b);
        p.attribution = QColor(0x8a, 0x92, 0x9a);
    } else {
        p.background = QColor(0xf4, 0xf1, 0xeb);
        p.land = QColor(0xdc, 0xe6, 0xd2);
        p.park = QColor(0xcd, 0xe3, 0xc4);
        p.water = QColor(0xa8, 0xc9, 0xe8);
        p.majorRoad = QColor(0xf2, 0xd1, 0xa0);
        p.minorRoad = QColor(0xe8, 0xe4, 0xdc);
        p.path = QColor(0xd4, 0xbd, 0x9e);
        p.rail = QColor(0xc4, 0xc0, 0xb8);
        p.attribution = QColor(0x60, 0x60, 0x60);
    }
    return p;
}

// Width in device-independent units, and which colour, per OpenMapTiles road
// class. The class vocabulary was read off a real tile rather than from a
// specification: minor, path, primary, secondary, service, tertiary, pier,
// rail, bridge, ferry, raceway, track all appear in one Monaco tile.
struct RoadStyle
{
    double width;
    const QColor *colour;
};

RoadStyle roadStyle(const std::string &roadClass, const Palette &p)
{
    if (roadClass == "motorway" || roadClass == "trunk" || roadClass == "primary")
        return { 2.6, &p.majorRoad };
    if (roadClass == "secondary" || roadClass == "tertiary")
        return { 2.0, &p.majorRoad };
    if (roadClass == "rail" || roadClass == "transit")
        return { 0.9, &p.rail };
    if (roadClass == "path" || roadClass == "track" || roadClass == "footway"
            || roadClass == "steps" || roadClass == "pier")
        return { 1.1, &p.path };
    // minor, service, bridge, ferry, raceway and anything this build has not
    // seen: drawn thin rather than dropped, since an unknown road is still a
    // road and a missing one leaves a hole where a junction should be.
    return { 1.3, &p.minorRoad };
}

bool isWaterClass(const std::string &value)
{
    // A `water` polygon with class "swimming_pool" at a workout's zoom is
    // noise; lakes, rivers and the sea are the point.
    return value != "swimming_pool";
}

} // namespace

MapCanvas::MapCanvas(QQuickItem *parent)
    : QQuickPaintedItem(parent)
    , m_source(new MapTileSource(this))
    , m_network(new QNetworkAccessManager(this))
{
    setRenderTarget(QQuickPaintedItem::FramebufferObject);
    setAntialiasing(true);

    // Tiles are immutable and the source says so - OpenFreeMap sends a
    // ten-year max-age against a path carrying the planet build's date - so a
    // disk cache means an area is paid for once. Qt's own cache does the
    // expiry arithmetic; this only has to give it somewhere to live.
    QNetworkDiskCache *cache = new QNetworkDiskCache(this);
    cache->setCacheDirectory(
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
            + QStringLiteral("/map-tiles"));
    cache->setMaximumCacheSize(64LL * 1024 * 1024);
    m_network->setCache(cache);
}

void MapCanvas::setTrack(const QVariantList &track)
{
    if (track == m_track)
        return;
    m_track = track;
    reproject();
    m_haveRange = false;
    refreshTiles();
    emit trackChanged();
    update();
}

void MapCanvas::setTileUrl(const QString &url)
{
    if (url == m_tileUrl)
        return;
    m_tileUrl = url;
    // A changed source invalidates everything drawn from the old one.
    m_resolvedTemplate.clear();
    m_tiles.clear();
    m_haveRange = false;
    setError(QString());
    refreshTiles();
    emit tileUrlChanged();
    update();
}

void MapCanvas::setDarkMode(bool dark)
{
    if (dark == m_darkMode)
        return;
    m_darkMode = dark;
    emit darkModeChanged();
    update();
}

void MapCanvas::setRouteColor(const QColor &color)
{
    if (color == m_routeColor)
        return;
    m_routeColor = color;
    emit routeColorChanged();
    update();
}

void MapCanvas::geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChanged(newGeometry, oldGeometry);
    // The view depends on the item's aspect ratio, so a resize can change
    // which tiles are needed - refreshTiles() returns early when it does not.
    refreshTiles();
    update();
}

void MapCanvas::setError(const QString &error)
{
    if (error == m_error)
        return;
    m_error = error;
    emit statusChanged();
}

void MapCanvas::reproject()
{
    m_projected.clear();
    m_projected.reserve(size_t(m_track.size()));
    for (const QVariant &value : m_track) {
        const QVariantMap point = value.toMap();
        m_projected.push_back(Mercator::fromLatLon(
                point.value(QStringLiteral("latitude")).toDouble(),
                point.value(QStringLiteral("longitude")).toDouble()));
    }
    if (m_projected.empty())
        return;

    m_minX = m_maxX = m_projected.front().x;
    m_minY = m_maxY = m_projected.front().y;
    for (const Mercator::Global &g : m_projected) {
        m_minX = std::min(m_minX, g.x);
        m_maxX = std::max(m_maxX, g.x);
        m_minY = std::min(m_minY, g.y);
        m_maxY = std::max(m_maxY, g.y);
    }
}

void MapCanvas::refreshTiles()
{
    if (m_projected.size() < 2 || width() <= 0 || height() <= 0)
        return;

    // The view: the track's box, padded, then widened on whichever axis is
    // short of the item's aspect ratio. Widening rather than cropping means
    // the whole track is always visible.
    const double spanX = m_maxX - m_minX;
    const double spanY = m_maxY - m_minY;
    const double pad = std::max(std::max(spanX, spanY) * kPadding, 1e-9);
    const double boxW = spanX + 2 * pad;
    const double boxH = spanY + 2 * pad;
    const double aspect = width() / height();

    double viewW = boxW;
    double viewH = boxH;
    if (boxW / boxH < aspect)
        viewW = boxH * aspect;
    else
        viewH = boxW / aspect;

    const double centreX = (m_minX + m_maxX) / 2.0;
    const double centreY = (m_minY + m_maxY) / 2.0;
    m_viewX = centreX - viewW / 2.0;
    m_viewY = centreY - viewH / 2.0;
    m_viewW = viewW;
    m_viewH = viewH;

    if (m_tileUrl.trimmed().isEmpty())
        return;

    // Resolve the source first; this calls straight back when it is cached,
    // which it is after the first workout.
    if (m_resolvedTemplate.isEmpty()) {
        m_source->resolve(m_tileUrl, [this](bool ok, const MapTileSource::Resolved &resolved,
                                             const QString &error) {
            if (!ok) {
                setError(error);
                return;
            }
            if (resolved.raster) {
                setError(tr("That address serves image tiles, which this version cannot "
                             "draw yet. A vector source such as the default works."));
                return;
            }
            m_resolvedTemplate = resolved.tileTemplate;
            m_attribution = resolved.attribution;
            m_maxZoom = resolved.maxZoom;
            setError(QString());
            m_haveRange = false;
            refreshTiles();
            update();
        });
        return;
    }

    const Mercator::TileRange range = Mercator::chooseTilesGlobal(
            m_viewX, m_viewY, m_viewX + m_viewW, m_viewY + m_viewH, m_maxZoom, kMaxTilesPerSide);
    if (m_haveRange && range.zoom == m_range.zoom && range.x0 == m_range.x0
            && range.y0 == m_range.y0 && range.x1 == m_range.x1 && range.y1 == m_range.y1) {
        return;
    }
    m_range = range;
    m_haveRange = true;

    for (int x = range.x0; x <= range.x1; ++x) {
        for (int y = range.y0; y <= range.y1; ++y)
            fetchTile(range.zoom, x, y);
    }
}

void MapCanvas::fetchTile(int zoom, int x, int y)
{
    const QString id = QStringLiteral("%1/%2/%3").arg(zoom).arg(x).arg(y);
    if (m_tiles.contains(id))
        return;
    // Reserved immediately, so a resize mid-flight does not ask twice.
    m_tiles.insert(id, Tile{ zoom, x, y, {} });

    QNetworkRequest request((QUrl(MapTileSource::tileUrl(m_resolvedTemplate, zoom, x, y))));
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                          QNetworkRequest::PreferCache);
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);

    ++m_outstanding;
    emit statusChanged();

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, zoom, x, y]() {
        reply->deleteLater();
        --m_outstanding;

        if (reply->error() != QNetworkReply::NoError) {
            // One failed tile is a hole in the map, not a failure of the
            // page - a dropped connection mid-workout-list is ordinary. Only
            // say something when nothing arrived at all.
            m_tiles.remove(id);
            if (m_tiles.isEmpty())
                setError(reply->errorString());
            emit statusChanged();
            update();
            return;
        }

        const QByteArray body = reply->readAll();
        const std::vector<uint8_t> bytes(body.begin(), body.end());
        std::vector<std::string> layers;
        for (const char *name : kLayers)
            layers.push_back(name);
        std::vector<std::string> keys;
        for (const char *name : kKeys)
            keys.push_back(name);

        std::string decodeError;
        Tile tile{ zoom, x, y, Mvt::decode(bytes, layers, keys, &decodeError) };
        // A half-decoded tile is kept: the decoder returns the layers it got,
        // and a map missing its roads still places the track on its water.
        m_tiles.insert(id, tile);
        emit statusChanged();
        update();
    });
}

void MapCanvas::paint(QPainter *painter)
{
    const Palette p = palette(m_darkMode);
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->fillRect(boundingRect(), p.background);

    if (m_projected.size() < 2)
        return;
    if (m_viewW <= 0 || m_viewH <= 0)
        return;

    const double scaleX = width() / m_viewW;
    const double scaleY = height() / m_viewH;
    auto toItem = [this, scaleX, scaleY](double gx, double gy) {
        return QPointF((gx - m_viewX) * scaleX, (gy - m_viewY) * scaleY);
    };

    // ---- the map, if any of it has arrived ----
    for (const Tile &tile : m_tiles) {
        if (tile.layers.empty() || tile.zoom != m_range.zoom)
            continue;
        const double tileScale = double(qint64(1) << tile.zoom);
        const double tileOriginX = double(tile.x) / tileScale;
        const double tileOriginY = double(tile.y) / tileScale;

        for (const Mvt::Layer &layer : tile.layers) {
            const double extent = layer.extent > 0 ? double(layer.extent) : 4096.0;
            auto project = [&](const Mvt::Point &local) {
                return toItem(tileOriginX + local.x / extent / tileScale,
                               tileOriginY + local.y / extent / tileScale);
            };

            const bool isWater = layer.name == "water" || layer.name == "waterway";
            const bool isPark = layer.name == "park";
            const bool isRoad = layer.name == "transportation";

            for (const Mvt::Feature &feature : layer.features) {
                const std::string featureClass = feature.attribute("class");
                if (isWater && !isWaterClass(featureClass))
                    continue;

                if (feature.type == Mvt::GeomType::Polygon) {
                    QPainterPath path;
                    for (const auto &ring : feature.parts) {
                        if (ring.size() < 3)
                            continue;
                        path.moveTo(project(ring.front()));
                        for (size_t i = 1; i < ring.size(); ++i)
                            path.lineTo(project(ring[i]));
                        path.closeSubpath();
                    }
                    if (path.isEmpty())
                        continue;
                    // OddEven, so a polygon's later rings cut holes in it -
                    // which is how the format expresses a lake with an
                    // island in it.
                    path.setFillRule(Qt::OddEvenFill);
                    painter->setPen(Qt::NoPen);
                    painter->setBrush(isWater ? p.water : (isPark ? p.park : p.land));
                    painter->drawPath(path);
                    continue;
                }

                if (feature.type != Mvt::GeomType::LineString)
                    continue;

                QPen pen;
                if (isRoad) {
                    const RoadStyle style = roadStyle(featureClass, p);
                    pen.setColor(*style.colour);
                    pen.setWidthF(style.width);
                } else if (isWater) {
                    pen.setColor(p.water);
                    pen.setWidthF(1.4);
                } else {
                    continue;
                }
                pen.setCapStyle(Qt::RoundCap);
                pen.setJoinStyle(Qt::RoundJoin);
                painter->setPen(pen);
                painter->setBrush(Qt::NoBrush);

                for (const auto &line : feature.parts) {
                    if (line.size() < 2)
                        continue;
                    QPainterPath path;
                    path.moveTo(project(line.front()));
                    for (size_t i = 1; i < line.size(); ++i)
                        path.lineTo(project(line[i]));
                    painter->drawPath(path);
                }
            }
        }
    }

    // ---- the track ----
    QPainterPath route;
    route.moveTo(toItem(m_projected.front().x, m_projected.front().y));
    for (size_t i = 1; i < m_projected.size(); ++i)
        route.lineTo(toItem(m_projected[i].x, m_projected[i].y));

    // Drawn twice: a dark casing under the line, so it stays readable over
    // water and over a road of a similar colour.
    QPen casing(QColor(0, 0, 0, 110));
    casing.setWidthF(5.5);
    casing.setCapStyle(Qt::RoundCap);
    casing.setJoinStyle(Qt::RoundJoin);
    painter->setPen(casing);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(route);

    QPen line(m_routeColor);
    line.setWidthF(3.0);
    line.setCapStyle(Qt::RoundCap);
    line.setJoinStyle(Qt::RoundJoin);
    painter->setPen(line);
    painter->drawPath(route);

    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(0xff, 0xff, 0xff, 220));
    painter->drawEllipse(toItem(m_projected.front().x, m_projected.front().y), 4.0, 4.0);
    painter->setBrush(m_routeColor);
    painter->drawEllipse(toItem(m_projected.back().x, m_projected.back().y), 4.0, 4.0);

    // ---- attribution ----
    // Required by the tile source's terms for a client that is not MapLibre,
    // which this is. Drawn whenever a map was, which is why it is gated on a
    // tile having arrived rather than on the setting being on.
    if (m_attribution.isEmpty() || m_tiles.isEmpty())
        return;
    bool drewSomething = false;
    for (const Tile &tile : m_tiles) {
        if (!tile.layers.empty())
            drewSomething = true;
    }
    if (!drewSomething)
        return;

    QFont font = painter->font();
    font.setPixelSize(10);
    painter->setFont(font);
    const QFontMetricsF metrics(font);
    const QRectF text = metrics.boundingRect(m_attribution);
    const QRectF box(width() - text.width() - 8, height() - text.height() - 4,
                      text.width() + 6, text.height() + 2);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(m_darkMode ? 0 : 255, m_darkMode ? 0 : 255, m_darkMode ? 0 : 255, 120));
    painter->drawRect(box);
    painter->setPen(p.attribution);
    painter->drawText(box, Qt::AlignCenter, m_attribution);
}
