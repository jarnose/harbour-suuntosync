#include "appcontroller.h"
#include "../secrets/tokenvault.h"
#include "../cloud/cloudaccountstore.h"
#include "../ble/pairedwatchstore.h"
#include "../ble/devicelistmodel.h"
#include "../ble/mdswhiteboardclient.h"
#include "../cloud/suuntocloudclient.h"
#include "../store/workoutstore.h"
#include "../store/workout.h"
#include "../model/workoutlistmodel.h"
#include "../ble/logbookdecoder.h"

#include <QStandardPaths>
#include <QDir>
#include <QDateTime>

#include <algorithm>
#include <stdexcept>

namespace {

QString dbPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("suuntosync.sqlite"));
}

// "ble_" prefix keeps this in its own key namespace, separate from cloud
// workouts' own "wk_..." keys - deliberately not attempting to merge a
// BLE-synced and cloud-synced record of the same real workout into one row
// (no reliable cross-reference beyond timestamp proximity, which isn't
// solid enough to upsert-collide on automatically); they'll just show up
// as two entries in the list for now. See docs/logbook-data-format.md for
// which Workout fields Logbook::decode() can and can't populate -
// totalAscent/totalDescent/energyConsumption stay at 0 (absent) here for
// the same "not found in this resource" reasons documented there.
Workout workoutFromDecoded(const QString &logbookId, const Logbook::DecodedWorkout &decoded)
{
    Workout w;
    w.key = QStringLiteral("ble_%1").arg(logbookId);
    w.source = QStringLiteral("ble");
    w.activityId = decoded.activityId;
    w.startTime = static_cast<qint64>(decoded.startTimeMs);
    w.stopTime = static_cast<qint64>(decoded.stopTimeMs);
    w.totalTime = decoded.totalTimeSeconds;
    w.totalDistance = decoded.totalDistanceMeters;
    w.maxSpeed = decoded.maxSpeedMs;
    w.avgHeartRate = decoded.avgHeartRateBpm;
    w.maxHeartRate = decoded.maxHeartRateBpm;
    w.stepCount = decoded.stepCount;
    return w;
}

} // namespace

AppController::AppController(QObject *parent)
    : QObject(parent)
    , m_tokenVault(new TokenVault(this))
    , m_cloudAccountStore(new CloudAccountStore(dbPath()))
    , m_cloudClient(new SuuntoCloudClient(this))
    , m_bluezAdapter(new BluezAdapter(this))
    , m_deviceModel(new DeviceListModel(this))
    , m_pairedWatchStore(new PairedWatchStore(dbPath()))
    , m_whiteboardClient(new MdsWhiteboardClient(this))
    , m_workoutStore(new WorkoutStore(dbPath()))
    , m_workoutModel(new WorkoutListModel(this))
{
    QString error;
    if (!m_cloudAccountStore->open(&error)) {
        emit errorOccurred(tr("Failed to open database: %1").arg(error));
    } else {
        m_cloudAccount = m_cloudAccountStore->load(&error);
        if (!error.isEmpty())
            emit errorOccurred(tr("Failed to load account: %1").arg(error));
    }

    if (!m_workoutStore->open(&error))
        emit errorOccurred(tr("Failed to open database: %1").arg(error));

    if (!m_pairedWatchStore->open(&error)) {
        emit errorOccurred(tr("Failed to open database: %1").arg(error));
    } else {
        m_pairedWatch = m_pairedWatchStore->load(&error);
        if (!error.isEmpty())
            emit errorOccurred(tr("Failed to load paired watch: %1").arg(error));
    }

    // Safe to call every launch - "already exists" counts as success inside
    // TokenVault::ensureCollection(). Not gating startup on this: a Secrets
    // failure should degrade to "signed-out looking" state, not crash/hang
    // the app (same reasoning as OTP Cove's AppController::loadAccounts()).
    m_tokenVault->ensureCollection([this](bool ok, const QString &vaultError) {
        if (!ok)
            emit errorOccurred(tr("Failed to open secure storage: %1").arg(vaultError));
    });

    connect(m_bluezAdapter, &BluezAdapter::deviceUpdated, this, &AppController::onDeviceUpdated);
    connect(m_bluezAdapter, &BluezAdapter::connectFinished,
            this, &AppController::onConnectFinished);
    connect(m_bluezAdapter, &BluezAdapter::errorOccurred, this, &AppController::errorOccurred);

    connect(m_whiteboardClient, &MdsWhiteboardClient::readyChanged, this, [this](bool ready) {
        m_whiteboardReady = ready;
        emit whiteboardReadyChanged();
    });
    connect(m_whiteboardClient, &MdsWhiteboardClient::errorOccurred,
            this, &AppController::errorOccurred);
}

AppController::~AppController() = default;

QObject *AppController::deviceModelObject() const
{
    return m_deviceModel;
}

QObject *AppController::workoutModelObject() const
{
    return m_workoutModel;
}

void AppController::onDeviceUpdated(const BluezAdapter::Device &device)
{
    // Only surface devices that look like a Suunto watch - BlueZ otherwise
    // reports every device it's ever seen (headphones, the car, etc.).
    // "Suunto Race 2352D0000247" is the confirmed advertised name for
    // Jarno's Race; matching on the "Suunto " prefix should cover the 9
    // Baro and future models too without needing a per-model list.
    if (!device.name.startsWith(QStringLiteral("Suunto "), Qt::CaseInsensitive))
        return;
    m_deviceModel->upsert(device);

    if (device.objectPath != m_pairedWatch.objectPath)
        return;

    if (device.connected != m_watchConnected) {
        m_watchConnected = device.connected;
        emit watchConnectedChanged();
    }

    // Covers both "just connected via selectWatch()" (onConnectFinished
    // already attaches it too - harmless to attach again, attachToDevice()
    // detaches any prior state first) and "was already connected at the
    // BlueZ level from before this app process started" (e.g. after a
    // rebuild/relaunch - onConnectFinished never fires again in that case
    // since we don't auto-reconnect, so this was the only place that could
    // ever attach the Whiteboard client for an already-connected watch).
    if (device.connected && !m_whiteboardReady)
        m_whiteboardClient->attachToDevice(device.objectPath);
}

void AppController::onConnectFinished(const QString &objectPath, bool ok, const QString &error)
{
    if (objectPath != m_pairedWatch.objectPath)
        return;
    if (!ok) {
        emit errorOccurred(tr("Could not connect to watch: %1").arg(error));
        return;
    }
    m_watchConnected = true;
    emit watchConnectedChanged();
    m_whiteboardClient->attachToDevice(objectPath);
}

void AppController::refreshDevices()
{
    m_bluezAdapter->refresh();
}

void AppController::startScan()
{
    m_bluezAdapter->startDiscovery();
}

void AppController::stopScan()
{
    m_bluezAdapter->stopDiscovery();
}

void AppController::selectWatch(const QString &objectPath, const QString &address,
                                 const QString &name)
{
    PairedWatch watch;
    watch.objectPath = objectPath;
    watch.address = address;
    watch.name = name;
    // Model (e.g. "Race", "9 Baro") isn't reliably derivable from the
    // advertised name alone - left blank until Phase 8 needs it for
    // protocol branching.

    QString error;
    if (!m_pairedWatchStore->save(watch, &error)) {
        emit errorOccurred(tr("Failed to save paired watch: %1").arg(error));
        return;
    }
    m_pairedWatch = watch;
    m_watchConnected = false;
    emit pairedWatchChanged();
    emit watchConnectedChanged();

    m_bluezAdapter->connectToDevice(objectPath);
}

void AppController::forgetWatch()
{
    QString error;
    if (!m_pairedWatchStore->clear(&error)) {
        emit errorOccurred(tr("Failed to forget watch: %1").arg(error));
        return;
    }
    if (m_watchConnected)
        m_bluezAdapter->disconnectFromDevice(m_pairedWatch.objectPath);
    m_whiteboardClient->detach();
    m_pairedWatch = PairedWatch();
    m_watchConnected = false;
    emit pairedWatchChanged();
    emit watchConnectedChanged();
}

void AppController::testWhiteboard()
{
    if (!m_whiteboardReady) {
        emit whiteboardTestResult(tr("Whiteboard channel isn't ready yet"));
        return;
    }
    if (m_whiteboardTestInFlight) {
        emit whiteboardTestResult(tr("A test request is already in flight"));
        return;
    }

    m_whiteboardTestInFlight = true;
    m_whiteboardClient->get(QStringLiteral("/Logbook/Entries"),
                             [this](bool ok, const Mds::Frame &frame, const QString &error) {
        m_whiteboardTestInFlight = false;
        if (!ok) {
            emit whiteboardTestResult(tr("Request failed: %1").arg(error));
            return;
        }
        emit whiteboardTestResult(tr("OK - type=0x%1 requestId=%2 body=%3 bytes")
                                           .arg(frame.type, 2, 16, QLatin1Char('0'))
                                           .arg(frame.requestId)
                                           .arg(frame.body.size()));
    });
}

void AppController::testLogbookFetch(const QString &logbookId)
{
    if (!m_whiteboardReady) {
        emit logbookTestResult(tr("Whiteboard channel isn't ready yet"));
        return;
    }
    if (m_logbookTestInFlight) {
        emit logbookTestResult(tr("A logbook fetch is already in flight"));
        return;
    }
    if (m_workoutSyncInProgress) {
        emit logbookTestResult(tr("A watch sync is already in progress"));
        return;
    }

    m_logbookTestInFlight = true;
    const QString path = QStringLiteral("/Logbook/byId/%1/Data").arg(logbookId);
    m_whiteboardClient->fetchLogbookData(path,
            [this, logbookId](bool ok, const std::vector<uint8_t> &data, const QString &error) {
        m_logbookTestInFlight = false;
        if (!ok) {
            emit logbookTestResult(tr("Fetch failed: %1").arg(error));
            return;
        }

        try {
            const Logbook::DecodedWorkout decoded = Logbook::decode(data);
            const Workout w = workoutFromDecoded(logbookId, decoded);

            QString storeError;
            if (!m_workoutStore->upsert(w, &storeError)) {
                emit logbookTestResult(tr("Decoded OK but failed to save: %1").arg(storeError));
                return;
            }
            loadCachedWorkouts();

            emit logbookTestResult(
                    tr("OK - saved. %1 bytes compressed, activity=%2 duration=%3s "
                       "distance=%4m maxSpeed=%5m/s avgHR=%6 maxHR=%7 steps=%8")
                            .arg(data.size())
                            .arg(decoded.activityId)
                            .arg(decoded.totalTimeSeconds, 0, 'f', 0)
                            .arg(decoded.totalDistanceMeters, 0, 'f', 0)
                            .arg(decoded.maxSpeedMs, 0, 'f', 1)
                            .arg(decoded.avgHeartRateBpm, 0, 'f', 0)
                            .arg(decoded.maxHeartRateBpm, 0, 'f', 0)
                            .arg(decoded.stepCount));
        } catch (const std::exception &e) {
            emit logbookTestResult(tr("Fetched %1 bytes but decoding failed: %2")
                                            .arg(data.size())
                                            .arg(QString::fromUtf8(e.what())));
        }
    });
}

void AppController::testEntriesFetch()
{
    if (!m_whiteboardReady) {
        emit logbookTestResult(tr("Whiteboard channel isn't ready yet"));
        return;
    }
    if (m_logbookTestInFlight) {
        emit logbookTestResult(tr("A logbook fetch is already in flight"));
        return;
    }

    if (m_workoutSyncInProgress) {
        emit logbookTestResult(tr("A watch sync is already in progress"));
        return;
    }

    // Replaced the earlier attempt at reusing fetchLogbookData()'s
    // TYPE=0x10 stream-trigger mechanism (confirmed not to work for
    // /Entries on real hardware, see docs/logbook-data-format.md) with
    // fetchLogEntries(), built from decompiling libmds.so's own
    // protocol_v9 structure-deserializer code - a TYPE=0x0D handle-fetch
    // request, not a stream trigger. Confirmed working on real hardware
    // 2026-09-22 (see the doc's "Gate: PASSED" note).
    m_logbookTestInFlight = true;
    m_whiteboardClient->fetchLogEntries(QStringLiteral("/Logbook/Entries"),
            [this](bool ok, const std::vector<LogEntries::Entry> &entries, const QString &error) {
        m_logbookTestInFlight = false;
        if (!ok) {
            emit logbookTestResult(tr("/Entries fetch failed: %1").arg(error));
            return;
        }

        QStringList ids;
        for (const LogEntries::Entry &entry : entries)
            ids.append(QString::number(entry.id));

        emit logbookTestResult(tr("OK - %1 entries: %2")
                                        .arg(entries.size())
                                        .arg(ids.join(QStringLiteral(", "))));
    });
}

void AppController::loginToCloud(const QString &email, const QString &password)
{
    if (m_cloudLoginInProgress)
        return;
    m_cloudLoginInProgress = true;
    emit cloudLoginInProgressChanged();

    m_cloudClient->login(email, password,
            [this](bool ok, const SuuntoCloudClient::Session &session, const QString &error) {
        m_cloudLoginInProgress = false;
        emit cloudLoginInProgressChanged();

        if (!ok) {
            emit errorOccurred(tr("Cloud login failed: %1").arg(error));
            return;
        }

        // Sessionkey is the only credential this API hands out (no separate
        // refresh token - see suuntocloudclient.h) - stored under the same
        // secret name Phase 2a already reserved for it.
        m_tokenVault->storeSecret(CloudAccountStore::TokenSecretName, session.sessionKey.toUtf8(),
                [this](bool storeOk, const QString &storeError) {
            if (!storeOk)
                emit errorOccurred(tr("Failed to store login session: %1").arg(storeError));
        });

        CloudAccount account;
        account.email = session.email;
        account.athleteId = session.userKey;
        account.tokenExpiry = 0; // no documented expiry for this sessionkey
        account.lastSync = 0;
        QString saveError;
        if (!m_cloudAccountStore->save(account, &saveError)) {
            emit errorOccurred(tr("Failed to save account: %1").arg(saveError));
            return;
        }
        m_cloudAccount = account;
        emit cloudAccountChanged();
    });
}

void AppController::logoutFromCloud()
{
    QString error;
    if (!m_cloudAccountStore->clear(&error))
        emit errorOccurred(tr("Failed to clear account: %1").arg(error));
    m_tokenVault->deleteSecret(CloudAccountStore::TokenSecretName, [](bool, const QString &) {});
    m_cloudAccount = CloudAccount();
    emit cloudAccountChanged();
}

void AppController::loadCachedWorkouts()
{
    QString error;
    const QVector<Workout> workouts = m_workoutStore->loadAll(&error);
    if (!error.isEmpty()) {
        emit errorOccurred(tr("Failed to load workouts: %1").arg(error));
        return;
    }
    m_workoutModel->setWorkouts(workouts);
}

void AppController::syncCloudWorkouts()
{
    if (!m_cloudAccount.isSignedIn() || m_workoutSyncInProgress)
        return;

    m_workoutSyncInProgress = true;
    emit workoutSyncInProgressChanged();

    m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
            [this](bool ok, const QByteArray &data, const QString &loadError) {
        if (!ok) {
            m_workoutSyncInProgress = false;
            emit workoutSyncInProgressChanged();
            emit errorOccurred(tr("Failed to load login session: %1").arg(loadError));
            return;
        }

        const QString sessionKey = QString::fromUtf8(data);
        m_cloudClient->listWorkouts(sessionKey, 100,
                [this](bool listOk, const QVector<Workout> &workouts, const QString &listError) {
            m_workoutSyncInProgress = false;
            emit workoutSyncInProgressChanged();

            if (!listOk) {
                emit errorOccurred(tr("Failed to sync workouts: %1").arg(listError));
                return;
            }

            for (const Workout &w : workouts) {
                QString storeError;
                if (!m_workoutStore->upsert(w, &storeError)) {
                    emit errorOccurred(tr("Failed to save workout: %1").arg(storeError));
                    return;
                }
            }

            // currentSecsSinceEpoch() is Qt 5.8+ - newer than Sailfish OS's
            // Qt5 (same vintage issue as QRandomGenerator elsewhere in this
            // project); currentMSecsSinceEpoch() has been available since
            // Qt 4.7 and works everywhere.
            m_cloudAccount.lastSync = QDateTime::currentMSecsSinceEpoch() / 1000;
            QString saveError;
            if (!m_cloudAccountStore->save(m_cloudAccount, &saveError))
                emit errorOccurred(tr("Failed to save account: %1").arg(saveError));

            loadCachedWorkouts();
        });
    });
}

void AppController::syncWatchWorkouts()
{
    if (!m_whiteboardReady || m_workoutSyncInProgress || m_logbookTestInFlight)
        return;

    m_workoutSyncInProgress = true;
    emit workoutSyncInProgressChanged();

    m_whiteboardClient->fetchLogEntries(QStringLiteral("/Logbook/Entries"),
            [this](bool ok, const std::vector<LogEntries::Entry> &entries, const QString &error) {
        if (!ok) {
            m_workoutSyncInProgress = false;
            emit workoutSyncInProgressChanged();
            emit errorOccurred(tr("Failed to list watch entries: %1").arg(error));
            return;
        }

        QVector<QString> logbookIds;
        logbookIds.reserve(static_cast<int>(entries.size()));
        for (const LogEntries::Entry &entry : entries)
            logbookIds.append(QString::number(entry.id));

        fetchWatchEntryAt(logbookIds, 0, 0, 0);
    });
}

void AppController::fetchWatchEntryAt(const QVector<QString> &logbookIds, int index,
                                       int succeeded, int failed)
{
    if (index >= logbookIds.size()) {
        m_workoutSyncInProgress = false;
        emit workoutSyncInProgressChanged();
        loadCachedWorkouts();
        if (failed > 0) {
            emit errorOccurred(tr("Synced %1 of %2 watch workouts (%3 failed)")
                                        .arg(succeeded)
                                        .arg(logbookIds.size())
                                        .arg(failed));
        }
        return;
    }

    const QString logbookId = logbookIds.at(index);
    const QString path = QStringLiteral("/Logbook/byId/%1/Data").arg(logbookId);
    m_whiteboardClient->fetchLogbookData(path,
            [this, logbookIds, index, succeeded, failed, logbookId]
            (bool ok, const std::vector<uint8_t> &data, const QString &error) mutable {
        Q_UNUSED(error);
        if (ok) {
            try {
                const Logbook::DecodedWorkout decoded = Logbook::decode(data);
                const Workout w = workoutFromDecoded(logbookId, decoded);
                QString storeError;
                if (m_workoutStore->upsert(w, &storeError))
                    ++succeeded;
                else
                    ++failed;
            } catch (const std::exception &) {
                ++failed;
            }
        } else {
            ++failed;
        }
        fetchWatchEntryAt(logbookIds, index + 1, succeeded, failed);
    });
}
