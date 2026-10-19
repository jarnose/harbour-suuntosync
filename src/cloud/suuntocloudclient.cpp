#include "suuntocloudclient.h"
#include "suuntoauth.h"
#include "iso8601.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QStringList>
#include <QHash>
#include <QSet>
#include <QUrl>
#include <QByteArray>

#include <random>

namespace {

const QString kBaseUrl = QStringLiteral("https://api.sports-tracker.com/apiserver/v1/");
// One endpoint here lives under v2 rather than v1 - the personal records.
// Its own v1 was not observed and is not assumed to exist.
const QString kBaseUrlV2 = QStringLiteral("https://api.sports-tracker.com/apiserver/v2/");
// Matches auth.UserAgent in tajchert/suuntool (PackageName + "/" + AppVersionCode)
// - the same constants SuuntoAuth::deriveLoginSecret() uses, so this is kept
// in lockstep with the APK version those constants were extracted from.
// The round-the-clock timeline lives on its own host, with its own
// conventions - see fetchHealthEntries().
const QString kHealthBaseUrl = QStringLiteral("https://247.sports-tracker.com/v1/");
const QString kUserAgent = QStringLiteral("com.stt.android.suunto/6008013");

// Percent-encodes for application/x-www-form-urlencoded, then swaps %20 for
// '+' - matches Go's net/url.Values.Encode() exactly (Qt's own percent-
// encoding leaves spaces as %20, which most servers accept but isn't
// byte-identical to what the real app sends).
QByteArray formUrlEncode(const QString &value)
{
    QByteArray encoded = QUrl::toPercentEncoding(value);
    encoded.replace("%20", "+");
    return encoded;
}

// The ASKO envelope's error, as a sentence, or an empty string when the
// reply carries none. The code arrives as a string on some endpoints and a
// number on others, which cost an afternoon once already.
QString envelopeError(const QJsonObject &envelope)
{
    const QJsonValue error = envelope.value(QStringLiteral("error"));
    if (error.isNull() || error.isUndefined())
        return QString();
    const QJsonObject object = error.toObject();
    const QString code = object.value(QStringLiteral("code")).toString(
            QString::number(object.value(QStringLiteral("code")).toInt()));
    return QStringLiteral("%1: %2")
            .arg(code, object.value(QStringLiteral("description")).toString());
}

} // namespace

SuuntoCloudClient::SuuntoCloudClient(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
}

QNetworkRequest SuuntoCloudClient::authorizedRequest(const QString &url,
                                                       const QString &sessionKey) const
{
    QNetworkRequest request((QUrl(url)));
    request.setRawHeader("STTAuthorization", sessionKey.toUtf8());
    request.setRawHeader("User-Agent", kUserAgent.toUtf8());
    request.setRawHeader("Accept-Language", "en");
    return request;
}

namespace {

// One endpoint per assist-data format, in the order the watch's own
// Format enum lists them: SGEE, EPO, CEP, LLE. All five URLs are literals
// in the official app; EPO is the odd one out, arriving in two indexed
// parts, and is left unimplemented because no watch here asks for it.
const char *const kEphemerisUrls[] = {
    "https://devices.suunto-operations.com/devices/gpsorbit/binary",
    nullptr, // EPO: mtk3day, two parts - no watch here reports format 1
    "https://devices.suunto-operations.com/devices/gpsorbit/sony",
    "https://devices.suunto-operations.com/devices/gpsorbit/sonylle",
};

} // namespace

void SuuntoCloudClient::downloadEphemeris(int formatIndex, RawDownloadCallback callback)
{
    const int count = static_cast<int>(sizeof(kEphemerisUrls) / sizeof(kEphemerisUrls[0]));
    if (formatIndex < 0 || formatIndex >= count || kEphemerisUrls[formatIndex] == nullptr) {
        callback(false, QByteArray(),
                  tr("This watch asks for GPS assist format %1, which isn't implemented")
                          .arg(formatIndex));
        return;
    }

    QNetworkRequest request{QUrl(QString::fromLatin1(kEphemerisUrls[formatIndex]))};
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, QByteArray(), reply->errorString());
            return;
        }
        const QByteArray body = reply->readAll();
        if (body.isEmpty()) {
            callback(false, QByteArray(), tr("The assist-data server returned nothing"));
            return;
        }
        callback(true, body, QString());
    });
}

namespace {

// One catalogue or library region, flattened into what a list needs. The
// sizes are summed over the region's `sources`, because that is where the
// bytes are: a region names several and the watch is told which by its own
// capabilities.
QVariantMap mapRegionRow(const QJsonObject &region, const QString &groupName)
{
    QVariantMap row;
    row.insert(QStringLiteral("id"), region.value(QStringLiteral("id")).toString());
    row.insert(QStringLiteral("name"), region.value(QStringLiteral("name")).toString());
    row.insert(QStringLiteral("description"),
                region.value(QStringLiteral("description")).toString());
    row.insert(QStringLiteral("groupName"), groupName);
    row.insert(QStringLiteral("status"), region.value(QStringLiteral("status")).toString());

    double storage = region.value(QStringLiteral("storageSize")).toDouble();
    double transfer = region.value(QStringLiteral("transferSize")).toDouble();
    for (const QJsonValue &value : region.value(QStringLiteral("sources")).toArray()) {
        const QJsonObject source = value.toObject();
        storage += source.value(QStringLiteral("storageSize")).toDouble();
        transfer += source.value(QStringLiteral("transferSize")).toDouble();
    }
    row.insert(QStringLiteral("storageSize"), storage);
    row.insert(QStringLiteral("transferSize"), transfer);
    row.insert(QStringLiteral("downloadedSize"),
                region.value(QStringLiteral("downloadedSize")).toDouble());
    return row;
}

} // namespace

void SuuntoCloudClient::fetchMapCatalogue(const QString &sessionKey,
                                           const QString &capabilities,
                                           MapRegionsCallback callback)
{
    // No lat/lon: both are nullable in the official app and leaving them
    // out asks for the whole catalogue rather than what is nearby, which
    // is what a list to browse wants. `includeNearbyGroups` is pointless
    // without a position, so it is left out too.
    const QString path = QStringLiteral("maps/catalogue?watchCapabilities=%1")
                                 .arg(QString::fromUtf8(QUrl::toPercentEncoding(capabilities)));
    QNetworkReply *reply = m_network->get(authorizedRequest(kBaseUrl + path, sessionKey));
    connect(reply, &QNetworkReply::finished, this, [reply, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, {}, reply->errorString());
            return;
        }
        const QJsonObject envelope = QJsonDocument::fromJson(reply->readAll()).object();
        const QString error = envelopeError(envelope);
        if (!error.isEmpty()) {
            callback(false, {}, error);
            return;
        }
        const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();

        // **A group's `regions` are references, not regions.** They carry
        // an id and nothing else (`RemoteId` in the official app), and the
        // full records live once in `payload.regions`. Reading a group's
        // entry as a region gives a row with an id and no name - which is
        // exactly what the first version of this drew.
        //
        // So the full records are indexed first, and the groups are then
        // walked only to learn which heading each one belongs under.
        QHash<QString, QJsonObject> byId;
        QStringList order;
        for (const QJsonValue &regionValue : payload.value(QStringLiteral("regions")).toArray()) {
            const QJsonObject region = regionValue.toObject();
            const QString id = region.value(QStringLiteral("id")).toString();
            if (id.isEmpty() || byId.contains(id))
                continue;
            byId.insert(id, region);
            order.append(id);
        }

        QHash<QString, QString> groupOf;
        QStringList groupOrder;
        for (const QJsonValue &groupValue : payload.value(QStringLiteral("groups")).toArray()) {
            const QJsonObject group = groupValue.toObject();
            const QString groupName = group.value(QStringLiteral("name")).toString();
            for (const QJsonValue &ref : group.value(QStringLiteral("regions")).toArray()) {
                // A reference is an object with an id; a bare string is
                // accepted too, because one listing of ids as strings
                // would otherwise silently drop a whole group.
                const QString id = ref.isString()
                        ? ref.toString()
                        : ref.toObject().value(QStringLiteral("id")).toString();
                if (id.isEmpty() || groupOf.contains(id))
                    continue;
                groupOf.insert(id, groupName);
                groupOrder.append(id);
            }
        }

        // Grouped regions first, in the cloud's own group order, then
        // anything no group claimed. The page draws a heading wherever the
        // group name changes, so the order is what makes the groups whole.
        QVariantList rows;
        for (const QString &id : groupOrder) {
            if (!byId.contains(id))
                continue;
            rows.append(mapRegionRow(byId.value(id), groupOf.value(id)));
        }
        for (const QString &id : order) {
            if (groupOf.contains(id))
                continue;
            rows.append(mapRegionRow(byId.value(id), QString()));
        }
        callback(true, rows, QString());
    });
}

void SuuntoCloudClient::fetchMapLibrary(const QString &sessionKey, const QString &serial,
                                         const QString &capabilities,
                                         MapRegionsCallback callback)
{
    const QString path = QStringLiteral("maps/library?deviceSerialNumber=%1&watchCapabilities=%2")
                                 .arg(QString::fromUtf8(QUrl::toPercentEncoding(serial)),
                                       QString::fromUtf8(QUrl::toPercentEncoding(capabilities)));
    QNetworkReply *reply = m_network->get(authorizedRequest(kBaseUrl + path, sessionKey));
    connect(reply, &QNetworkReply::finished, this, [reply, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, {}, reply->errorString());
            return;
        }
        const QJsonObject envelope = QJsonDocument::fromJson(reply->readAll()).object();
        const QString error = envelopeError(envelope);
        if (!error.isEmpty()) {
            callback(false, {}, error);
            return;
        }
        QVariantList rows;
        for (const QJsonValue &value : envelope.value(QStringLiteral("payload")).toArray())
            rows.append(mapRegionRow(value.toObject(), QString()));
        callback(true, rows, QString());
    });
}

void SuuntoCloudClient::addMapRegion(const QString &sessionKey, const QString &regionId,
                                      const QString &serial, const QString &capabilities,
                                      SimpleCallback callback)
{
    QJsonObject body;
    body.insert(QStringLiteral("regionId"), regionId);
    body.insert(QStringLiteral("watchCapabilities"), capabilities);
    body.insert(QStringLiteral("deviceSerialNumber"), serial);

    QNetworkRequest request =
            authorizedRequest(kBaseUrl + QStringLiteral("maps/library"), sessionKey);
    // The official app sends this header explicitly on this one request,
    // which is a hint worth copying rather than reasoning about.
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QNetworkReply *reply = m_network->put(request,
                                            QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, callback]() {
        reply->deleteLater();
        const QByteArray answer = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, reply->errorString());
            return;
        }
        const QString error = envelopeError(QJsonDocument::fromJson(answer).object());
        callback(error.isEmpty(), error);
    });
}

void SuuntoCloudClient::removeMapRegion(const QString &sessionKey, const QString &regionId,
                                         const QString &serial, const QString &capabilities,
                                         SimpleCallback callback)
{
    const QString path = QStringLiteral("maps/library/%1?deviceSerialNumber=%2"
                                         "&watchCapabilities=%3")
                                 .arg(QString::fromUtf8(QUrl::toPercentEncoding(regionId)),
                                       QString::fromUtf8(QUrl::toPercentEncoding(serial)),
                                       QString::fromUtf8(QUrl::toPercentEncoding(capabilities)));
    QNetworkReply *reply =
            m_network->deleteResource(authorizedRequest(kBaseUrl + path, sessionKey));
    connect(reply, &QNetworkReply::finished, this, [reply, callback]() {
        reply->deleteLater();
        const QByteArray answer = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, reply->errorString());
            return;
        }
        const QString error = envelopeError(QJsonDocument::fromJson(answer).object());
        callback(error.isEmpty(), error);
    });
}

void SuuntoCloudClient::fetchWatchKey(const QString &sessionKey, WatchKeyCallback callback)
{
    if (m_accountEmail.isEmpty()) {
        callback(false, QString(), tr("No account email to build a TOTP from."));
        return;
    }
    const std::string totp = SuuntoAuth::generateTotp(m_accountEmail.toStdString(),
                                                       QDateTime::currentMSecsSinceEpoch());

    QNetworkRequest request =
            authorizedRequest(kBaseUrl + QStringLiteral("userlogin"), sessionKey);
    request.setRawHeader("x-totp", QByteArray::fromStdString(totp));

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, callback]() {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            // The error body can carry the account object too, so it is
            // described rather than quoted.
            callback(false, QString(),
                      tr("%1 (%2 bytes of answer)")
                              .arg(reply->errorString()).arg(body.size()));
            return;
        }
        // Measured, 2026-10-19: the field is **`watchUserKey`**, sixteen
        // characters, at the top level of a plain user object - no
        // envelope. The APK's own class calls it `watchKey`
        // (`UserLoginResponse(watchKey=)`), so the Kotlin property name and
        // the wire name differ; that cost one round trip. `watchKey` is
        // still accepted in case another account or version answers it.
        const QJsonObject object = QJsonDocument::fromJson(body).object();
        QString key = object.value(QStringLiteral("watchUserKey")).toString();
        if (key.isEmpty())
            key = object.value(QStringLiteral("watchKey")).toString();
        if (key.isEmpty()) {
            key = object.value(QStringLiteral("payload")).toObject()
                          .value(QStringLiteral("watchUserKey")).toString();
        }
        if (key.isEmpty()) {
            // The field names, never the values. This endpoint answers the
            // whole user object - the account's own userKey is in there -
            // and the first version of this put all of it in the journal,
            // which is a credential in a log file. Names are enough to see
            // what the answer was shaped like.
            callback(false, QString(),
                      tr("No watchUserKey in the answer; it carried: %1")
                              .arg(QStringList(object.keys()).join(QStringLiteral(", "))));
            return;
        }
        callback(true, key, QString());
    });
}

void SuuntoCloudClient::login(const QString &email, const QString &password,
                               LoginCallback callback)
{
    const std::string emailStd = email.toStdString();
    const std::string passwordStd = password.toStdString();

    const std::string totp = SuuntoAuth::generateTotp(emailStd, QDateTime::currentMSecsSinceEpoch());
    const std::string signature = SuuntoAuth::signParams(
            "login2", { { "l", emailStd }, { "p", passwordStd }, { "totp", totp } });
    const std::string salt = SuuntoAuth::base64UrlNoPad(
            [] {
                // 16 random bytes, matching auth.RandomSalt(). std::random_device
                // rather than QRandomGenerator (added in Qt 5.10 - newer than
                // Sailfish OS's Qt5) - this salt only needs to be unpredictable-
                // enough/unique-enough per request, same role a nonce plays
                // elsewhere in this scheme, not itself a secret.
                std::vector<uint8_t> bytes(16);
                std::random_device rd;
                std::uniform_int_distribution<int> dist(0, 255);
                for (auto &b : bytes)
                    b = static_cast<uint8_t>(dist(rd));
                return bytes;
            }());

    // Field order here doesn't need to match Go's url.Values.Encode() (which
    // sorts keys alphabetically) - a form-urlencoded body is parsed as an
    // unordered set of pairs by any standard web framework, this API
    // included per suuntool's own working client.
    QByteArray body;
    body += "l=" + formUrlEncode(email);
    body += "&p=" + formUrlEncode(password);
    body += "&totp=" + formUrlEncode(QString::fromStdString(totp));
    body += "&timestamp=" + QByteArray::number(QDateTime::currentMSecsSinceEpoch());
    body += "&salt=" + formUrlEncode(QString::fromStdString(salt));
    body += "&signature=" + formUrlEncode(QString::fromStdString(signature));

    QNetworkRequest request(QUrl(kBaseUrl + QStringLiteral("login2")));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                       QStringLiteral("application/x-www-form-urlencoded;charset=UTF-8"));
    request.setRawHeader("x-login-email-verification-enabled", "true");
    request.setRawHeader("User-Agent", kUserAgent.toUtf8());
    request.setRawHeader("Accept-Language", "en");

    QNetworkReply *reply = m_network->post(request, body);
    connect(reply, &QNetworkReply::finished, this, [reply, callback]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            callback(false, Session(), reply->errorString());
            return;
        }

        const QByteArray responseBody = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(responseBody);
        if (!doc.isObject()) {
            callback(false, Session(), tr("Unexpected response from server"));
            return;
        }

        const QJsonObject obj = doc.object();
        Session session;
        session.sessionKey = obj.value(QStringLiteral("sessionkey")).toString();
        session.username = obj.value(QStringLiteral("username")).toString();
        session.email = obj.value(QStringLiteral("email")).toString();
        session.userKey = obj.value(QStringLiteral("userKey")).toString();
        session.country = obj.value(QStringLiteral("country")).toString();
        session.emailVerified = obj.value(QStringLiteral("emailVerified")).toBool();

        if (!session.isValid()) {
            callback(false, Session(), tr("Login failed: no session key in response"));
            return;
        }
        callback(true, session, QString());
    });
}

void SuuntoCloudClient::fetchWorkoutSml(const QString &sessionKey, const QString &key,
                                          RawBodyCallback callback)
{
    const QNetworkRequest request = authorizedRequest(
            kBaseUrl + QStringLiteral("workouts/") + key + QStringLiteral("/sml"),
            sessionKey);

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, QByteArray(), reply->errorString());
            return;
        }
        callback(true, reply->readAll(), QString());
    });
}

void SuuntoCloudClient::uploadWorkout(const QString &sessionKey, const QByteArray &smlZip,
                                       UploadCallback callback)
{
    // Part names, filenames and content types are copied from the captured
    // request rather than chosen - the server matches on them.
    QHttpMultiPart *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    QHttpPart extensions;
    extensions.setHeader(QNetworkRequest::ContentDispositionHeader,
                          QVariant(QStringLiteral(
                                  "form-data; name=\"workoutExtensions\"; filename=\"extension\"")));
    extensions.setHeader(QNetworkRequest::ContentTypeHeader,
                          QVariant(QStringLiteral("application/json;charset=UTF-8")));
    extensions.setBody(QByteArrayLiteral("[]"));
    multiPart->append(extensions);

    QHttpPart sml;
    sml.setHeader(QNetworkRequest::ContentDispositionHeader,
                   QVariant(QStringLiteral(
                           "form-data; name=\"sml\"; filename=\"sml.zip\"")));
    sml.setHeader(QNetworkRequest::ContentTypeHeader,
                   QVariant(QStringLiteral("application/zip")));
    sml.setBody(smlZip);
    multiPart->append(sml);

    const QNetworkRequest request = authorizedRequest(
            kBaseUrl + QStringLiteral("workout"), sessionKey);

    QNetworkReply *reply = m_network->post(request, multiPart);
    multiPart->setParent(reply); // freed with the reply

    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            // The body often says more than the status line does.
            const QByteArray body = reply->readAll();
            callback(false, QString(),
                     body.isEmpty() ? reply->errorString()
                                     : QStringLiteral("%1: %2").arg(reply->errorString(),
                                                                      QString::fromUtf8(body.left(300))));
            return;
        }

        const QJsonObject envelope = QJsonDocument::fromJson(reply->readAll()).object();
        if (!envelope.value(QStringLiteral("error")).isNull()) {
            const QJsonObject err = envelope.value(QStringLiteral("error")).toObject();
            callback(false, QString(),
                     tr("Server error %1: %2")
                             .arg(err.value(QStringLiteral("code")).toString(
                                      QString::number(err.value(QStringLiteral("code"))
                                                              .toInt())))
                             .arg(err.value(QStringLiteral("description")).toString()));
            return;
        }
        callback(true,
                 envelope.value(QStringLiteral("payload")).toObject()
                         .value(QStringLiteral("key")).toString(),
                 QString());
    });
}

void SuuntoCloudClient::uploadHealthEntries(const QString &sessionKey, const QString &kind,
                                             const QVector<HealthEntry> &entries,
                                             int offsetMinutes, SimpleCallback callback)
{
    if (entries.isEmpty()) {
        callback(true, QString());
        return;
    }

    QJsonArray array;
    for (const HealthEntry &entry : entries) {
        const QJsonObject data = QJsonDocument::fromJson(entry.data).object();
        if (data.isEmpty())
            continue; // nothing worth sending, and an empty entryData would be rejected
        QJsonObject wrapper;
        wrapper.insert(QStringLiteral("timestamp"),
                        QString::fromStdString(Iso8601::formatLocal(entry.timestamp,
                                                                      offsetMinutes)));
        wrapper.insert(QStringLiteral("entryData"), data);
        array.append(wrapper);
    }
    if (array.isEmpty()) {
        callback(true, QString());
        return;
    }

    QNetworkRequest request = authorizedRequest(kHealthBaseUrl + kind, sessionKey);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                       QStringLiteral("application/json; charset=UTF-8"));

    QNetworkReply *reply = m_network->post(request, QJsonDocument(array).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, kind, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            const QByteArray body = reply->readAll();
            callback(false, body.isEmpty()
                     ? reply->errorString()
                     : QStringLiteral("%1: %2").arg(reply->errorString(),
                                                      QString::fromUtf8(body.left(200))));
            return;
        }
        callback(true, QString());
    });
}

void SuuntoCloudClient::fetchHealthEntries(const QString &sessionKey, const QString &kind,
                                            qint64 sinceMs, HealthCallback callback)
{
    const QNetworkRequest request = authorizedRequest(
            kHealthBaseUrl + kind + QStringLiteral("/export?since=")
                    + QString::number(sinceMs),
            sessionKey);

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, kind, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, {}, reply->errorString());
            return;
        }

        // 204 means "nothing newer than `since`" - a success, not an error,
        // and the body is empty rather than "[]".
        const int status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 204) {
            callback(true, {}, QString());
            return;
        }

        QVector<HealthEntry> entries;
        int malformed = 0;
        const QList<QByteArray> lines = reply->readAll().split('\n');
        for (const QByteArray &line : lines) {
            const QByteArray trimmed = line.trimmed();
            if (trimmed.isEmpty())
                continue;
            const QJsonObject obj = QJsonDocument::fromJson(trimmed).object();
            const QString stamp = obj.value(QStringLiteral("timestamp")).toString();
            const QJsonValue data = obj.value(QStringLiteral("entryData"));

            // Skip rather than guess: an entry whose timestamp can't be read
            // has no place to go in a table keyed by time, and silently
            // filing it at the epoch would be worse than dropping it.
            qint64 ms = 0;
            int64_t parsed = 0;
            if (stamp.isEmpty() || !data.isObject()
                    || !Iso8601::parseToUnixMs(stamp.toStdString(), &parsed)) {
                ++malformed;
                continue;
            }
            ms = static_cast<qint64>(parsed);

            HealthEntry entry;
            entry.kind = kind;
            entry.timestamp = ms;
            entry.data = QJsonDocument(data.toObject()).toJson(QJsonDocument::Compact);
            entries.append(entry);
        }

        // Partial success is still success - one unreadable line shouldn't
        // cost a night of sleep data - but it is worth saying out loud
        // rather than hiding, since it would mean the format has moved.
        if (malformed > 0 && entries.isEmpty()) {
            callback(false, {}, tr("Could not read any %1 entries (%2 unparseable)")
                                  .arg(kind).arg(malformed));
            return;
        }
        callback(true, entries, QString());
    });
}

void SuuntoCloudClient::fetchWorkoutDetail(const QString &sessionKey,
                                            const QString &key,
                                            WorkoutDetailCallback callback)
{
    const QNetworkRequest request = authorizedRequest(
            kBaseUrl + QStringLiteral("workouts/") + key, sessionKey);

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            callback(false, QJsonObject(), reply->errorString());
            return;
        }

        // Same ASKO envelope as listWorkouts(), payload being one object
        // rather than an array.
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            callback(false, QJsonObject(), tr("Unexpected response from server"));
            return;
        }
        const QJsonObject envelope = doc.object();
        if (!envelope.value(QStringLiteral("error")).isNull()) {
            const QJsonObject err = envelope.value(QStringLiteral("error")).toObject();
            callback(false, QJsonObject(),
                     tr("Server error %1: %2")
                             .arg(err.value(QStringLiteral("code")).toString(
                                      QString::number(err.value(QStringLiteral("code"))
                                                              .toInt())))
                             .arg(err.value(QStringLiteral("description")).toString()));
            return;
        }
        callback(true, envelope.value(QStringLiteral("payload")).toObject(), QString());
    });
}

// The nine type names the official app asks for, verbatim from a capture
// and in its order. A subset would very likely work - the server is being
// told what to include, not matched against a signature - but there is no
// measurement saying so, and sending exactly what was observed costs
// nothing: the three types this app reads are in the list, and the rest
// come back as extra rows in the field table.
static const char *const kExtensionTypes[] = {
    "SummaryExtension", "FitnessExtension", "SkiExtension", "IntensityExtension",
    "DiveHeaderExtension", "SwimmingHeaderExtension", "WeatherExtension",
    "WeatherStreamExtension", "JumpRopeExtension",
};

void SuuntoCloudClient::fetchWorkoutExtensions(const QString &sessionKey,
                                                const QString &key,
                                                WorkoutDetailCallback callback)
{
    QNetworkRequest request = authorizedRequest(
            kBaseUrl + QStringLiteral("workout/extensions/") + key, sessionKey);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                       QStringLiteral("application/json"));

    QJsonArray types;
    for (const char *const type : kExtensionTypes)
        types.append(QString::fromLatin1(type));

    QNetworkReply *reply =
            m_network->post(request, QJsonDocument(types).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        reply->deleteLater();

        // 403 is a real, reproducible answer for some workouts rather than
        // a transient failure - see docs/workout-upload.md. QNetworkReply
        // reports it as an error, and the body still carries the envelope,
        // so the error text below comes from the envelope where there is
        // one.
        const QByteArray body = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            callback(false, QJsonObject(),
                     reply->error() != QNetworkReply::NoError
                             ? reply->errorString()
                             : tr("Unexpected response from server"));
            return;
        }
        const QJsonObject envelope = doc.object();
        if (!envelope.value(QStringLiteral("error")).isNull()) {
            const QJsonObject err = envelope.value(QStringLiteral("error")).toObject();
            callback(false, QJsonObject(),
                     tr("Server error %1: %2")
                             .arg(err.value(QStringLiteral("code")).toString(
                                      QString::number(err.value(QStringLiteral("code")).toInt())))
                             .arg(err.value(QStringLiteral("description")).toString()));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, QJsonObject(), reply->errorString());
            return;
        }
        callback(true, envelope.value(QStringLiteral("payload")).toObject(), QString());
    });
}

void SuuntoCloudClient::fetchPersonalRecords(const QString &sessionKey,
                                              const QString &timezone,
                                              JsonArrayCallback callback)
{
    // The zone is percent-encoded, slash included, exactly as the captured
    // request has it: tz=Europe%2FHelsinki.
    const QString url = kBaseUrlV2 + QStringLiteral("personal/best/records?statsVersion=V2&tz=")
            + QString::fromUtf8(QUrl::toPercentEncoding(timezone));
    const QNetworkRequest request = authorizedRequest(url, sessionKey);

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            callback(false, QJsonArray(), reply->errorString());
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            callback(false, QJsonArray(), tr("Unexpected response from server"));
            return;
        }
        const QJsonObject envelope = doc.object();
        if (!envelope.value(QStringLiteral("error")).isNull()) {
            const QJsonObject err = envelope.value(QStringLiteral("error")).toObject();
            callback(false, QJsonArray(),
                     tr("Server error %1: %2")
                             .arg(err.value(QStringLiteral("code")).toString(
                                      QString::number(err.value(QStringLiteral("code"))
                                                              .toInt())))
                             .arg(err.value(QStringLiteral("description")).toString()));
            return;
        }
        callback(true, envelope.value(QStringLiteral("payload")).toArray(), QString());
    });
}

void SuuntoCloudClient::downloadSuuntoPlusPlugin(const QString &sessionKey,
                                                  const QString &id,
                                                  const QString &capabilities,
                                                  PluginDownloadCallback callback)
{
    fetchSuuntoPlusZip(QStringLiteral("suuntoplus/features/plugins/%1?capabilities=%2")
                               .arg(QString::fromUtf8(QUrl::toPercentEncoding(id)),
                                     QString::fromUtf8(QUrl::toPercentEncoding(capabilities))),
                        sessionKey, callback);
}

void SuuntoCloudClient::downloadSuuntoPlusGuide(const QString &sessionKey,
                                                 const QString &id,
                                                 const QString &capabilities,
                                                 PluginDownloadCallback callback)
{
    fetchSuuntoPlusZip(QStringLiteral("suuntoplus/guides/plugins/%1?capabilities=%2")
                               .arg(QString::fromUtf8(QUrl::toPercentEncoding(id)),
                                     QString::fromUtf8(QUrl::toPercentEncoding(capabilities))),
                        sessionKey, callback);
}

void SuuntoCloudClient::fetchSuuntoPlusZip(const QString &path, const QString &sessionKey,
                                            PluginDownloadCallback callback)
{
    const QNetworkRequest request = authorizedRequest(kBaseUrl + path, sessionKey);

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, QByteArray(), reply->errorString());
            return;
        }
        const QByteArray body = reply->readAll();
        // A zip, not the usual ASKO envelope, so the only shape check worth
        // making is the one the local header gives. An error here would
        // arrive as JSON and fail this.
        if (!body.startsWith("PK\x03\x04")) {
            callback(false, QByteArray(),
                     tr("Expected a zip, got %1 bytes starting %2")
                             .arg(body.size())
                             .arg(QString::fromLatin1(body.left(16).toHex())));
            return;
        }
        callback(true, body, QString());
    });
}

void SuuntoCloudClient::listSuuntoPlusFeatures(const QString &sessionKey,
                                                const QString &variant,
                                                const QString &capabilities,
                                                SuuntoPlusListCallback callback)
{
    // onlyMyLibrary=true is what the official app sends for the library
    // view; the whole catalogue is a different endpoint (store/sportsapps).
    fetchSuuntoPlusList(QStringLiteral("suuntoplus/features/items?onlyMyLibrary=true"
                                        "&variant=%1&capabilities=%2")
                                .arg(QString::fromUtf8(QUrl::toPercentEncoding(variant)),
                                      QString::fromUtf8(QUrl::toPercentEncoding(capabilities))),
                         sessionKey, callback);
}

void SuuntoCloudClient::listSuuntoPlusGuides(const QString &sessionKey,
                                              SuuntoPlusListCallback callback)
{
    fetchSuuntoPlusList(QStringLiteral("suuntoplus/guides/items"), sessionKey, callback);
}

void SuuntoCloudClient::fetchSuuntoPlusList(const QString &path, const QString &sessionKey,
                                             SuuntoPlusListCallback callback)
{
    const QNetworkRequest request = authorizedRequest(kBaseUrl + path, sessionKey);

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            callback(false, {}, reply->errorString());
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            callback(false, {}, tr("Unexpected response from server"));
            return;
        }
        const QJsonObject envelope = doc.object();
        if (!envelope.value(QStringLiteral("error")).isNull()) {
            const QJsonObject err = envelope.value(QStringLiteral("error")).toObject();
            // The code arrives as a string on some endpoints and a number on
            // others, which cost an afternoon once already.
            callback(false, {},
                     tr("Server error %1: %2")
                             .arg(err.value(QStringLiteral("code")).toString(
                                      QString::number(err.value(QStringLiteral("code"))
                                                              .toInt())))
                             .arg(err.value(QStringLiteral("description")).toString()));
            return;
        }

        QVector<SuuntoPlusItem> items;
        const QJsonArray payload = envelope.value(QStringLiteral("payload")).toArray();
        items.reserve(payload.size());
        for (const QJsonValue &v : payload) {
            const QJsonObject o = v.toObject();
            SuuntoPlusItem item;
            item.id = o.value(QStringLiteral("id")).toString();
            item.pluginId = o.value(QStringLiteral("pluginId")).toString();
            item.name = o.value(QStringLiteral("name")).toString();
            item.description = o.value(QStringLiteral("description")).toString();
            item.type = o.value(QStringLiteral("type")).toString();
            item.version = o.value(QStringLiteral("version")).toString();
            item.iconUrl = o.value(QStringLiteral("iconUrl")).toString();
            item.enabled = o.value(QStringLiteral("enabled")).toBool();
            item.modificationTime =
                    static_cast<qint64>(o.value(QStringLiteral("modificationTime")).toDouble());
            if (!item.id.isEmpty())
                items.append(item);
        }
        callback(true, items, QString());
    });
}

void SuuntoCloudClient::downloadWorkoutFit(const QString &sessionKey,
                                            const QString &workoutKey,
                                            RawDownloadCallback callback)
{
    const QString path = QStringLiteral("workout/exportFit/%1")
            .arg(QString::fromUtf8(QUrl::toPercentEncoding(workoutKey)));
    QNetworkReply *reply = m_network->get(authorizedRequest(kBaseUrl + path, sessionKey));
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            // The body is worth keeping in the message: an ASKO envelope
            // says why, and an HTML page says this is not the API at all.
            callback(false, QByteArray(),
                     tr("%1 (%2 bytes: %3)")
                             .arg(reply->errorString())
                             .arg(body.size())
                             .arg(QString::fromUtf8(body.left(200))));
            return;
        }
        // A FIT file's header carries ".FIT" at offset 8. Checked rather
        // than assumed, because this endpoint is not one the official app
        // uses and a 200 carrying JSON would otherwise be written to disk
        // as a .fit nobody can open.
        if (body.size() < 14 || body.mid(8, 4) != QByteArray(".FIT")) {
            callback(false, QByteArray(),
                     tr("The answer is not a FIT file: %1 bytes starting %2")
                             .arg(body.size())
                             .arg(QString::fromUtf8(body.left(80))));
            return;
        }
        callback(true, body, QString());
    });
}

void SuuntoCloudClient::fetchStoreHome(const QString &sessionKey, const QString &variant,
                                       const QString &capabilities,
                                       StoreHomeCallback callback)
{
    const QString path = QStringLiteral("store/home?variant=%1&capabilities=%2")
            .arg(QString::fromUtf8(QUrl::toPercentEncoding(variant)),
                  QString::fromUtf8(QUrl::toPercentEncoding(capabilities)));
    const QNetworkRequest request = authorizedRequest(kBaseUrl + path, sessionKey);

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            // This endpoint answered 403 three times in a row in the capture
            // and then 200 to the identical request, so a refusal here is
            // worth retrying before it is worth believing.
            callback(false, StoreHome(), reply->errorString());
            return;
        }
        const QJsonObject envelope = QJsonDocument::fromJson(reply->readAll()).object();
        const QString error = envelopeError(envelope);
        if (!error.isEmpty()) {
            callback(false, StoreHome(), tr("Server error %1").arg(error));
            return;
        }
        const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();

        StoreHome home;
        for (const QJsonValue &value : payload.value(QStringLiteral("items")).toArray()) {
            const QJsonObject object = value.toObject();
            StoreItem item;
            item.id = object.value(QStringLiteral("id")).toString();
            item.type = object.value(QStringLiteral("type")).toString();
            item.title = object.value(QStringLiteral("title")).toString();
            item.subTitle = object.value(QStringLiteral("subTitle")).toString().trimmed();
            item.description = object.value(QStringLiteral("description")).toString();
            item.iconUrl = object.value(QStringLiteral("iconUrl")).toString();
            item.bannerUrl = object.value(QStringLiteral("tileBannerV2Url")).toString();
            if (item.bannerUrl.isEmpty())
                item.bannerUrl = object.value(QStringLiteral("tileBannerUrl")).toString();
            item.richText = object.value(QStringLiteral("richText")).toString();
            item.ownerTitle = object.value(QStringLiteral("owner")).toObject()
                    .value(QStringLiteral("title")).toString();
            item.version = object.value(QStringLiteral("version")).toString();
            const QJsonObject rating = object.value(QStringLiteral("rating")).toObject();
            item.avgRating = rating.value(QStringLiteral("avgRating")).toDouble();
            item.reviews = rating.value(QStringLiteral("reviews")).toInt();
            item.inLibrary = object.value(QStringLiteral("libraryStatus")).toString()
                    == QStringLiteral("ADDED");
            if (!item.id.isEmpty())
                home.items.append(item);
        }
        for (const QJsonValue &value : payload.value(QStringLiteral("categories")).toArray()) {
            const QJsonObject object = value.toObject();
            StoreCategory category;
            category.id = object.value(QStringLiteral("id")).toString();
            category.title = object.value(QStringLiteral("title")).toString();
            // The refs are grouped by type inside a category, and the groups
            // are kept in the order the store gives them. A category's
            // `type` says nothing about what the items are: the watch-face
            // category's refs are all typed `sportsapp`.
            for (const QJsonValue &group : object.value(QStringLiteral("types")).toArray()) {
                for (const QJsonValue &ref :
                             group.toObject().value(QStringLiteral("itemRefs")).toArray()) {
                    const QString id = ref.toObject().value(QStringLiteral("id")).toString();
                    if (!id.isEmpty())
                        category.itemIds.append(id);
                }
            }
            if (!category.itemIds.isEmpty())
                home.categories.append(category);
        }
        callback(true, home, QString());
    });
}

void SuuntoCloudClient::addToStoreLibrary(const QString &sessionKey, const QString &type,
                                           const QString &id, SimpleCallback callback)
{
    QNetworkRequest request = authorizedRequest(kBaseUrl + QStringLiteral("store/library"),
                                                 sessionKey);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                       QStringLiteral("application/json; charset=UTF-8"));

    QJsonObject body;
    body.insert(QStringLiteral("type"), type);
    body.insert(QStringLiteral("id"), id);
    body.insert(QStringLiteral("addToWatch"), true);

    QNetworkReply *reply =
            m_network->put(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        reply->deleteLater();
        const QByteArray answer = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, reply->errorString());
            return;
        }
        // 201, with the added item as the payload. The envelope is checked
        // all the same: this endpoint answers 200-with-an-error elsewhere.
        const QString error = envelopeError(QJsonDocument::fromJson(answer).object());
        if (!error.isEmpty()) {
            callback(false, tr("Server error %1").arg(error));
            return;
        }
        callback(true, QString());
    });
}

void SuuntoCloudClient::removeFromStoreLibrary(const QString &sessionKey, const QString &type,
                                                const QString &id, SimpleCallback callback)
{
    // The store's two plural path segments. Anything that is not a guide is
    // a sportsapp here, watch faces included - see StoreItem.
    //
    // Both words for a guide are accepted, because both callers are right:
    // the store calls it a `guide` and the library listing calls the same
    // thing a `sequence`, and a removal is offered from either page. The
    // mapping belongs here rather than in one of them, where the other
    // would eventually get it wrong and silently delete nothing.
    const QString segment = (type == QStringLiteral("guide")
                              || type == QStringLiteral("sequence"))
            ? QStringLiteral("guides")
            : QStringLiteral("sportsapps");
    const QString path = QStringLiteral("store/library/%1/%2")
            .arg(segment, QString::fromUtf8(QUrl::toPercentEncoding(id)));

    QNetworkReply *reply =
            m_network->deleteResource(authorizedRequest(kBaseUrl + path, sessionKey));
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        reply->deleteLater();
        const QByteArray answer = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, reply->errorString());
            return;
        }
        const QString error = envelopeError(QJsonDocument::fromJson(answer).object());
        if (!error.isEmpty()) {
            callback(false, tr("Server error %1").arg(error));
            return;
        }
        callback(true, QString());
    });
}

void SuuntoCloudClient::listWorkouts(const QString &sessionKey, qint64 sinceMs, int limit,
                                      int offset, WorkoutListCallback callback)
{
    const QString path = QStringLiteral("workouts?since=%1&limit=%2&offset=%3")
                                 .arg(sinceMs)
                                 .arg(limit)
                                 .arg(offset);
    const QNetworkRequest request = authorizedRequest(kBaseUrl + path, sessionKey);

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, callback]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            callback(false, {}, WorkoutPageInfo(), reply->errorString());
            return;
        }

        // Envelope per tajchert/suuntool's AskoResponse[T]:
        // {"error": null|{"code":int,"description":string}, "metadata": {...}, "payload": T}
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            callback(false, {}, WorkoutPageInfo(),
                     tr("Unexpected response from server"));
            return;
        }
        const QJsonObject envelope = doc.object();
        if (!envelope.value(QStringLiteral("error")).isNull()) {
            const QJsonObject err = envelope.value(QStringLiteral("error")).toObject();
            callback(false, {}, WorkoutPageInfo(),
                     tr("Server error %1: %2")
                             .arg(err.value(QStringLiteral("code")).toString(
                                      QString::number(err.value(QStringLiteral("code"))
                                                              .toInt())))
                             .arg(err.value(QStringLiteral("description")).toString()));
            return;
        }

        QVector<Workout> workouts;
        const QJsonArray payload = envelope.value(QStringLiteral("payload")).toArray();
        workouts.reserve(payload.size());
        for (const QJsonValue &v : payload) {
            const QJsonObject o = v.toObject();
            Workout w;
            w.key = o.value(QStringLiteral("key")).toString();
            // The cloud's other identifier for the same workout - see
            // Workout::workoutKey for why both are kept, and for the fact
            // that it is `workoutKey` that holds the ObjectId and `key`
            // that holds the short one, not the other way round.
            w.workoutKey = o.value(QStringLiteral("workoutKey")).toString();
            w.source = QStringLiteral("cloud");
            w.activityId = o.value(QStringLiteral("activityId")).toInt();
            // startTime/stopTime (unix ms) arrive as JSON numbers. QJsonValue
            // stores all numbers as double regardless of accessor, so this
            // cast isn't losing anything toInt64()-equivalent wouldn't also
            // lose - a double's 53-bit mantissa covers unix-ms timestamps
            // exactly until roughly the year 287396, not a practical concern.
            w.startTime = qint64(o.value(QStringLiteral("startTime")).toDouble());
            w.stopTime = qint64(o.value(QStringLiteral("stopTime")).toDouble());
            w.totalTime = o.value(QStringLiteral("totalTime")).toDouble();
            w.totalDistance = o.value(QStringLiteral("totalDistance")).toDouble();
            w.totalAscent = o.value(QStringLiteral("totalAscent")).toDouble();
            w.totalDescent = o.value(QStringLiteral("totalDescent")).toDouble();
            w.maxSpeed = o.value(QStringLiteral("maxSpeed")).toDouble();
            w.energyConsumption = o.value(QStringLiteral("energyConsumption")).toDouble();
            w.stepCount = o.value(QStringLiteral("stepCount")).toInt();
            // hrdata is itself an optional nested object (RemoteSyncedWorkout.HRData
            // in tajchert/suuntool) - toObject() on a missing/non-object value
            // returns {} harmlessly, so avg/max just fall back to Workout's own
            // 0-means-absent default without an extra presence check here.
            const QJsonObject hrData = o.value(QStringLiteral("hrdata")).toObject();
            w.avgHeartRate = hrData.value(QStringLiteral("avg")).toDouble();
            w.maxHeartRate = hrData.value(QStringLiteral("max")).toDouble();

            // Three more fields the list response has always carried and
            // this client used to drop on the floor. "tss" is an object
            // rather than a number (it also holds intensity factor and
            // normalised power, not used yet), and recoveryTime is in
            // seconds like the watch's own.
            w.recoveryTime = o.value(QStringLiteral("recoveryTime")).toDouble();
            const QJsonObject tss = o.value(QStringLiteral("tss")).toObject();
            w.trainingStressScore =
                    tss.value(QStringLiteral("trainingStressScore")).toDouble();
            // Kept as-is here and decoded on the way into storage, so this
            // client stays a thin JSON-to-struct mapping.
            w.polyline = o.value(QStringLiteral("polyline")).toString();
            workouts.append(w);
        }

        // metadata.until is the server's own clock, and both it and
        // workoutcount arrive as JSON *strings* rather than numbers -
        // "1791205512579", "7" - so they are parsed rather than read as
        // doubles. A missing or unparseable value leaves the defaults,
        // which make the caller keep whatever cursor it already had.
        WorkoutPageInfo page;
        const QJsonObject metadata = envelope.value(QStringLiteral("metadata")).toObject();
        bool parsed = false;
        const qint64 until =
                metadata.value(QStringLiteral("until")).toString().toLongLong(&parsed);
        if (parsed)
            page.untilMs = until;
        const int count =
                metadata.value(QStringLiteral("workoutcount")).toString().toInt(&parsed);
        if (parsed)
            page.reportedCount = count;

        callback(true, workouts, page, QString());
    });
}
