#include "appcontroller.h"
#include "../secrets/tokenvault.h"
#include "../cloud/cloudaccountstore.h"
#include "../ble/pairedwatchstore.h"
#include "../ble/devicelistmodel.h"
#include "../ble/mdswhiteboardclient.h"
#include "../cloud/suuntocloudclient.h"

#include <QStandardPaths>
#include <QDir>

namespace {

QString dbPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("suuntosync.sqlite"));
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
{
    QString error;
    if (!m_cloudAccountStore->open(&error)) {
        emit errorOccurred(tr("Failed to open database: %1").arg(error));
    } else {
        m_cloudAccount = m_cloudAccountStore->load(&error);
        if (!error.isEmpty())
            emit errorOccurred(tr("Failed to load account: %1").arg(error));
    }

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
