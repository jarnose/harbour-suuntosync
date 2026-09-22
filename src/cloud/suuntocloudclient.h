#pragma once

#include "../store/workout.h"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

// Thin QNetworkAccessManager client for the Suunto cloud account (Sports-
// Tracker backend). Request signing/TOTP is SuuntoAuth's job (see
// suuntoauth.h) - this class only builds/sends the actual HTTP requests and
// parses responses.
//
// Auth model here is simpler than the OAuth access/refresh pair Phase 2a's
// TokenVault/CloudAccountStore were originally scoped for: /login2 returns a
// single opaque "sessionkey" sent back as the STTAuthorization header on
// every subsequent request - no separate refresh token or documented expiry
// (confirmed by reading tajchert/suuntool's client, which never refreshes,
// only re-logs-in on a 401). AppController stores the sessionkey as
// TokenVault's "cloud_access_token" secret and leaves the unused
// "cloud_refresh_token" slot empty.
class SuuntoCloudClient : public QObject
{
    Q_OBJECT
public:
    struct Session
    {
        QString sessionKey;
        QString username;
        QString email;
        QString userKey;
        QString country;
        bool emailVerified = false;

        bool isValid() const { return !sessionKey.isEmpty(); }
    };

    using LoginCallback = std::function<void(bool ok, const Session &session,
                                               const QString &error)>;
    using WorkoutListCallback = std::function<void(bool ok, const QVector<Workout> &workouts,
                                                      const QString &error)>;
    using WorkoutDetailCallback = std::function<void(bool ok, const QJsonObject &workout,
                                                        const QString &error)>;

    explicit SuuntoCloudClient(QObject *parent = nullptr);

    // POSTs to /login2 with the signed+TOTP'd form body SuuntoAuth builds.
    // callback is invoked exactly once, on this object's thread.
    void login(const QString &email, const QString &password, LoginCallback callback);

    // GET /v1/workouts?since=0&limit=<limit>&offset=0, authenticated with
    // sessionKey (the STTAuthorization header). Only the first page (most
    // recent `limit` workouts, server max 100) - older-than-that pagination
    // isn't implemented yet.
    void listWorkouts(const QString &sessionKey, int limit, WorkoutListCallback callback);

    // GET /v1/workouts/{key} - the same fields as the list entry plus an
    // "extensions" array, which is where the cloud keeps the analysis the
    // list doesn't carry. Handed back as the raw payload object rather than
    // a struct: the extensions are typed by a discriminator and this
    // project has no captured example to model them from, so the caller
    // decides what to make of whatever arrives.
    void fetchWorkoutDetail(const QString &sessionKey, const QString &workoutKey,
                             WorkoutDetailCallback callback);

private:
    QNetworkAccessManager *m_network;
};
