#include "notifierdaemon.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QTimer>

namespace {
const int kMaxQueued = 8;
// How long to wait before trying the lock again. The application holds it
// for as long as it is attached to the watch, so this is a slow poll on
// purpose: there is nothing to be gained by asking often.
const int kRetryMs = 5000;
} // namespace

NotifierDaemon::NotifierDaemon(const QString &databasePath, bool dryRun, QObject *parent)
    : QObject(parent)
    , m_store(databasePath)
    , m_dryRun(dryRun)
    , m_adapter(new BluezAdapter(this))
    , m_client(new MdsWhiteboardClient(this))
    , m_monitor(new NotificationMonitor(this))
    , m_retry(new QTimer(this))
    , m_link(QFileInfo(databasePath).absolutePath(), WatchLink::Background)
{
    m_retry->setInterval(kRetryMs);
    connect(m_retry, &QTimer::timeout, this, &NotifierDaemon::pump);
}

bool NotifierDaemon::start(QString *error)
{
    connect(m_monitor, &NotificationMonitor::posted, this, &NotifierDaemon::onPosted);
    connect(m_monitor, &NotificationMonitor::closed, this, &NotifierDaemon::onClosed);
    if (!m_monitor->start(error))
        return false;
    qInfo() << "watching the session bus via"
            << (m_monitor->isMonitor() ? "BecomeMonitor" : "legacy eavesdropping");

    if (m_dryRun) {
        qInfo() << "dry run: nothing will be sent to a watch";
        return true;
    }

    if (!m_store.open(error))
        return false;
    reloadWatch();

    connect(m_adapter, &BluezAdapter::deviceUpdated, this, &NotifierDaemon::onDeviceUpdated);
    connect(m_adapter, &BluezAdapter::connectFinished, this,
             &NotifierDaemon::onConnectFinished);
    connect(m_adapter, &BluezAdapter::errorOccurred, this, [](const QString &message) {
        qWarning() << "bluez:" << message;
    });
    connect(m_client, &MdsWhiteboardClient::errorOccurred, this, [](const QString &message) {
        qWarning() << "whiteboard:" << message;
    });
    connect(m_client, &MdsWhiteboardClient::readyChanged, this, [this](bool ready) {
        m_attaching = false;
        qInfo() << "whiteboard session" << (ready ? "ready" : "gone");
        pump();
    });

    // After the event loop starts, so the connections above are in place
    // when the first batch of devices arrives.
    QTimer::singleShot(0, this, [this]() { m_adapter->refresh(); });
    return true;
}

void NotifierDaemon::reloadWatch()
{
    QString error;
    m_watch = m_store.load(&error);
    if (!error.isEmpty())
        qWarning() << "could not read the paired watch:" << error;
    else if (m_watch.address.isEmpty())
        qInfo() << "no watch is paired in the application yet";
    else
        qInfo() << "paired watch:" << m_watch.name << m_watch.address;
}

void NotifierDaemon::onDeviceUpdated(const BluezAdapter::Device &device)
{
    // The user may have switched watches in the app since we last looked.
    if (m_watch.objectPath.isEmpty())
        reloadWatch();
    if (device.objectPath != m_watch.objectPath)
        return;

    if (device.connected != m_connected) {
        m_connected = device.connected;
        qInfo() << "watch" << (m_connected ? "connected" : "disconnected");
        if (m_connected)
            m_connecting = false;
        else
            m_attaching = false;
    }
    pump();
}

void NotifierDaemon::onConnectFinished(const QString &objectPath, bool ok, const QString &error)
{
    if (objectPath != m_watch.objectPath)
        return;
    m_connecting = false;
    if (!ok) {
        qWarning() << "could not connect to the watch:" << error;
        // Leave the queue alone and let the retry timer come back to it: a
        // watch out of range now may be in range in five seconds.
        m_retry->start();
        return;
    }

    // Take this as connected rather than waiting for BlueZ to say so.
    // connectFinished arrives *before* the Connected property update, and
    // pumping on the strength of the property alone spun: every pump saw
    // !m_connected and started another connect, dozens per millisecond.
    // The application has always done it this way; the daemon now does too.
    if (!m_connected) {
        m_connected = true;
        qInfo() << "watch connected";
    }
    pump();
}

void NotifierDaemon::onPosted(const PhoneNotification &notification)
{
    NotificationRouter::Candidate candidate;
    candidate.appId = (notification.owner.isEmpty() ? notification.appName
                                                     : notification.owner).toStdString();
    candidate.title = notification.title().toStdString();
    candidate.message = notification.text().toStdString();
    candidate.category = notification.category.toStdString();
    candidate.phoneId = notification.id;

    if (const char *reason = NotificationRouter::dropReason(candidate)) {
        qDebug().noquote() << QStringLiteral("ignoring %1 from %2: %3")
                                      .arg(notification.category.isEmpty()
                                                   ? QStringLiteral("(no category)")
                                                   : notification.category,
                                            QString::fromStdString(candidate.appId),
                                            QString::fromLatin1(reason));
        return;
    }

    Pending pending;
    pending.add = NotificationRouter::toWatchNotification(
            candidate, static_cast<uint32_t>(QDateTime::currentMSecsSinceEpoch() / 1000));

    // Logged at info even in a real run: this is the one line that says what
    // the daemon decided, and the categories are still being learned.
    qInfo().noquote() << QStringLiteral("notification %1 from %2 category=%3 -> ancs %4: %5 / %6")
                                 .arg(notification.id)
                                 .arg(QString::fromStdString(candidate.appId))
                                 .arg(notification.category.isEmpty()
                                              ? QStringLiteral("(none)")
                                              : notification.category)
                                 .arg(pending.add.categoryId)
                                 .arg(QString::fromStdString(pending.add.title),
                                       QString::fromStdString(pending.add.message));
    if (m_dryRun)
        return;

    m_sent.insert(notification.id, pending.add.notificationId);
    if (m_queue.size() >= kMaxQueued)
        m_queue.removeFirst();
    m_queue.append(pending);
    pump();
}

void NotifierDaemon::onClosed(quint32 id)
{
    if (m_dryRun || !m_sent.contains(id))
        return;
    Pending pending;
    pending.isRemoval = true;
    pending.removeId = m_sent.take(id);
    if (m_queue.size() >= kMaxQueued)
        m_queue.removeFirst();
    m_queue.append(pending);
    pump();
}

void NotifierDaemon::letGo()
{
    m_attaching = false;
    // Unconditionally: detach() is documented to drop whatever state there
    // is, and "not ready yet" can also mean "half way through attaching".
    m_client->detach();
    m_link.release();
    m_retry->stop();
}

void NotifierDaemon::pump()
{
    if (m_dryRun || m_busy)
        return;

    if (m_queue.isEmpty()) {
        // Nothing to do, so hold nothing: the application should be able to
        // pick the watch up without waiting for us.
        letGo();
        return;
    }

    if (m_watch.objectPath.isEmpty()) {
        // Nothing has been paired in the application yet.
        return;
    }

    if (!m_connected) {
        // Open the connection ourselves. The watch drops the link within
        // seconds of the last client letting go, so by the time a
        // notification arrives there is usually nothing to attach to.
        if (!m_connecting) {
            m_connecting = true;
            qInfo() << "connecting to" << m_watch.name;
            m_adapter->connectToDevice(m_watch.objectPath);
        }
        return;
    }

    QString error;
    if (!m_link.tryClaim(&error)) {
        qInfo().noquote() << QStringLiteral("waiting for the watch: %1").arg(error);
        m_retry->start();
        return;
    }
    m_retry->stop();

    if (!m_client->isReady()) {
        // attachToDevice() answers through readyChanged(), which calls back
        // in here. Once is enough: starting it again would throw away the
        // attempt already in progress.
        if (!m_attaching) {
            m_attaching = true;
            m_client->attachToDevice(m_watch.objectPath);
        }
        return;
    }

    const Pending pending = m_queue.takeFirst();
    m_busy = true;
    if (pending.isRemoval) {
        const quint32 id = pending.removeId;
        m_client->removeNotification(id, [this, id](bool ok, const QString &failure) {
            m_busy = false;
            if (ok)
                qInfo() << "took notification" << id << "off the watch";
            else
                qDebug() << "removal failed:" << failure;
            pump();
        });
    } else {
        const quint32 id = pending.add.notificationId;
        m_client->sendNotification(pending.add, [this, id](bool ok, const QString &failure) {
            m_busy = false;
            // Logged on success too. This is the line that says the whole
            // thing worked, and it used to be the only event in the daemon
            // that produced no output at all.
            if (ok)
                qInfo() << "the watch took notification" << id;
            else
                qWarning() << "sending failed:" << failure;
            pump();
        });
    }
}
