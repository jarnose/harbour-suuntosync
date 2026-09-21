#include "suuntocloudclient.h"
#include "suuntoauth.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
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
