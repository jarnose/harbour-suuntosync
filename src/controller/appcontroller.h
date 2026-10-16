#pragma once

#include "../ble/sbemlayout.h"
#include "../cloud/accountmeta.h"
#include "../ble/bluezadapter.h"
#include "../ble/pairedwatch.h"
#include "../notify/watchlink.h"

#include <QSet>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>

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
    // What the cover shows. Stored in QSettings; see coverMode().
    // Whether to draw a base map under a workout's track, and where its
    // tiles come from. Off by default: it is somebody else's bandwidth, and
    // a map is a want rather than a need. See docs/base-map.md.
    Q_PROPERTY(bool baseMapEnabled READ baseMapEnabled WRITE setBaseMapEnabled NOTIFY baseMapChanged)
    Q_PROPERTY(QString mapTileUrl READ mapTileUrl WRITE setMapTileUrl NOTIFY baseMapChanged)
    Q_PROPERTY(QString defaultMapTileUrl READ defaultMapTileUrl CONSTANT)
    Q_PROPERTY(bool recordsSyncInProgress READ isRecordsSyncInProgress NOTIFY recordsSyncInProgressChanged)
    Q_PROPERTY(bool suuntoPlusBusy READ isSuuntoPlusBusy NOTIFY suuntoPlusBusyChanged)
    Q_PROPERTY(bool storeBusy READ isStoreBusy NOTIFY storeBusyChanged)
    // Unix ms of the cached records' own fetch, 0 if never. The page shows
    // it, because an all-time best that is a month stale is still right and
    // saying when it was asked for beats implying it is live.
    Q_PROPERTY(qint64 recordsFetchedAt READ recordsFetchedAt NOTIFY personalRecordsChanged)
    Q_PROPERTY(QString coverMode READ coverMode WRITE setCoverMode NOTIFY coverModeChanged)
    Q_PROPERTY(bool syncOnConnect READ syncOnConnect WRITE setSyncOnConnect NOTIFY syncOnConnectChanged)
    Q_PROPERTY(bool gpsUpdateInProgress READ isGpsUpdateInProgress NOTIFY gpsUpdateInProgressChanged)

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
    bool isGpsUpdateInProgress() const { return m_gpsUpdateInProgress; }
    bool baseMapEnabled() const { return m_baseMapEnabled; }
    void setBaseMapEnabled(bool enabled);
    // Either a {z}/{x}/{y} tile template or a TileJSON endpoint - see
    // MapTileSource, which decides by looking for "{z}".
    QString mapTileUrl() const { return m_mapTileUrl; }
    void setMapTileUrl(const QString &url);
    QString defaultMapTileUrl() const;

    bool isRecordsSyncInProgress() const { return m_recordsSyncInProgress; }
    // A held Whiteboard session. The watch is attached while at least one
    // of these is alive and let go as soon as the last one dies, so an
    // operation holds the watch for exactly as long as its own chain of
    // requests - and the notification daemon, which needs the watch for
    // about a second per notification, gets it the rest of the time.
    //
    // It is a shared_ptr rather than a flag because every operation here is
    // an asynchronous chain: carrying the session through the callbacks
    // means the release cannot be forgotten on one of the exit paths, and
    // forgetting it would block notifications for as long as the app ran -
    // which is exactly the behaviour this replaces.
    using WatchSession = std::shared_ptr<void>;

    bool isSuuntoPlusBusy() const { return m_suuntoPlusBusy; }
    bool isStoreBusy() const { return m_storeBusy; }
    qint64 recordsFetchedAt() const;

    // One of "latest", "totals", "progress", "sleep" or "nothing" - what
    // the cover page draws. A plain string rather than an enum so QML can
    // pass it straight through to a ComboBox without a registered type,
    // and so a mode added later cannot reinterpret a stored index: a value
    // this build does not know simply draws the empty state.
    QString coverMode() const { return m_coverMode; }
    void setCoverMode(const QString &mode);

    // Whether connecting to the watch should start a sync by itself.
    // Default off: a sync is minutes of radio time, and doing it because
    // the watch happened to come into range is not obviously what someone
    // wants.
    bool syncOnConnect() const { return m_syncOnConnect; }
    void setSyncOnConnect(bool enabled);

    // Summary for the cover, so CoverPage doesn't reach into models: the
    // newest workout's name/date/distance, lifetime totals, and last
    // night's sleep. Empty values where there is nothing to show.
    Q_INVOKABLE QVariantMap coverSummary() const;

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

    // Every watch that has been paired here, most recent first, as
    // {address, objectPath, name, active} for a menu. Changes whenever
    // pairedWatchChanged() fires, which is what QML should listen to.
    Q_INVOKABLE QVariantList knownWatches() const;

    // Makes a remembered watch the active one and connects to it, without
    // going back through the pairing page. A no-op if that address is
    // already active, so re-picking the current entry from a menu does not
    // drop a working connection.
    Q_INVOKABLE void switchToWatch(const QString &address);

    // Phase 6 validation probe: issues a real "GET /Logbook/Entries"
    // Whiteboard request over the connected watch's GATT link and reports
    // the raw result via whiteboardTestResult() - nothing is parsed or
    // stored yet (that's LogbookSync, once this plumbing is proven).
    Q_INVOKABLE void testWhiteboard();
    // Reads the watch's capability list and reports it as the cloud's own
    // parameter would spell it. A probe, because nothing in the SuuntoPlus
    // store can be asked for before this works, and because the whole
    // decode was derived from a capture rather than tried on hardware -
    // see docs/suuntoplus-and-watchfaces.md.
    Q_INVOKABLE void testCapabilities();
    // What the watch says is installed. Reports pluginIds, which is what
    // the watch stores - mapping them to catalogue ids needs the cloud's
    // listing. A probe for the same reason the one above is.
    Q_INVOKABLE void testPluginList();

    // Ties the two together: reads the watch's capabilities and the variant
    // inside them, asks the cloud for this watch's library, reads what the
    // watch has installed, and reports the join. The last probe before any
    // of this gets a UI - it exercises every piece a catalogue page would
    // need, and nothing it does writes anything.
    Q_INVOKABLE void testSuuntoPlusLibrary();

    // Downloads one library item's zip and reports what came back. Reads
    // only: nothing is written to the watch. zzaero01 (ZoneSense) is the
    // target because it is the one item in this account's library that the
    // watch does not have, which makes it the install step's natural first
    // subject - and the capture says its zip is 17784 bytes, so the answer
    // is checkable.
    Q_INVOKABLE void testDownloadPlugin();

    // **The first thing here that writes to the watch.** Installs
    // ZoneSense: looks it up in the library for its pluginId and type,
    // downloads the zip and streams it over, then commits. The file is named
    // by pluginId for a sports app and by catalogue id for a watch face,
    // which is what the capture shows and not a choice.
    Q_INVOKABLE void testInstallPlugin();

    // Lists b:/zapp on the watch. The only honest answer to "did the
    // install work": /Plugin/List does not enumerate everything that is
    // there, and a feature showing up in the watch's own menu may be built
    // into the firmware.
    Q_INVOKABLE void testListZappDirectory();

    // Deletes ZoneSense's installed file and lists the directory to show
    // that it is gone. The point is the experiment that follows: install it
    // again with this app and see whether the file comes back, which is the
    // only way left to tell our install from the official app's earlier
    // push of the same package. Reversible - it is in the account's library.
    //
    // The only probe here that removes anything from the watch.
    Q_INVOKABLE void testDeleteZoneSense();

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

    // Probe for item 3 of the 2026-09-22 list: can the watch's own sleep
    // and daily-activity data be read over BLE, rather than only from the
    // cloud after the watch has uploaded it?
    //
    // Uses the mechanism the 2026-09-23 capture revealed: the watch
    // renders a timeline file and we page it off its filesystem. The first
    // version of this probe asked for "/Sleep/<serial>/Entries", which the
    // capture showed does not exist on the wire at all - that path is an
    // MDS-library abstraction on the Android side. Still a hex dump rather
    // than a decoder: the payload is SBEM0102, and this project has only
    // ever parsed 0103.
    //
    // `kind` is "Sleep" or "Activity".
    Q_INVOKABLE void testHealthResourceFetch(const QString &kind);

    // Probe for daily activity, which is the last health series still
    // missing. docs/watch-push-resources.md previously wrote /Activity/
    // TrendData off as "a different structure, no unix timestamps in it";
    // libmds.so says otherwise, and says exactly what it is - see that
    // doc's 2026-09-25 section. In short: SDS::Activity::getFragment()
    // builds "/net/<serial>/Activity/TrendData", passes one parameter
    // named "timestamp", and parses an array of entries carrying
    // Timestamp, Steps and Energy.
    //
    // What the disassembly does NOT say is the parameter's width or unit
    // on the wire - the JSON layer hides both, and this project has
    // already been caught once by a cursor that was seconds where the
    // neighbouring resource used milliseconds. So the probe tries all four
    // combinations in turn and stops at the first the watch doesn't
    // reject, which settles it in one build instead of four.
    Q_INVOKABLE void testActivityTrendFetch();

    // Reads the connected watch's own field table off
    // /Logbook/byId/<id>/Descriptors and saves it, because the compiled-in
    // one (src/ble/sbemdescriptors.h, generated from a Suunto Race) turns
    // out to be per-model.
    //
    // A Suunto 9 Baro transfers a workout perfectly - same Whiteboard
    // protocol, same Heatshrink, same SBEM0103 container, parses into
    // thousands of clean chunks - and then every field decodes to zero,
    // because its chunk ids are not the Race's. The Race puts GPS in
    // 0x0c and heart rate in 0x12; the 9 Baro's payload has no 0x0c or
    // 0x12 at all, and its summary uses 0x1e/0x20 where the Race uses
    // 0x1b/0x1d/0x158-0x167. Same protocol, different vocabulary.
    //
    // No new protocol code: /Descriptors is paged exactly like /Summary
    // (see Mds::encodePagedReadRequest()), so this is fetchSummary() with
    // a different path.
    Q_INVOKABLE void testDescriptorsFetch(const QString &logbookId);

    // Sends one plain GET to any Whiteboard path and reports what came
    // back. A resource this watch does not have answers with the six-byte
    // f5 error rather than a timeout, so "does this model have that
    // resource at all" is a question with a two-second answer - which is
    // otherwise an evening with adb, a capture and tshark.
    //
    // Read-only by construction: a GET resolves a handle, and nothing here
    // follows it with the PUT that would write anything.
    Q_INVOKABLE void probePath(const QString &path);

    // Sends one notification to the watch, and reports what it said.
    //
    // The whole encoder was built against three captured requests and has
    // never been sent to a watch; this is the gate for that. It writes to
    // the watch's notification queue, which is not read-only the way
    // probePath() is - so it is a deliberate button with typed-in text
    // rather than anything automatic.
    //
    // The notification is filed under this app's own id, so sending it
    // twice replaces rather than stacks, and `categoryId` is whatever was
    // asked for: 0 files it the way a calendar alert was filed, 6 the way
    // a text message is (see docs/notifications.md for the whole table).
    Q_INVOKABLE void testNotification(const QString &title, const QString &message,
                                       int categoryId);

    // Takes that notification back off the watch. Answers "no notification
    // with that id" if it was already dismissed there.
    Q_INVOKABLE void testNotificationRemove();

    // Reads and writes /Settings/Ble/AncsEnabled on the watch - the switch
    // the official app flips when you turn notifications on for a watch.
    //
    // This is why a 9 Baro answered a perfectly good notification with 400:
    // the setting read 0 on the Baro and 1 on the Race in captures taken the
    // same evening, and the watch's own menu does not appear to be the same
    // switch. The resource exists on both watches either way, so probing it
    // said nothing; only the value did.
    //
    // The answer arrives as watchNotificationsRead(); writing reports
    // through logbookTestResult() like the other watch-side actions.
    Q_INVOKABLE void readWatchNotificationsEnabled();
    Q_INVOKABLE void setWatchNotificationsEnabled(bool enabled);

    // Refreshes the watch's GPS assist data, which is what makes it find
    // satellites in seconds instead of minutes. Asks the watch which of
    // four formats it wants, downloads that one, and writes it across in
    // 453-byte chunks (docs/watch-push-resources.md).
    //
    // The one feature here that needs no Suunto account: the assist-data
    // endpoints ignore the appkey the official app sends, so nothing in
    // this path touches a session or a login.
    //
    // Confirmed working on a Suunto 9 Baro, which has no WiFi of its own
    // and so has no other way to get this. A Race answers the same
    // resources but has never been given a blob by anything - it normally
    // fetches its own over WiFi - so that case is genuinely untried.
    Q_INVOKABLE void updateWatchGps();

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
    // Incremental: asks the cloud only for what reached it since the last
    // successful sync, which is what the official app does (measured, not
    // assumed - see SuuntoCloudClient::listWorkouts). A decade of history
    // is twelve pages, and re-fetching all of it to learn about one new
    // walk was the reason a routine sync took as long as it did.
    Q_INVOKABLE void syncCloudWorkouts();
    // The same thing from the beginning of time, for when the local copy is
    // damaged rather than merely out of date - restored from elsewhere, or
    // interrupted part way through its first run. Deliberately not for an
    // edit made in the official app: `since` matches `lastModified`, so an
    // ordinary sync already brings those back (measured - see
    // SuuntoCloudClient::listWorkouts). Separate because it is slow enough
    // that it should be the user's choice, not something that happens on
    // every pull-to-refresh.
    Q_INVOKABLE void resyncCloudWorkouts();

    // The account's personal bests, per activity: all-time and, separately,
    // this year. Cached in the database, so personalRecords() answers
    // without a network and syncPersonalRecords() is what goes and asks.
    // Each row carries its value verbatim with a `type` name; the page
    // decides how to show it, because the cloud's units are only partly
    // established - distances and durations yes, speeds and pace no. See
    // docs/workout-upload.md.
    Q_INVOKABLE QVariantList personalRecords() const;

    // The account's SuuntoPlus library, each entry carrying whether the
    // watch has it. Read from what the last refresh left in memory, so the
    // page has something to draw before any request finishes.
    //
    // "installed" comes from the watch's *directory*, not from
    // /Plugin/List: that resource lists less than what is there - fifteen
    // names against seventeen files on this watch - and the official app's
    // own pushes are missing from it too. See
    // docs/suuntoplus-and-watchfaces.md.
    //
    // `type` is the cloud's own word for the kind - "watchface", "feature"
    // or "sequence" - and the list arrives grouped by it, watch faces then
    // sports apps then guides, so the page can head each group where the
    // kind changes.
    Q_INVOKABLE QVariantList suuntoPlusItems() const;

    // Reads the watch's capabilities, asks the cloud for this watch's
    // library - two listings, because guides are their own endpoint - and
    // lists the watch's plugin directory. Needs both the watch and an
    // account: the capability string decides what the cloud offers.
    Q_INVOKABLE void refreshSuuntoPlus();

    // Downloads one library item and installs it. `id` is the catalogue id.
    Q_INVOKABLE void installSuuntoPlusItem(const QString &id);

    // Deletes the installed file. The item stays in the library, so this is
    // undone by installing it again.
    Q_INVOKABLE void removeSuuntoPlusItem(const QString &id);

    // Takes the item out of the account's library, from the library page
    // rather than the store - the two pages reach the same endpoint, but
    // only this one can name an item the store listing has never been
    // asked about.
    //
    // It does not touch the watch, so the page only offers it for an item
    // the watch does not have: otherwise the file would stay on the watch
    // while the row vanished from a page that lists the library, which is
    // the same blind spot the watch's own extra files have. The guard is in
    // the menu rather than here, because the watch's directory is what
    // decides it and the page already has that answer.
    Q_INVOKABLE void removeSuuntoPlusFromLibrary(const QString &id);

    // The store, in two parts: the categories, and one category's items.
    // Both answer from what the last refresh left in memory - the whole
    // catalogue arrives in a single request, so there is nothing to fetch
    // per category.
    //
    // A category row carries `id`, `title` and `count`. An item row carries
    // `id`, `type`, `title`, `subTitle`, `description`, `iconUrl`,
    // `bannerUrl`, `richText`, `owner`, `version`, `avgRating`, `reviews`
    // and `inLibrary` - everything either page shows, because the single
    // store request answers all of it and there is no second fetch.
    Q_INVOKABLE QVariantList storeCategories() const;
    Q_INVOKABLE QVariantList storeItems(const QString &categoryId) const;

    // One item, by id. For the item page, which re-reads after an add or a
    // removal rather than holding the snapshot it was pushed with.
    Q_INVOKABLE QVariantMap storeItem(const QString &id) const;

    // Fetches the catalogue. Needs the watch, because the store answers a
    // different catalogue per model, and an account.
    Q_INVOKABLE void refreshStore();

    // Adds the item to the account's library, or takes it out again. Adding
    // does not put anything on the watch: the item appears on the SuuntoPlus
    // page afterwards, and installing it from there is a separate step.
    Q_INVOKABLE void addStoreItem(const QString &id);
    Q_INVOKABLE void removeStoreItem(const QString &id);
    Q_INVOKABLE void syncPersonalRecords();

    // The stored track as real degrees, one { latitude, longitude } per point,
    // for the map item - it projects them itself against the tile grid it
    // chose, so a pre-fitted route is no use to it. Empty when the workout
    // has fewer than two points.
    Q_INVOKABLE QVariantList workoutTrack(const QString &key) const;

    // The centre of a workout's track in real degrees, { latitude, longitude },
    // or an empty map when it has no track. For handing the place to a map
    // application; see workoutCenter() for why the centre and not the start.
    Q_INVOKABLE QVariantMap workoutCenter(const QString &key) const;

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
    // `saveRaw` writes the response to <cache>/sml-<key>.json whether or not
    // the parser recognises it, which is the only way to get at a part of
    // the JSON this parser does not read - the laps, as it happens. Not the
    // default, because the responses run to megabytes and the cache is not
    // the place to keep one per workout anybody ever looked at.
    Q_INVOKABLE void loadCloudSamples(const QString &key, bool saveRaw = false);

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

    // The same sleep data, read straight off the watch instead of from the
    // cloud (item 3 of the 2026-09-22 list). Decodes the watch's timeline
    // file and stores it in exactly the shape the cloud sync produces, so
    // HealthPage shows it without knowing where it came from - and so a
    // night that reached the watch but never reached the cloud still shows
    // up, which is the case that started this.
    //
    // Entries are keyed by (kind, timestamp), so a night synced both ways
    // is stored once rather than twice.
    Q_INVOKABLE void syncWatchHealth();

    // Reads a FIT activity file and files it as a workout of its own, with
    // its route, charts and the laps the file itself states. `path` may be
    // a plain path or the file:// URL a FilePickerPage hands back.
    //
    // The key is "fit_<start time in seconds>", so importing the same file
    // twice overwrites rather than duplicates - and a cloud copy of the
    // same outing stays its own row, the way a watch-read one does.
    //
    // Synchronous: a five-hour ride is some 18 000 records and parses in
    // well under a second, and there is nothing to wait for on a radio.
    Q_INVOKABLE void importFitFile(const QString &path);

    // Writes a workout out to ~/Documents/SuuntoSync, and says where.
    //
    // What can be exported depends on where the workout came from, and the
    // page asks before offering it:
    //
    //  - the **FIT file** only for an import, which is the only kind this
    //    app has one of. A watch workout would need a FIT *encoder* and
    //    there is none here; the cloud has an exportFit endpoint for its
    //    own copies, which this project has never exercised.
    //  - the **SML zip** for anything with stored sources - a watch
    //    workout or an import - because that is what buildUploadZip()
    //    makes, so what lands on disk is byte for byte what an upload
    //    would send. That is most of why it is worth having.
    //
    // `label` is the display name the page already has for the activity;
    // the vocabulary lives in QML, so it comes from there rather than
    // being looked up again here.
    Q_INVOKABLE bool canExportFit(const QString &key) const;
    Q_INVOKABLE bool canExportSml(const QString &key) const;
    Q_INVOKABLE void exportWorkoutFit(const QString &key, const QString &label);
    Q_INVOKABLE void exportWorkoutSml(const QString &key, const QString &label);

    // Pushes what the watch gave us up to the Suunto cloud - the other
    // direction from syncHealthData(). Only rows read over BLE are ever
    // sent; anything that came from the cloud is already there.
    Q_INVOKABLE void uploadHealthToCloud();
    // How many rows are waiting, so the menu can say so rather than
    // offering an action that would do nothing.
    Q_INVOKABLE int pendingHealthUploads() const;

    // Stored entries for one kind, newest first, as
    // { timestamp, <the entryData fields, flattened> } - the payload's own
    // field names are passed through untouched rather than mapped, since
    // they are the watch's and this project has no better names for them.
    Q_INVOKABLE QVariantList healthEntries(const QString &kind, int limit) const;


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

    // True for a workout read off the watch that the cloud has not taken.
    // Those are the only ones worth offering to delete: a cloud workout
    // would come straight back on the next sync, and one the cloud has
    // taken is not this app's to throw away.
    Q_INVOKABLE bool canDeleteWorkout(const QString &key) const;

    // Deletes the phone's copy, and only the phone's copy. If the watch
    // still has the workout a sync brings it back; if the watch has pruned
    // it - which it does, and which is why three of these once had to be
    // repaired - this was the last copy of it anywhere. The caller is
    // expected to have asked.
    Q_INVOKABLE void deleteWorkout(const QString &key);

    // Uploads every watch-synced workout that the cloud hasn't taken yet,
    // oldest first. Sequential rather than parallel: the server has no
    // idempotency key, and a batch that half-succeeded would be harder to
    // reason about than one that stops at the first failure.
    Q_INVOKABLE void uploadAllWorkouts();
    // How many are waiting, so the menu can say so and hide itself when
    // there is nothing to do.
    Q_INVOKABLE int pendingWorkoutUploads() const;

    // A workout's laps, as stored. Always `number`, `type`,
    // `durationSeconds` and `distanceMeters`, where type is the reason the
    // lap ended (manual, auto-lap by distance, interval...). A cloud
    // workout's laps carry `avgSpeed`, `avgHeartRate`, `ascent` and
    // `energy` as well. A watch-read one carries the first two, derived
    // from its own samples - the marker events it builds laps from hold a
    // time and a type and nothing else - and deliberately not the other
    // two: energy is in no sample, and a per-lap ascent would carry the
    // whole of this project's altitude hysteresis error inside a few
    // hundred metres.
    //
    // Deliberately not a fixed list of fields: this returns whatever the
    // stored JSON holds, so a field added where the laps are built reaches
    // the page without a second edit here. See workoutLaps() itself.
    //
    // Empty when the workout has no lap markers, which is the common case
    // for a plain outing.
    Q_INVOKABLE QVariantList workoutLaps(const QString &key) const;

    // This calendar month's training, the way Suunto's own app puts it:
    // how many sessions and how long altogether. Returns `count`,
    // `seconds` and `month` (the local month's name).
    //
    // Deduplicated - see WorkoutSummary. The same outing can be in the
    // database twice, from the watch and from the cloud, and a month that
    // reads double is worse than no month at all.
    Q_INVOKABLE QVariantMap monthSummary() const;

    // Chronic and acute load and the balance between them - Suunto's
    // "Progress". Returns `ctl`, `atl`, `tsb` and `days`, the length of the
    // history they were computed over; a CTL with a fortnight behind it is
    // not worth showing, so the caller can decide.
    //
    // The stress figures are Suunto's own trainingStressScore, which its
    // cloud returns per workout. Nothing here derives a load from duration
    // or heart rate: a number that looked like Suunto's and was not would
    // be worse than no number. Watch-only workouts therefore contribute
    // nothing, because the watch's own TraingingLoadPeak comes back empty.
    Q_INVOKABLE QVariantMap trainingProgress() const;

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
    void gpsUpdateInProgressChanged();
    void healthDataChanged();
    void baseMapChanged();
    void recordsSyncInProgressChanged();
    void personalRecordsChanged();
    void suuntoPlusBusyChanged();
    void suuntoPlusChanged();
    void storeBusyChanged();
    void storeChanged();
    void coverModeChanged();
    void syncOnConnectChanged();
    // -1 when the watch could not be asked at all, otherwise its own value.
    // `detail` says why on -1, because a switch that is simply absent is
    // indistinguishable from a feature that was never built - which is how
    // this signal came to carry a reason.
    void watchNotificationsRead(int value, const QString &detail);
    void workoutDeleted(const QString &key);
    void workoutUploaded(const QString &key, bool ok, const QString &message);
    void workoutUploadProgress(int done, int total);

private:
    void onDeviceUpdated(const BluezAdapter::Device &device);
    void onConnectFinished(const QString &objectPath, bool ok, const QString &error,
                            const QString &errorName);
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

    // One page of the cloud's workout list, then the next. The list used to
    // be a single request for the hundred most recent, so anything older was
    // never fetched at all - which is a hole in the history and therefore in
    // the chronic load computed over it.
    // `untilMs` threads the first page's server cursor through the chain so
    // the last page can store it; 0 on the initial call.
    void fetchCloudWorkoutPage(const QString &sessionKey, qint64 sinceMs, int offset, int stored,
                                qint64 untilMs);
    void startCloudSync(bool full);

    // Whiteboard is one request, one response: only one process may hold a
    // session with the watch. The app takes the link whenever it attaches
    // and gives it back when it detaches, which makes the notification
    // daemon - if one is installed at all - step aside while the app is
    // using the watch. Nothing here fails if no daemon exists; the name is
    // simply uncontested.
    // Takes the watch link and attaches, waiting a little for the lock
    // rather than barging in. The daemon holds it for a second or two per
    // notification, so waiting costs almost nothing - and attaching anyway,
    // which is what this used to do, puts two writers on one Whiteboard
    // session and corrupts the framing. That showed up as "the watch didn't
    // acknowledge the session handshake", which is a true statement about a
    // self-inflicted problem.
    // 250 ms apart, so the default is half a minute of waiting before the
    // lock is ignored. It was five seconds, which was right when the
    // application attached once per run and wrong as soon as it attached
    // per operation: the daemon holds the watch for about a second per
    // notification and longer when one is being retried, so five seconds
    // ran out routinely - and barging in makes both processes notifying
    // clients, which doubles every reply and wedges whichever handshake or
    // paged read is in flight. Measured once at eighteen seconds for an
    // attach that should take a tenth of that. Half a minute still covers
    // the case the barge-in exists for, a daemon that is genuinely stuck.
    void attachWhenLinkIsFree(const QString &objectPath, int attemptsLeft = 120);
    void releaseWatchLink();

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
    // Held while the Whiteboard session is, so the notification daemon - if
    // one is installed - knows to stay off the watch. Declared here, next to
    // the session it guards, and initialised in that order.
    WatchLink m_watchLink;
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
    // The id of the last notification testNotification() sent, so that
    // testNotificationRemove() can take that one back off.
    quint32 m_lastNotificationId = 0;

    WorkoutStore *m_workoutStore;
    WorkoutListModel *m_workoutModel;
    bool m_workoutSyncInProgress = false;
    bool m_cloudSamplesInProgress = false;

    // Guards against a second tap while a POST is in flight - the server
    // has no idempotency key, so a double send would create two workouts.
    bool m_uploadInProgress = false;

    bool m_baseMapEnabled = false;
    QString m_mapTileUrl;
    bool m_recordsSyncInProgress = false;
    bool m_suuntoPlusBusy = false;
    bool m_storeBusy = false;

    // Weak on purpose: this does not keep the session alive, it only finds
    // the one the holders share.
    std::weak_ptr<void> m_watchSession;
    // Operations waiting for an attach that is in flight.
    QVector<std::function<void(const WatchSession &)>> m_watchSessionWaiters;
    bool m_watchSessionAttaching = false;
    // Tells one attempt's timeout from the next one's, so a late timer
    // cannot fail the operations queued behind a later, healthy attach.
    int m_watchSessionAttachGeneration = 0;
    // How long an attach takes, measured rather than assumed: it is the
    // whole cost of holding the watch per operation instead of for the run
    // of the app, and a guess about it is exactly the kind that would never
    // be checked.
    QElapsedTimer m_watchAttachTimer;
    // How long the current session has been held, so the log says that as
    // well as how long it took to get - see releaseIdleWatchSession().
    QElapsedTimer m_watchSessionHeld;
    // The operations whose span is a member flag rather than one chain of
    // lambdas - a sync walks through several member functions, and its
    // in-progress flag already has correct clear-on-every-exit-path
    // behaviour, so the session rides along with the flag rather than
    // getting a second, parallel set of bookkeeping to get wrong.
    WatchSession m_workoutSyncSession;
    WatchSession m_suuntoPlusSession;
    WatchSession m_storeSession;
    WatchSession m_gpsSession;
    // There is deliberately no session member for the probes and the
    // watch-settings reads. There was one, assigned at the start of each
    // and released where their in-flight flag cleared - and two of them
    // have no flag, so opening the settings page took the watch and never
    // gave it back, which blocked notifications for the rest of the run.
    // A session belongs in the callback that needs it: a capture cannot be
    // forgotten on a path nobody thought about.
    // One cloud FIT export at a time; it is a whole file over the network.
    bool m_fitExportInProgress = false;
    // The last refresh's answers. The library as the cloud gave it, and the
    // filenames the watch's directory held - matching one to the other is
    // what says "installed".
    //
    // Kept as variant maps rather than the client's own struct because this
    // header only forward-declares SuuntoCloudClient, and it is what QML
    // wants anyway. Keys: id, pluginId, name, description, type, version.
    QVariantList m_suuntoPlusLibrary;
    // The catalogue, as the last refresh left it: the categories in the
    // store's own order, and every item keyed by id, because a category
    // holds references and several categories hold the same item.
    QVariantList m_storeCategories;
    QVariantMap m_storeItemsById;
    QSet<QString> m_suuntoPlusFiles;
    QString m_suuntoPlusLocation;
    QString m_coverMode;
    bool m_syncOnConnect = false;

    HealthStore *m_healthStore;
    bool m_healthSyncInProgress = false;
    bool m_gpsUpdateInProgress = false;
    // The sequential per-kind loop behind syncHealthData(), same shape as
    // fetchWatchEntryAt(): four requests one after another rather than four
    // at once, so a failure can name which kind failed and the tally stays
    // simple.
    void fetchHealthKindAt(int index, int fetched, const QStringList &failures);
    void uploadHealthKindAt(int index, int sent, const QStringList &failures);
    // Chained after the sleep fetch - a separate resource, so a separate
    // request. Sleep having already succeeded, a failure here is reported
    // rather than failing the whole sync.
    void fetchWatchRecovery(const WatchSession &session);
    // Chained after recovery - a third resource again, and the one that
    // needed libmds.so to find at all. Same treatment: reported but not
    // fatal, since sleep and recovery have already landed by then.
    void fetchWatchActivity(const WatchSession &session);

    // The connected watch's own SBEM field table, and the field offsets
    // resolved from it. Descriptor ids are per watch model, so decoding a
    // Suunto 9 Baro's workout against a Race's table yields zeros for
    // every field (docs/sbem-chunk-map.md). Defaults to the compiled-in
    // Race table until the real one has been read off the watch.
    SbemDescriptors::Table m_descriptorTable;
    SbemLayout::Layout m_descriptorLayout;
    // Whether m_descriptorTable came from the watch now connected, as
    // opposed to being the compiled-in fallback.
    QString m_descriptorAddress;

    // Reads the watch's table if it isn't already in hand: from the local
    // store first, and failing that off the watch itself, using any
    // logbook id (the resource is per workout but the table is not).
    // Calls `then` either way - a watch whose table cannot be read still
    // gets decoded with the built-in one, which is no worse than before.
    // The file the watch keeps an item under: <id>.wfa for a watch face,
    // <pluginId>.fea for a sports app. A guide is <id>.gui, but guides come
    // from a different listing and are not in this library.
    static QString suuntoPlusFileName(const QVariantMap &item);
    void setSuuntoPlusBusy(bool busy);

    // The second half of refreshSuuntoPlus(): where the watch keeps plugins
    // and what is in there. Its own method because the library half is two
    // chained cloud requests now, and the nesting had stopped being
    // readable.
    void readWatchPluginFiles();
    // Shared by the two exports: the directory, the name, the write and the
    // one message that says where it went.
    void writeExport(const QString &key, const QString &label, const QString &extension,
                      const QByteArray &contents);
    QString sourceOfWorkout(const QString &key) const;
    // A watch workout's stored payload, written as a FIT file: the samples
    // from the payload itself and the summary from the row, because a
    // watch workout's ascent, descent and energy are the watch's own
    // arithmetic from /Summary rather than something to derive again.
    QByteArray fitFromWatchWorkout(const QString &key, const QByteArray &payload) const;
    // The second half of the two watch syncs: everything after the session
    // is in hand. Split out so the entry point can be the one place that
    // takes the session, rather than every branch of the chain below.
    void startWatchWorkoutSync();
    // The health sync is three resources read one after another, and its
    // in-progress flag clears between them - so the session is a parameter
    // here rather than riding with the flag. It did ride with the flag for
    // one build, and the sleep step's own completion let go of the watch
    // before recovery had asked for anything.
    void startWatchHealthSync(const WatchSession &session);

    // Attaches to the watch if nothing holds a session, then runs
    // `operation` with one. Refuses through errorOccurred() when no watch
    // is connected, and reports an attach that fails or never completes the
    // same way, in which case `operation` never runs.
    void withWatchSession(const std::function<void(const WatchSession &)> &operation);
    // The session every holder shares, created on demand.
    WatchSession currentWatchSession();
    // Detaches, if and only if nothing took a new session in the meantime.
    // Deferred through the event loop by the session's own deleter: the
    // last holder is usually a callback being destroyed inside the
    // whiteboard client, and detaching from in there would pull the ground
    // out from under the code doing the destroying.
    void releaseIdleWatchSession();
    void setStoreBusy(bool busy);
    // Adding and removing differ by one call, so they share the rest: the
    // context, the item lookup, the error wording and the reload.
    void changeStoreLibrary(const QString &id, bool add);
    // Runs `then` with the session key, the variant and the capability
    // string - the three things every SuuntoPlus request needs.
    // `onFailure` clears whichever busy flag the caller set; omitted, it
    // clears the SuuntoPlus one.
    void withSuuntoPlusContext(const std::function<void(const QString &key,
                                                          const QString &variant,
                                                          const QString &capabilities)> &then,
                                const std::function<void()> &onFailure = nullptr);
    void ensureDescriptors(const QString &logbookId, const std::function<void()> &then);
    // The store-only half, which needs no BLE and no callback. Called on
    // startup and whenever the paired watch changes, so that an upload or
    // a probe run before any sync this session still uses the right table.
    void loadStoredDescriptors();
    // Clears the in-progress flag and reports, from every exit of
    // updateWatchGps()'s callback chain.
    void finishGpsUpdate(const QString &message);
    // After the commit the watch validates the data on its own time - a
    // Race is done in about three seconds, a 9 Baro takes minutes for
    // nearly three times as much. So the date is polled rather than read
    // once after a fixed wait that would be wrong for one of them.
    void pollEphemerisDate(int format, int attempt);

    // Decodes again, from bytes already stored, any watch workout that
    // decoded to nothing the first time.
    //
    // Needed because the watch lists only entries it considers
    // unsynchronised: three Suunto 9 Baro workouts were fetched and stored
    // while the decoder was still using a Race's field table, decoded to
    // rows of zeros, and then vanished from /Logbook/Entries for good. No
    // amount of syncing brings them back. Their bytes are still here,
    // which is exactly why they are kept, so the repair is local.
    //
    // Runs whenever a watch's field table changes, and only touches rows
    // that have nothing in them - see WorkoutStore::keysWithEmptyDecode().
    // Returns how many rows it actually improved.
    int redecodeStoredWorkouts();
    // One step of testActivityTrendFetch()'s encoding sweep.
    void tryActivityTrendEncoding(int index);
    void uploadWorkoutAt(const QVector<QString> &keys, int index, int succeeded,
                          const QStringList &failures);
    // Builds the sml.zip for one workout from its stored raw payloads.
    QByteArray buildUploadZip(const QString &key) const;
};
