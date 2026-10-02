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
    void onConnectFinished(const QString &objectPath, bool ok, const QString &error,
                            const QString &errorName);
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
        // When it was queued. A notification nobody could deliver because
        // the watch was out of range is noise by the time it comes back,
        // and without a deadline it would keep the retry timer - and a
        // connect attempt every five seconds - going for as long as the
        // daemon lives.
        qint64 queuedAtMs = 0;
    };

    void reloadWatch();

    // Asks BlueZ, rather than trusting what it last said. BluezAdapter does
    // not subscribe to a device's PropertiesChanged - it learns state from
    // refresh() and InterfacesAdded only - so a disconnect is invisible to
    // it. The application gets away with that because a person drives it; a
    // daemon that believed a four-hour-old "connected" claimed the watch
    // lock and sat in an attach that could never finish.
    bool watchIsConnected();
    // Gives up the Whiteboard session and the lock, and deliberately leaves
    // the Bluetooth connection up: BlueZ refcounts it, the application can
    // open its own, and keeping it means the next notification pays for a
    // handshake rather than for a whole reconnect.
    void letGo();

    PairedWatchStore m_store;
    bool m_dryRun;

    BluezAdapter *m_adapter;
    MdsWhiteboardClient *m_client;
    NotificationMonitor *m_monitor;
    QTimer *m_retry;
    // An attach that never completes used to hold the lock for ever:
    // MdsWhiteboardClient waits for ServicesResolved with no deadline of its
    // own, and the handshake timeout only fires once the handshake has been
    // sent.
    QTimer *m_attachTimeout;
    WatchLink m_link;

    PairedWatch m_watch;
    bool m_connected = false;
    bool m_busy = false;
    // attachToDevice() takes seconds - GATT resolution, StartNotify, the
    // session handshake - and BlueZ reports device properties repeatedly
    // while it happens. Without this, every property change started the
    // attach over, each one detaching the last one's half-finished state,
    // and the log filled with ready/gone pairs and handshake timeouts from
    // attempts nobody was waiting for any more.
    bool m_attaching = false;
    // The daemon has to open the Bluetooth connection itself. Waiting for
    // one, the way the first version did, means it only ever works just
    // after the application has been using the watch - which is exactly
    // when it is least needed. The watch drops the link within seconds of
    // the last client letting go.
    bool m_connecting = false;

    // Bounded: long enough that a burst does not evict itself, short enough
    // that a watch coming back into range does not replay a whole morning.
    QList<Pending> m_queue;

    // Phone notification id -> the id we sent it to the watch as, so a
    // close on the phone can take the right one off the watch.
    QHash<quint32, quint32> m_sent;

    // The last notification's package, title and message, and when it was
    // seen. Sailfish's mail server posts the same notification twice - ids
    // 531 and 532, same sender, same subject, within the same second - and
    // because the watch id is derived from the phone's id, the two are
    // different notifications as far as the watch is concerned. A person
    // would see one message arrive and two appear on their wrist.
    QString m_lastContent;
    qint64 m_lastContentAtMs = 0;
};
