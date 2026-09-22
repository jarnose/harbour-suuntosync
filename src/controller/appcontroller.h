#pragma once

#include "../cloud/accountmeta.h"
#include "../ble/bluezadapter.h"
#include "../ble/pairedwatch.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

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

    // Phase 6: fetches and fully decodes one real workout directly from the
    // watch by its logbook id (the numeric suffix of
    // /Logbook/byId/<id>/Data - which is itself that workout's Unix start
    // timestamp in seconds, confirmed in docs/logbook-data-format.md, so
    // any id already seen via cloud sync or testWhiteboard()'s /Entries
    // probe works here), and - on success - saves it into WorkoutStore
    // (key "ble_<id>", source "ble", distinct from any cloud-synced record
    // of the same real workout - see workoutFromDecoded()'s comment in the
    // .cpp for why they're not merged) and refreshes workoutModel so it
    // shows up on MainPage immediately. Exercises the whole pipeline end to
    // end - MdsWhiteboardClient::fetchLogbookData()'s still-experimental
    // bulk-transfer trigger (see its doc comment) and Logbook::decode() -
    // and reports either a human-readable field summary or the failure via
    // logbookTestResult(). Still named/kept as "test..." since manual id
    // entry (no on-device /Entries listing UI yet) makes this a developer
    // probe more than a real end-user sync action for now, even though it
    // does persist.
    Q_INVOKABLE void testLogbookFetch(const QString &logbookId);

    // Listing a watch's workouts without typing in a known id by hand (see
    // docs/logbook-data-format.md's "Third decompilation pass" section).
    // The first attempt at this reused /Data's TYPE=0x10 stream-trigger
    // shortcut directly against /Entries and was confirmed NOT to work on
    // real hardware (a clean timeout) - /Entries turned out to need a
    // TYPE=0x0D handle-fetch request instead, built from decompiling
    // libmds.so's whiteboard::protocol_v9::StructureDeserializer code and
    // re-deriving the real captured request/response byte-for-byte (see
    // MdsWhiteboardClient::fetchLogEntries(),
    // Mds::encodeEntriesFetchTrigger(), LogEntries::decode()). Not yet run
    // on real hardware - reports either the decoded entry ids via
    // logbookTestResult() or the failure.
    Q_INVOKABLE void testEntriesFetch();

    // The real end-user "sync directly from the watch" action, now that
    // both halves are proven on real hardware:
    // MdsWhiteboardClient::fetchLogEntries() (see testEntriesFetch()) lists
    // the watch's own logbook entries, then each one is fetched and decoded
    // the same way testLogbookFetch() does a single entry - sequentially,
    // one at a time (not fired concurrently), since fetchLogbookData()
    // refuses a second bulk fetch while one's still active. Uses the same
    // workoutSyncInProgress property as syncCloudWorkouts() (the two share
    // one "a sync is running" concept, and mutually exclude each other - see
    // the .cpp) and refreshes workoutModel once at the end rather than
    // per-entry. A single entry failing to fetch/decode doesn't abort the
    // rest; the final tally is reported via errorOccurred() only if at
    // least one entry failed, so a fully successful sync stays silent like
    // syncCloudWorkouts() does.
    Q_INVOKABLE void syncWatchWorkouts();

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
    void logbookTestResult(const QString &summary);
    void workoutSyncInProgressChanged();

private:
    void onDeviceUpdated(const BluezAdapter::Device &device);
    void onConnectFinished(const QString &objectPath, bool ok, const QString &error);
    // The sequential per-entry loop behind syncWatchWorkouts() - fetches
    // logbookIds[index], then recurses to index+1 (or finishes at the end),
    // tallying succeeded and collecting one human-readable line per failure
    // (id + the actual fetch/decode/save error) as it goes, so a partial
    // sync's errorOccurred() message says *what* went wrong per entry, not
    // just how many failed. A plain index/counts recursion rather than an
    // index member variable since only one such loop can ever be running at
    // a time anyway (guarded by workoutSyncInProgress).
    //
    // Reconnects the watch (see reconnectWatch()) before every entry,
    // including the first - real-hardware testing 2026-09-22 found the
    // /Data shortcut (Mds::encodeStreamStartTrigger()) only works for the
    // first "Data" fetch on a given BLE connection; a second consecutive
    // fetch on the same connection times out with no bulk data at all, but
    // the exact same logbook id succeeds when fetched right after a fresh
    // reconnect. Not a real fix (the shortcut's trigger derivation is
    // presumably only valid for whichever handle the watch happens to
    // assign first per connection - see docs/logbook-data-format.md) but a
    // confirmed-working, if slow, way to sync more than one entry until
    // that's understood properly.
    void fetchWatchEntryAt(const QVector<QString> &logbookIds, int index, int succeeded,
                            const QStringList &failures);
    // Disconnects and reconnects the paired watch, then waits (polling, see
    // the .cpp) for whiteboardReady to come back up before calling back -
    // ok=false if reconnecting or re-establishing the Whiteboard channel
    // didn't happen within a bounded timeout. BluezAdapter::
    // disconnectFromDevice() has no completion signal of its own (see its
    // header), so this waits out a fixed settle delay before asking BlueZ
    // to reconnect rather than racing the two.
    void reconnectWatch(std::function<void(bool ok)> callback);
    void pollForWhiteboardReady(int elapsedMs, std::function<void(bool ok)> callback);

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
    // Same "no overlapping request" reasoning as m_whiteboardTestInFlight,
    // and doubly warranted here: MdsWhiteboardClient::fetchLogbookData()
    // refuses a second bulk fetch outright while one's active rather than
    // queueing it (see that method's own doc comment), so a second overlap
    // would surface as a confusing "Another bulk fetch is already in
    // progress" error rather than actually retrying.
    bool m_logbookTestInFlight = false;

    WorkoutStore *m_workoutStore;
    WorkoutListModel *m_workoutModel;
    bool m_workoutSyncInProgress = false;
};
