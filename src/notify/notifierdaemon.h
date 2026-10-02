#pragma once

#include "notificationmonitor.h"
#include "notificationrouter.h"
#include "watchlink.h"
#include "../ble/bluezadapter.h"
#include "../ble/mdswhiteboardclient.h"
#include "../ble/pairedwatch.h"
#include "../ble/pairedwatchstore.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

class QTimer;

// The notification daemon: watch the session bus, send what arrives to the
// watch.
//
// A separate package from the application on purpose. Observing other
// applications' notifications needs a D-Bus monitor connection, which the
// Sailjail proxy will not relay - so the daemon lives outside the sandbox
// and the application stays inside it, and stays Store-eligible.
//
// It reads the application's own database to learn which watch to talk to
// rather than having configuration of its own: same user, same file, one
// source of truth, and nothing to keep in step when the user switches
// watches in the app.
//
// It holds the watch only while it has something to send, and lets go
// immediately afterwards - see watchlink.h for why that asymmetry is the
// arbitration, and what it costs.
class NotifierDaemon : public QObject
{
    Q_OBJECT
public:
    // `dryRun` sends nothing and logs what it would have sent, which is how
    // the monitoring half gets tested with no watch in the room.
    NotifierDaemon(const QString &databasePath, bool dryRun, QObject *parent = nullptr);

    bool start(QString *error);

private slots:
    void onPosted(const PhoneNotification &notification);
    void onClosed(quint32 id);
    void onDeviceUpdated(const BluezAdapter::Device &device);
    void pump();

private:
    // One thing to do to the watch. Removals ride the same queue as sends
    // so that a notification dismissed on the phone a second after it
    // arrived cannot overtake its own arrival.
    struct Pending
    {
        bool isRemoval = false;
        Ancs::Notification add;
        quint32 removeId = 0;
    };

    void reloadWatch();
    void letGo();

    PairedWatchStore m_store;
    bool m_dryRun;

    BluezAdapter *m_adapter;
    MdsWhiteboardClient *m_client;
    NotificationMonitor *m_monitor;
    QTimer *m_retry;
    WatchLink m_link;

    PairedWatch m_watch;
    bool m_connected = false;
    bool m_busy = false;

    // Bounded: long enough that a burst does not evict itself, short enough
    // that a watch coming back into range does not replay a whole morning.
    QList<Pending> m_queue;

    // Phone notification id -> the id we sent it to the watch as, so a
    // close on the phone can take the right one off the watch.
    QHash<quint32, quint32> m_sent;
};
