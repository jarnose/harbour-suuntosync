#pragma once

#include "../cloud/accountmeta.h"
#include "../ble/bluezadapter.h"
#include "../ble/pairedwatch.h"

#include <QObject>
#include <QString>

class TokenVault;
class CloudAccountStore;
class PairedWatchStore;
class DeviceListModel;
class MdsWhiteboardClient;
class SuuntoCloudClient;
class WorkoutStore;
class WorkoutListModel;

// QML-facing facade. Phase 6: once BluezAdapter reports a connected watch,
// attaches MdsWhiteboardClient to it and can run real Whiteboard GET
// requests (see testWhiteboard() - a deliberately minimal probe, ahead of
// the full LogbookSync/WorkoutStore pipeline, so the GATT
// characteristic-discovery/write/notify plumbing gets validated on a real
// device on its own first). See ~/.claude/plans/agile-hopping-harp.md.
class AppController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString appName READ appName CONSTANT)
    Q_PROPERTY(bool cloudSignedIn READ isCloudSignedIn NOTIFY cloudAccountChanged)
    Q_PROPERTY(QString cloudEmail READ cloudEmail NOTIFY cloudAccountChanged)
    Q_PROPERTY(QObject *deviceModel READ deviceModelObject CONSTANT)
    Q_PROPERTY(bool watchPaired READ isWatchPaired NOTIFY pairedWatchChanged)
    Q_PROPERTY(QString pairedWatchName READ pairedWatchName NOTIFY pairedWatchChanged)
    Q_PROPERTY(bool watchConnected READ isWatchConnected NOTIFY watchConnectedChanged)
    Q_PROPERTY(bool whiteboardReady READ isWhiteboardReady NOTIFY whiteboardReadyChanged)
    Q_PROPERTY(bool cloudLoginInProgress READ isCloudLoginInProgress NOTIFY cloudLoginInProgressChanged)
    Q_PROPERTY(QObject *workoutModel READ workoutModelObject CONSTANT)
    Q_PROPERTY(bool workoutSyncInProgress READ isWorkoutSyncInProgress NOTIFY workoutSyncInProgressChanged)

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    QString appName() const { return QStringLiteral("Suunto Sync"); }
    bool isCloudSignedIn() const { return m_cloudAccount.isSignedIn(); }
    QString cloudEmail() const { return m_cloudAccount.email; }
    bool isCloudLoginInProgress() const { return m_cloudLoginInProgress; }
    // Defined in the .cpp - see deviceModelObject()'s comment, same reason.
    QObject *workoutModelObject() const;
    bool isWorkoutSyncInProgress() const { return m_workoutSyncInProgress; }

    // Defined in the .cpp file, not inline here: DeviceListModel is only
    // forward-declared in this header, so the implicit DeviceListModel* ->
    // QObject* upcast needs the complete type, which only the .cpp
    // (including devicelistmodel.h) has.
    QObject *deviceModelObject() const;
    bool isWatchPaired() const { return m_pairedWatch.isValid(); }
    QString pairedWatchName() const { return m_pairedWatch.name; }
    bool isWatchConnected() const { return m_watchConnected; }
    bool isWhiteboardReady() const { return m_whiteboardReady; }

    // Populates deviceModel with devices already known to BlueZ (bonded in
    // Settings > Bluetooth, or seen in an earlier scan this boot).
    Q_INVOKABLE void refreshDevices();
    Q_INVOKABLE void startScan();
    Q_INVOKABLE void stopScan();
    // Adopts the given (already BlueZ-paired) device as "the" watch and
    // connects to it. objectPath must be one DeviceListModel just reported.
    Q_INVOKABLE void selectWatch(const QString &objectPath, const QString &address,
                                  const QString &name);
    Q_INVOKABLE void forgetWatch();

    // Phase 6 validation probe: issues a real "GET /Logbook/Entries"
    // Whiteboard request over the connected watch's GATT link and reports
    // the raw result via whiteboardTestResult() - nothing is parsed or
    // stored yet (that's LogbookSync, once this plumbing is proven).
    Q_INVOKABLE void testWhiteboard();

    // Signs in to the Suunto cloud account. Progress/result surface via
    // cloudLoginInProgress and cloudAccountChanged (success) /
    // errorOccurred (failure) - no separate "login result" signal, since
    // QML's LoginPage only needs to know when it's safe to pop back to
    // MainPage (cloudSignedIn flipping true) or show an error.
    Q_INVOKABLE void loginToCloud(const QString &email, const QString &password);
    Q_INVOKABLE void logoutFromCloud();

    // Loads whatever's cached in WorkoutStore (call on page open) and,
    // separately, fetches a fresh page from the cloud and re-populates the
    // store/model with it (call from pull-to-refresh). Both are silent
    // no-ops (no errorOccurred) if not signed in - MainPage's placeholder
    // text already explains that, no need to also toast it.
    Q_INVOKABLE void loadCachedWorkouts();
    Q_INVOKABLE void syncCloudWorkouts();

signals:
    void errorOccurred(const QString &message);
    void cloudAccountChanged();
    void cloudLoginInProgressChanged();
    void pairedWatchChanged();
    void watchConnectedChanged();
    void whiteboardReadyChanged();
    void whiteboardTestResult(const QString &summary);
    void workoutSyncInProgressChanged();

private:
    void onDeviceUpdated(const BluezAdapter::Device &device);
    void onConnectFinished(const QString &objectPath, bool ok, const QString &error);

    TokenVault *m_tokenVault;
    CloudAccountStore *m_cloudAccountStore;
    CloudAccount m_cloudAccount;
    SuuntoCloudClient *m_cloudClient;
    bool m_cloudLoginInProgress = false;

    BluezAdapter *m_bluezAdapter;
    DeviceListModel *m_deviceModel;
    PairedWatchStore *m_pairedWatchStore;
    PairedWatch m_pairedWatch;
    bool m_watchConnected = false;

    MdsWhiteboardClient *m_whiteboardClient;
    bool m_whiteboardReady = false;
    // Guards against testWhiteboard() firing a second overlapping request -
    // a real capture showed two byte-identical GET requests (same request
    // ID) going out under 1ms apart from a single tap, almost certainly a
    // QML double-invocation rather than a deliberate second request. Two
    // requests racing each other isn't just wasteful: the watch's own
    // receive-side reassembly plausibly doesn't tolerate two independently-
    // chunked writes interleaving into one corrupted buffer, which is the
    // leading suspect for why the watch never responded to either.
    bool m_whiteboardTestInFlight = false;

    WorkoutStore *m_workoutStore;
    WorkoutListModel *m_workoutModel;
    bool m_workoutSyncInProgress = false;
};
