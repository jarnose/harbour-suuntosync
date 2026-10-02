#pragma once

#include "phonenotification.h"

#include <QHash>
#include <QObject>

struct DBusConnection;
struct DBusMessage;
class QSocketNotifier;

// Watches the session bus for every application's notifications.
//
// This cannot be done with QtDBus. A notification is an ordinary method
// call addressed to org.freedesktop.Notifications, and QtDBus only delivers
// messages addressed to us - so seeing somebody else's needs a monitor
// connection and a raw message filter, which means libdbus-1 directly.
//
// The mechanism was read from libwatchfish's NotificationMonitor
// (GPL-3.0, Javier S. Pedro) the way libdivecomputer was read earlier in
// this project: for the protocol facts, not the code. Those facts:
//
//   - org.freedesktop.DBus.Monitoring.BecomeMonitor, taking an array of
//     match rules and a flags word, on dbus 1.10 and newer. The phone here
//     runs 1.16.2.
//   - On failure, legacy AddMatch rules with eavesdrop='true'.
//   - Three rules are needed, not one: the Notify *method call*, the
//     *method return* that carries the id the server assigned, and the
//     NotificationClosed signal.
//   - Notify's signature is susssasa{sv}i.
//
// Confirmed on the phone before any of this was written: an unsandboxed
// process does see another connection's Notify call in full, hints
// included. That is the whole premise, and it is why this is a separate
// package - inside Sailjail the D-Bus proxy relays only our own traffic.
//
// The libdbus connection is driven from the Qt event loop with a
// QSocketNotifier on its socket rather than with libdbus watch functions:
// one fd, no threads, and nothing to get wrong about re-entrancy.
class NotificationMonitor : public QObject
{
    Q_OBJECT
public:
    explicit NotificationMonitor(QObject *parent = nullptr);
    ~NotificationMonitor() override;

    // Connects to the session bus and starts monitoring. Returns false and
    // fills `error` if the bus is unreachable or the filter cannot be
    // installed. Becoming a monitor failing is NOT an error: the legacy
    // eavesdrop path is tried next, and `isMonitor()` says which happened.
    bool start(QString *error);

    bool isMonitor() const { return m_isMonitor; }

    // Called from the libdbus message filter, which has to be a plain
    // function. Public only for that reason; the return value is a
    // DBusHandlerResult, kept as an int so this header does not have to
    // drag dbus/dbus.h into everything that includes it.
    unsigned int handleMessage(DBusConnection *connection, DBusMessage *message);

signals:
    // Emitted once per notification, after the server's reply has supplied
    // the id - so `notification.id` is always set. A notification whose
    // reply never arrives is dropped rather than reported without an id,
    // because an id is what a later removal needs.
    void posted(const PhoneNotification &notification);
    void closed(quint32 id);

private slots:
    void onSocketReady();

private:
    void handleNotify(DBusMessage *message);
    void handleReply(DBusMessage *message);
    void handleClosed(DBusMessage *message);
    void dispatchAll();

    DBusConnection *m_connection = nullptr;
    QSocketNotifier *m_notifier = nullptr;
    bool m_isMonitor = false;

    // Notify calls whose reply has not arrived yet, by serial. Bounded:
    // anything that has been waiting while 32 newer calls came and went is
    // never going to get an id.
    QHash<quint32, PhoneNotification> m_pending;
};
