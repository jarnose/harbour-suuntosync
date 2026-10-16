#pragma once

#include <QString>

// Arbitrates who may hold a Whiteboard session with the watch.
//
// The constraint is the protocol's: Whiteboard is strictly one request, one
// response, with matched request ids, so two processes writing to the same
// watch corrupt each other's framing. BlueZ refcounts the *connection*, so
// that part is fine - what has to be exclusive is the session, which means
// MdsWhiteboardClient::attachToDevice() and detach().
//
// **This is a file lock, not a D-Bus name**, and that is not a matter of
// taste. A D-Bus name would have been the tidier mechanism, and it does not
// work: Sailjail's Base.permission grants a sandboxed application
// `dbus-user.own org.sailfishos.coveraction.*` and nothing else, so owning
// io.github.jarnose.suuntosync.WatchLink would need a permission file
// installed into /etc/sailjail/permissions - which is what Whisperfish
// ships, and which is a thing a Store package should not be doing. An
// advisory lock on a file in the application's own data directory needs no
// permission at all, and the kernel releases it when the holder dies, which
// a D-Bus name does not improve on.
//
// The two sides are deliberately asymmetric:
//
//  - Both sides now take the lock for one operation and let go. The
//    application attaches when something it is asked to do needs the
//    watch, holds the session for that chain of requests, and releases as
//    soon as the last holder of it goes away - see
//    AppController::withWatchSession(). The daemon does the same for one
//    send.
//  - The asymmetry that is left: the application **attaches whether or not
//    it got the lock**, after waiting five seconds for it. It is the side
//    with a person waiting on it, and a stuck daemon should not make the
//    watch unusable from the app.
//
// **What an attach costs, measured on a Jolla Phone 2026 and a Race:**
// 537 ms for the first one after launch and 91-105 ms for every one after
// that. So taking the watch per operation is close to free, and there is
// deliberately no idle timer holding the session "just in case" - the
// first version of this change was going to have one, and the measurement
// is what settled that it would have bought nothing but a longer wait for
// notifications. The one-off half second is the GATT object enumeration
// being cold.
//
// **One way this has already gone wrong**, since the shape of the mistake
// is the reusable part: the first version kept a session in a member
// variable for the developer probes and the watch-settings reads, released
// where their in-flight flag cleared. Two of those have no in-flight flag,
// so opening the settings page - which reads the watch's notification
// switch - took the watch and never gave it back, and notifications
// stopped arriving for the rest of the run. The daemon's log said so
// plainly ("waiting for the watch: the other process is using the watch",
// for minutes) while the application looked perfectly healthy. A session
// belongs in the callback that needs it, where the capture cannot be
// forgotten on a path nobody thought about; a member only works where
// something is *obliged* to clear it, which is why the two syncs pass it
// as a parameter or tie it to a flag that clears on every exit.
//
// **The second way it has gone wrong**, the same day as the first: the
// application takes the lock *before* it attaches, and an attach that
// never finishes - which is what a wedged or frozen watch produces -
// created no session token, so nothing was ever destroyed and nothing ever
// released the lock. The daemon starved for the rest of the run, logging
// "the other process is using the watch" by the minute and giving up on
// notifications the watch never saw. From the outside it looked like the
// watch had stopped alerting; the application's own log showed no session
// at all, because none had been created.
//
// Found with /proc/locks - the application's pid holding an FLOCK on this
// file with nothing in its log to match - after two wrong guesses, one of
// which came from `pgrep -x` failing to match the kernel's truncated
// process name and so reporting the application as not running at all.
// AppController::abandonWatchSession() is the fix: the failure paths let
// the lock go, because they are the ones with no token to do it for them.
//
// This was not always so. The application used to attach on connect and
// hold the session for as long as it ran, which meant "the application is
// open" meant "notifications wait" - including while it sat in the
// background, which on this platform is most of the time. Only a transfer
// blocks a notification now, and that wait is seconds, because Whiteboard
// is strictly one request at a time and nothing can change that.
class WatchLink
{
public:
    enum Role {
        Foreground,  // the application: takes the lock, proceeds regardless
        Background,  // the daemon: no lock, no watch
    };

    // `dataDirectory` is the application's AppDataLocation - the same
    // directory the SQLite database lives in, so both processes name the
    // same file without either of them configuring anything.
    WatchLink(const QString &dataDirectory, Role role);
    ~WatchLink();

    WatchLink(const WatchLink &) = delete;
    WatchLink &operator=(const WatchLink &) = delete;

    // Non-blocking. True if the lock is now held - by us, and only us.
    // Calling it while already holding is a no-op that returns true.
    bool tryClaim(QString *error = nullptr);

    void release();

    bool owns() const { return m_fd >= 0; }

    Role role() const { return m_role; }

private:
    QString m_path;
    Role m_role;
    int m_fd = -1;
};
