#include "notificationmonitor.h"

#include <QSocketNotifier>
#include <QVariant>

#include <dbus/dbus.h>

namespace {

const char *const kNotificationsInterface = "org.freedesktop.Notifications";

// The three rules. A monitor sees a message matching any of them.
const char *const kRules[] = {
    "type='method_call',interface='org.freedesktop.Notifications',member='Notify'",
    "type='method_return',sender='org.freedesktop.Notifications'",
    "type='signal',sender='org.freedesktop.Notifications',"
    "interface='org.freedesktop.Notifications',member='NotificationClosed'",
};
const int kRuleCount = 3;

// How many unanswered Notify calls to keep. The reply normally arrives in
// microseconds; this only bounds the damage if a server never answers.
const int kMaxPending = 32;

// The filter libdbus calls. A plain function because that is what the API
// takes; everything it does is in handleMessage().
DBusHandlerResult messageFilter(DBusConnection *connection, DBusMessage *message, void *user)
{
    NotificationMonitor *monitor = static_cast<NotificationMonitor *>(user);
    return static_cast<DBusHandlerResult>(monitor->handleMessage(connection, message));
}

bool readString(DBusMessageIter *iter, QString *out)
{
    if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_STRING)
        return false;
    const char *value = nullptr;
    dbus_message_iter_get_basic(iter, &value);
    *out = QString::fromUtf8(value ? value : "");
    dbus_message_iter_next(iter);
    return true;
}

bool readUInt32(DBusMessageIter *iter, quint32 *out)
{
    if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_UINT32)
        return false;
    dbus_uint32_t value = 0;
    dbus_message_iter_get_basic(iter, &value);
    *out = value;
    dbus_message_iter_next(iter);
    return true;
}

// A variant holding a string, which is all three hints we want are. A hint
// of another type is simply not read rather than coerced.
QString variantString(DBusMessageIter *variantIter)
{
    DBusMessageIter inner;
    dbus_message_iter_recurse(variantIter, &inner);
    if (dbus_message_iter_get_arg_type(&inner) != DBUS_TYPE_STRING)
        return QString();
    const char *value = nullptr;
    dbus_message_iter_get_basic(&inner, &value);
    return QString::fromUtf8(value ? value : "");
}

} // namespace

NotificationMonitor::NotificationMonitor(QObject *parent)
    : QObject(parent)
{
}

NotificationMonitor::~NotificationMonitor()
{
    if (m_notifier)
        m_notifier->setEnabled(false);
    if (m_connection) {
        dbus_connection_remove_filter(m_connection, messageFilter, this);
        dbus_connection_close(m_connection);
        dbus_connection_unref(m_connection);
        m_connection = nullptr;
    }
}

bool NotificationMonitor::start(QString *error)
{
    DBusError dbusError = DBUS_ERROR_INIT;

    // A private connection: a monitor connection cannot be used for
    // ordinary traffic, so it must not be the one anything else shares.
    m_connection = dbus_bus_get_private(DBUS_BUS_SESSION, &dbusError);
    if (!m_connection) {
        if (error)
            *error = QString::fromUtf8(dbusError.message ? dbusError.message
                                                          : "no session bus");
        dbus_error_free(&dbusError);
        return false;
    }

    // Losing the bus must not take the daemon with it - systemd will
    // restart us, and an abort looks like a crash in the journal.
    dbus_connection_set_exit_on_disconnect(m_connection, FALSE);

    DBusMessage *request = dbus_message_new_method_call(
            DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
            "org.freedesktop.DBus.Monitoring", "BecomeMonitor");
    if (request) {
        DBusMessageIter iter;
        DBusMessageIter array;
        dbus_message_iter_init_append(request, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &array);
        for (int i = 0; i < kRuleCount; ++i)
            dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &kRules[i]);
        dbus_message_iter_close_container(&iter, &array);
        const dbus_uint32_t flags = 0;
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &flags);

        DBusMessage *reply = dbus_connection_send_with_reply_and_block(
                m_connection, request, 5000, &dbusError);
        if (reply) {
            dbus_message_unref(reply);
            m_isMonitor = true;
        } else {
            dbus_error_free(&dbusError);
        }
        dbus_message_unref(request);
    }

    if (!m_isMonitor) {
        // Pre-1.10 buses, and anything that refuses Monitoring. Same rules
        // with eavesdrop='true' appended, which that era understood.
        for (int i = 0; i < kRuleCount; ++i) {
            const QByteArray rule = QByteArray(kRules[i]) + ",eavesdrop='true'";
            dbus_bus_add_match(m_connection, rule.constData(), &dbusError);
            if (dbus_error_is_set(&dbusError))
                dbus_error_free(&dbusError);
        }
    }

    if (!dbus_connection_add_filter(m_connection, messageFilter, this, nullptr)) {
        if (error)
            *error = QStringLiteral("could not install a D-Bus message filter");
        return false;
    }

    int fd = -1;
    if (!dbus_connection_get_unix_fd(m_connection, &fd) || fd < 0) {
        if (error)
            *error = QStringLiteral("the session bus connection has no socket to watch");
        return false;
    }

    m_notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated,
            this, &NotificationMonitor::onSocketReady);

    // Anything already queued by the blocking call above.
    dispatchAll();
    return true;
}

void NotificationMonitor::onSocketReady()
{
    // Non-blocking: read whatever is there, then dispatch it all.
    dbus_connection_read_write(m_connection, 0);
    dispatchAll();
}

void NotificationMonitor::dispatchAll()
{
    while (dbus_connection_get_dispatch_status(m_connection) == DBUS_DISPATCH_DATA_REMAINS)
        dbus_connection_dispatch(m_connection);
}

unsigned int NotificationMonitor::handleMessage(DBusConnection *, DBusMessage *message)
{
    const int type = dbus_message_get_type(message);

    if (type == DBUS_MESSAGE_TYPE_METHOD_CALL
            && dbus_message_is_method_call(message, kNotificationsInterface, "Notify")) {
        handleNotify(message);
    } else if (type == DBUS_MESSAGE_TYPE_METHOD_RETURN) {
        handleReply(message);
    } else if (type == DBUS_MESSAGE_TYPE_SIGNAL
               && dbus_message_is_signal(message, kNotificationsInterface,
                                          "NotificationClosed")) {
        handleClosed(message);
    }

    // A monitor must never claim a message: it is somebody else's.
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void NotificationMonitor::handleNotify(DBusMessage *message)
{
    // Check the signature rather than trusting the member name: a Notify
    // with a different shape is some other server's, and reading it with
    // the wrong iterator types would be reading garbage.
    const char *signature = dbus_message_get_signature(message);
    if (!signature || qstrcmp(signature, "susssasa{sv}i") != 0)
        return;

    PhoneNotification notification;
    notification.serial = dbus_message_get_serial(message);

    DBusMessageIter iter;
    dbus_message_iter_init(message, &iter);
    if (!readString(&iter, &notification.appName))
        return;
    if (!readUInt32(&iter, &notification.replacesId))
        return;
    if (!readString(&iter, &notification.appIcon))
        return;
    if (!readString(&iter, &notification.summary))
        return;
    if (!readString(&iter, &notification.body))
        return;

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
        return;
    DBusMessageIter actions;
    dbus_message_iter_recurse(&iter, &actions);
    while (dbus_message_iter_get_arg_type(&actions) == DBUS_TYPE_STRING) {
        QString action;
        readString(&actions, &action);
        notification.actions.append(action);
    }
    dbus_message_iter_next(&iter);

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
        return;
    DBusMessageIter hints;
    dbus_message_iter_recurse(&iter, &hints);
    while (dbus_message_iter_get_arg_type(&hints) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&hints, &entry);
        QString key;
        if (readString(&entry, &key)
                && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
            if (key == QStringLiteral("x-nemo-owner"))
                notification.owner = variantString(&entry);
            else if (key == QStringLiteral("x-nemo-preview-summary"))
                notification.previewSummary = variantString(&entry);
            else if (key == QStringLiteral("x-nemo-preview-body"))
                notification.previewBody = variantString(&entry);
            else if (key == QStringLiteral("category"))
                notification.category = variantString(&entry);
        }
        dbus_message_iter_next(&hints);
    }
    dbus_message_iter_next(&iter);

    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INT32) {
        dbus_int32_t timeout = -1;
        dbus_message_iter_get_basic(&iter, &timeout);
        notification.expireTimeout = timeout;
    }

    if (m_pending.size() >= kMaxPending)
        m_pending.clear();
    m_pending.insert(notification.serial, notification);
}

void NotificationMonitor::handleReply(DBusMessage *message)
{
    const quint32 replyTo = dbus_message_get_reply_serial(message);
    if (replyTo == 0 || !m_pending.contains(replyTo))
        return;

    const char *signature = dbus_message_get_signature(message);
    if (!signature || qstrcmp(signature, "u") != 0) {
        m_pending.remove(replyTo);
        return;
    }

    DBusMessageIter iter;
    dbus_message_iter_init(message, &iter);
    quint32 id = 0;
    if (!readUInt32(&iter, &id)) {
        m_pending.remove(replyTo);
        return;
    }

    PhoneNotification notification = m_pending.take(replyTo);
    notification.id = id;
    emit posted(notification);
}

void NotificationMonitor::handleClosed(DBusMessage *message)
{
    DBusMessageIter iter;
    dbus_message_iter_init(message, &iter);
    quint32 id = 0;
    if (readUInt32(&iter, &id))
        emit closed(id);
}
