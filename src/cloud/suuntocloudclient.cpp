#include "suuntocloudclient.h"
#include "suuntoauth.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
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
    QNetworkRequest request(QUrl(kBaseUrl + QStringLiteral("workouts/") + workoutKey
                                  + QStringLiteral("/sml")));
    request.setRawHeader("STTAuthorization", sessionKey.toUtf8());
    request.setRawHeader("User-Agent", kUserAgent.toUtf8());
    request.setRawHeader("Accept-Language", "en");

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

void SuuntoCloudClient::fetchWorkoutDetail(const QString &sessionKey,
                                            const QString &workoutKey,
                                            WorkoutDetailCallback callback)
{
    QNetworkRequest request(QUrl(kBaseUrl + QStringLiteral("workouts/") + workoutKey));
    request.setRawHeader("STTAuthorization", sessionKey.toUtf8());
    request.setRawHeader("User-Agent", kUserAgent.toUtf8());
    request.setRawHeader("Accept-Language", "en");

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
    QNetworkRequest request(QUrl(kBaseUrl + path));
    request.setRawHeader("STTAuthorization", sessionKey.toUtf8());
    request.setRawHeader("User-Agent", kUserAgent.toUtf8());
    request.setRawHeader("Accept-Language", "en");

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
