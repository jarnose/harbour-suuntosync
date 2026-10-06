#include "maptilesource.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

namespace {

// What the terms require of a client that is not MapLibre, which this is.
// Used when a source states no attribution of its own; a source that states
// one has its own used instead.
const QString kFallbackAttribution =
        QStringLiteral("OpenFreeMap © OpenMapTiles Data from OpenStreetMap");

// TileJSON's attribution is HTML - links and all - and this ends up painted
// on a canvas, so the tags come off and the entities most likely to appear
// are turned back into characters.
QString plainText(const QString &html)
{
    QString out = html;
    out.replace(QRegularExpression(QStringLiteral("<[^>]*>")), QString());
    out.replace(QStringLiteral("&copy;"), QStringLiteral("©"));
    out.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    out.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    out.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return out.trimmed();
}

} // namespace

MapTileSource::MapTileSource(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
}

bool MapTileSource::looksRaster(const QString &tileTemplate)
{
    const QString path = QUrl(tileTemplate).path().toLower();
    return path.endsWith(QStringLiteral(".png")) || path.endsWith(QStringLiteral(".jpg"))
            || path.endsWith(QStringLiteral(".jpeg")) || path.endsWith(QStringLiteral(".webp"));
}

QString MapTileSource::tileUrl(const QString &tileTemplate, int zoom, int x, int y)
{
    QString url = tileTemplate;
    url.replace(QStringLiteral("{z}"), QString::number(zoom));
    url.replace(QStringLiteral("{x}"), QString::number(x));
    url.replace(QStringLiteral("{y}"), QString::number(y));
    return url;
}

void MapTileSource::resolve(const QString &url, Callback callback)
{
    const QString trimmed = url.trimmed();
    if (trimmed.isEmpty()) {
        callback(false, Resolved(), tr("No map tile address set"));
        return;
    }

    if (m_cache.contains(trimmed)) {
        callback(true, m_cache.value(trimmed), QString());
        return;
    }

    // Already a template: nothing to look up, and deliberately no request to
    // check it with. The first tile will say whether it works.
    if (trimmed.contains(QStringLiteral("{z}"))) {
        Resolved resolved;
        resolved.tileTemplate = trimmed;
        resolved.attribution = kFallbackAttribution;
        resolved.raster = looksRaster(trimmed);
        m_cache.insert(trimmed, resolved);
        callback(true, resolved, QString());
        return;
    }

    QNetworkRequest request((QUrl(trimmed)));
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                          QNetworkRequest::PreferCache);
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, trimmed, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, Resolved(), reply->errorString());
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            callback(false, Resolved(),
                     tr("That address did not answer with TileJSON. Give a {z}/{x}/{y} "
                         "template instead."));
            return;
        }
        const QJsonObject object = doc.object();
        const QJsonArray tiles = object.value(QStringLiteral("tiles")).toArray();
        if (tiles.isEmpty()) {
            callback(false, Resolved(), tr("The map source listed no tiles"));
            return;
        }

        Resolved resolved;
        resolved.tileTemplate = tiles.first().toString();
        // maxzoom is advisory in TileJSON but every source states it, and
        // asking for a zoom a source does not have is a 404 per tile.
        if (object.contains(QStringLiteral("maxzoom")))
            resolved.maxZoom = object.value(QStringLiteral("maxzoom")).toInt(14);
        const QString attribution =
                plainText(object.value(QStringLiteral("attribution")).toString());
        resolved.attribution = attribution.isEmpty() ? kFallbackAttribution : attribution;
        resolved.raster = looksRaster(resolved.tileTemplate);

        if (resolved.tileTemplate.isEmpty()) {
            callback(false, Resolved(), tr("The map source listed no tiles"));
            return;
        }
        m_cache.insert(trimmed, resolved);
        callback(true, resolved, QString());
    });
}
