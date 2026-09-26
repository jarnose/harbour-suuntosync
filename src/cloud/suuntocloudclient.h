#pragma once

#include "../store/workout.h"
#include "../health/healthentry.h"

#include <QJsonObject>
#include <QNetworkRequest>
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

    // The signed-in account's email. Currently unused for request building
    // - see authorizedRequest() for the x-totp story - but kept because the
    // two endpoints that do want it are keyed by it.
    void setAccountEmail(const QString &email) { m_accountEmail = email; }

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

    // GET /v1/workouts/{key}/sml - the workout's full sample data. Despite
    // the path it is JSON rather than the binary SBEM the watch serves, and
    // it runs to several megabytes per workout, so this is only ever called
    // from an explicit user action. Handed back as the raw body: the shape
    // hasn't been captured yet, so the caller does the interpreting.
    using RawBodyCallback = std::function<void(bool ok, const QByteArray &body,
                                                 const QString &error)>;
    void fetchWorkoutSml(const QString &sessionKey, const QString &workoutKey,
                          RawBodyCallback callback);

    // GET 247.sports-tracker.com/v1/<kind>/export?since=<ms> - the watch's
    // round-the-clock data: "sleep", "sleepstages", "recovery", "activity".
    //
    // A different host and a different response convention from everything
    // above: no ASKO envelope, just NDJSON (one {"timestamp","entryData"}
    // object per line), and 204 for "nothing new" rather than an empty
    // list. Authentication is the same session key. All confirmed against a
    // real capture - see docs/workout-upload.md's health section.
    //
    // `sinceMs` is passed through to the server's own filter; 0 fetches
    // everything the account has, which for a long-standing account is
    // years of history.
    // POST /apiserver/v1/workout - uploads a watch-recorded workout.
    //
    // Multipart with exactly two parts, which is what a real capture of the
    // official app syncing from the watch contains (docs/workout-upload.md):
    // `workoutExtensions` (the JSON "[]") and `sml` (a zip of samples.json
    // and summary.json). Notably there is NO `workoutBinary` part - that is
    // what a phone-recorded workout sends instead; the two are
    // alternatives, and a watch upload carrying only SML was accepted with
    // 200. Authentication is the plain session key, no signature.
    //
    // On success the payload is the created workout, whose "key" is the
    // cloud's own id for it.
    using UploadCallback = std::function<void(bool ok, const QString &workoutKey,
                                                const QString &error)>;
    void uploadWorkout(const QString &sessionKey, const QByteArray &smlZip,
                        UploadCallback callback);

    using HealthCallback = std::function<void(bool ok, const QVector<HealthEntry> &entries,
                                                const QString &error)>;
    void fetchHealthEntries(const QString &sessionKey, const QString &kind, qint64 sinceMs,
                             HealthCallback callback);

    // POST 247.sports-tracker.com/v1/<kind> - pushes entries the watch gave
    // us up to the cloud, so data read over BLE ends up where the official
    // app would have put it.
    //
    // Body shape and headers copied from the capture, not chosen: a plain
    // JSON array of {"timestamp": <ISO 8601>, "entryData": {...}} with
    // content-type "application/json; charset=UTF-8" and the ordinary
    // session key. No envelope, no signature. See
    // docs/workout-upload.md's health section.
    //
    // `offsetMinutes` stamps the timestamps; the cloud's own entries carry
    // a local offset rather than UTC.
    // GPS assist data for a watch, from the same host the official app
    // uses. Unauthenticated on purpose rather than by omission: the
    // endpoints ignore the appkey the app sends, tested four ways
    // (docs/watch-push-resources.md), so nothing here needs a session or
    // an account. `formatIndex` is what the watch answered for
    // /Device/GNSS/ExtendedEphemerisData/Format.
    using RawDownloadCallback = std::function<void(bool ok, const QByteArray &data,
                                                     const QString &error)>;
    void downloadEphemeris(int formatIndex, RawDownloadCallback callback);

    using SimpleCallback = std::function<void(bool ok, const QString &error)>;
    void uploadHealthEntries(const QString &sessionKey, const QString &kind,
                              const QVector<HealthEntry> &entries, int offsetMinutes,
                              SimpleCallback callback);

private:
    // Every authenticated call sends the same four headers - and
    // deliberately NOT x-totp.
    //
    // This briefly did send it, on the strength of
    // Marius-Ar/suunto-api-wrapper putting it on every request. A real
    // capture of the official app (2026-09-22, see docs/workout-upload.md)
    // settled it: the app sends x-totp on 2 of 122 requests - a user
    // email-status check and a settings POST - and not on the workout
    // upload or on any read this project makes. Sending it everywhere
    // imitates a third-party client rather than the app, so it is gone.
    // SuuntoAuth::generateTotp() stays, for whichever endpoint eventually
    // needs it.
    QNetworkRequest authorizedRequest(const QString &url, const QString &sessionKey) const;

    QNetworkAccessManager *m_network;
    QString m_accountEmail;
};
