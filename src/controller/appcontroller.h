#pragma once

#include "../cloud/accountmeta.h"
#include "../ble/bluezadapter.h"
#include "../ble/pairedwatch.h"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QStringList>
#include <QVector>

class TokenVault;
class CloudAccountStore;
class PairedWatchStore;
class DeviceListModel;
class MdsWhiteboardClient;
class SuuntoCloudClient;
class WorkoutStore;
class WorkoutListModel;
class HealthStore;

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
    Q_PROPERTY(bool cloudSamplesInProgress READ isCloudSamplesInProgress NOTIFY cloudSamplesInProgressChanged)
    Q_PROPERTY(bool healthSyncInProgress READ isHealthSyncInProgress NOTIFY healthSyncInProgressChanged)

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
    bool isCloudSamplesInProgress() const { return m_cloudSamplesInProgress; }
    bool isHealthSyncInProgress() const { return m_healthSyncInProgress; }

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
    // Mds::encodeEntriesFetchTrigger(), LogEntries::decode()). Confirmed
    // working on real hardware 2026-09-22 - reports either the decoded
    // entry ids via logbookTestResult() or the failure.
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

    // A BLE-synced workout's GPS track, projected for drawing: a list of
    // { "x": 0..1, "y": 0..1 } points, already aspect-corrected (longitude
    // degrees are scaled by cos(latitude), so the shape isn't stretched) and
    // fitted to the unit square with y pointing down, ready to multiply by a
    // canvas size. Empty for a cloud workout or one synced before routes
    // were stored. Projection is a plain equirectangular one - fine over a
    // workout-sized area, not a map projection.
    Q_INVOKABLE QVariantList workoutRoute(const QString &key) const;

    // Every field the watch recorded for a BLE-synced workout beyond the
    // dozen with a column of their own - zone durations, running dynamics,
    // device and settings, and the rest of the hundred-plus in
    // docs/sbem-chunk-map.md. A list of { name, value, unit }, sorted by
    // name, with unrecorded (zero) fields left out. Empty for a cloud
    // workout or one synced before this existed.
    Q_INVOKABLE QVariantList workoutDetails(const QString &key) const;

    // Fetches a cloud workout's extensions - the analysis the list response
    // doesn't carry - and merges them into workoutDetails(). Lazy and
    // one-shot: called when a workout is opened, does nothing for a BLE
    // workout or one already fetched, and reports completion via
    // workoutDetailsChanged() rather than blocking the page.
    Q_INVOKABLE void loadCloudDetails(const QString &key);

    // Chartable per-sample series for a BLE-synced workout: a list of
    // { name, unit, min, max, points }, where points is already reduced to
    // a fixed number of averaged buckets ready to draw. Empty for a cloud
    // workout, and for a workout with nothing worth charting.
    Q_INVOKABLE QVariantList workoutSeries(const QString &key) const;

    // Fetches a cloud workout's sample data (GET /v1/workouts/{key}/sml) and
    // turns it into the same stored series a BLE workout gets. Unlike
    // loadCloudDetails() this is *not* automatic: the response runs to
    // several megabytes, so it hangs off an explicit menu action and
    // cloudSamplesInProgress drives the page's busy state.
    //
    // The response's shape hasn't been captured, so the parser recognises
    // the schema's leaf names wherever they turn up rather than assuming a
    // nesting. If it finds nothing, the raw body is written to
    // <cache>/sml-<key>.json and errorOccurred() names the file, so it can
    // be pulled off the phone and the parser corrected against the real
    // thing instead of another guess.
    Q_INVOKABLE void loadCloudSamples(const QString &key);

    // The watch's round-the-clock data from the cloud: sleep, sleep stages,
    // recovery and activity (see SuuntoCloudClient::fetchHealthEntries()).
    // Fetches all four, newest-first, asking only for what is newer than
    // what's already stored - so the first sync on an old account pulls
    // years and every one after it pulls a day.
    //
    // This reads what the *watch already uploaded*; it does not reach the
    // watch itself. If a night is missing here it is missing in the cloud,
    // which is exactly the case that prompted this: the sleep for
    // 2026-09-21 only reached the cloud when the watch was finally synced.
    Q_INVOKABLE void syncHealthData();

    // Stored entries for one kind, newest first, as
    // { timestamp, <the entryData fields, flattened> } - the payload's own
    // field names are passed through untouched rather than mapped, since
    // they are the watch's and this project has no better names for them.
    Q_INVOKABLE QVariantList healthEntries(const QString &kind, int limit) const;

    // One-line summary per kind for the page header: the newest entry's
    // timestamp and how many are stored. Empty map for a kind with none.
    Q_INVOKABLE QVariantMap healthOverview(const QString &kind) const;

    // Uploads a watch-synced workout to the Suunto cloud (item 2 of the
    // 2026-09-22 list). The payload was built at sync time and stored (see
    // WorkoutStore::saveSml()), so this is just the POST.
    //
    // Nothing about this is automatic: a watch sync stores the payload but
    // does not send it, because publishing a workout to an account is the
    // user's call, not a side effect of looking at it.
    Q_INVOKABLE void uploadWorkoutToCloud(const QString &key);
    // Whether the action is worth offering: a stored payload exists, the
    // cloud hasn't already taken it, and we're signed in.
    Q_INVOKABLE bool canUploadWorkout(const QString &key) const;

    // A BLE-synced workout's laps: { number, type, durationSeconds,
    // distanceMeters }, where type is the watch's own reason for the marker
    // (manual, auto-lap by distance, interval...). Empty when the workout
    // has no lap markers, which is the common case for a plain outing.
    Q_INVOKABLE QVariantList workoutLaps(const QString &key) const;

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
    void workoutDetailsChanged(const QString &key);
    void cloudSamplesInProgressChanged();
    void healthSyncInProgressChanged();
    void healthDataChanged();
    void workoutUploaded(const QString &key, bool ok, const QString &message);

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
    void fetchWatchEntryAt(const QVector<QString> &logbookIds, int index, int succeeded,
                            const QStringList &failures);

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
    bool m_cloudSamplesInProgress = false;

    // Guards against a second tap while a POST is in flight - the server
    // has no idempotency key, so a double send would create two workouts.
    bool m_uploadInProgress = false;

    HealthStore *m_healthStore;
    bool m_healthSyncInProgress = false;
    // The sequential per-kind loop behind syncHealthData(), same shape as
    // fetchWatchEntryAt(): four requests one after another rather than four
    // at once, so a failure can name which kind failed and the tally stays
    // simple.
    void fetchHealthKindAt(int index, int fetched, const QStringList &failures);
};
