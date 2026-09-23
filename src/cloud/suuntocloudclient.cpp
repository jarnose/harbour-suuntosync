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
#include <QUrl>
#include <QByteArray>

#include <random>

namespace {

const QString kBaseUrl = QStringLiteral("https://api.sports-tracker.com/apiserver/v1/");
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

void SuuntoCloudClient::fetchWorkoutSml(const QString &sessionKey, const QString &workoutKey,
                                          RawBodyCallback callback)
{
    const QNetworkRequest request = authorizedRequest(
            kBaseUrl + QStringLiteral("workouts/") + workoutKey + QStringLiteral("/sml"),
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
                             .arg(err.value(QStringLiteral("code")).toInt())
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
                                            const QString &workoutKey,
                                            WorkoutDetailCallback callback)
{
    const QNetworkRequest request = authorizedRequest(
            kBaseUrl + QStringLiteral("workouts/") + workoutKey, sessionKey);

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
                             .arg(err.value(QStringLiteral("code")).toInt())
                             .arg(err.value(QStringLiteral("description")).toString()));
            return;
        }
        callback(true, envelope.value(QStringLiteral("payload")).toObject(), QString());
    });
}

void SuuntoCloudClient::listWorkouts(const QString &sessionKey, int limit,
                                      WorkoutListCallback callback)
{
    const QString path = QStringLiteral("workouts?since=0&limit=%1&offset=0").arg(limit);
    const QNetworkRequest request = authorizedRequest(kBaseUrl + path, sessionKey);

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, callback]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            callback(false, {}, reply->errorString());
            return;
        }

        // Envelope per tajchert/suuntool's AskoResponse[T]:
        // {"error": null|{"code":int,"description":string}, "metadata": {...}, "payload": T}
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            callback(false, {}, tr("Unexpected response from server"));
            return;
        }
        const QJsonObject envelope = doc.object();
        if (!envelope.value(QStringLiteral("error")).isNull()) {
            const QJsonObject err = envelope.value(QStringLiteral("error")).toObject();
            callback(false, {},
                     tr("Server error %1: %2")
                             .arg(err.value(QStringLiteral("code")).toInt())
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
        callback(true, workouts, QString());
    });
}
