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
//  - The application takes the lock when it attaches and holds it until it
//    detaches, and **attaches whether or not it got it**. It is the side
//    with a person waiting on it, and the daemon's own window is seconds.
//  - The daemon attaches only while it holds the lock, for one send, and
//    lets go immediately afterwards. So "the application is open" means
//    notifications wait, which is the trade-off this design accepts.
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
