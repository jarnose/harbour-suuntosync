#include "notifierdaemon.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
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
// How long a notification is worth delivering. Two minutes is long enough to
// cover a watch that was simply asleep and short enough that nobody is
// surprised by what turns up on their wrist.
const qint64 kMaxAgeMs = 120000;

// How many times one thing is offered to the watch before giving up on it.
// A failure used to end the attempt, which cost a real notification every
// time the link went quiet for ten seconds - and cost more than that for a
// removal, since a ring nobody takes off the watch stays there until it is
// dismissed by hand.
//
// This is deliberately larger than the age bound above will ever allow at a
// ten-second timeout, so **kMaxAgeMs is the real limit** and this is only
// here to stop a request that fails *instantly* from spinning. Four was
// tried first and was measurably too few: a Race that is displaying an
// incoming-call alert answers no Whiteboard request at all - four removals
// timed out in a row over forty seconds, and on an earlier call the
// missed-call notification queued behind one timed out with it - so the
// retry has to outlast the alert rather than the link.
const int kMaxAttempts = 16;
// How long to give an attach before deciding it is not going to happen.
// A good one takes about a second.
const int kAttachTimeoutMs = 20000;

// How long a ring may sit on the watch with no end in sight before it is
// taken off anyway.
//
// Ninety seconds is past any ring a caller waits out, so in normal
// operation this never fires: the call ends, the status change arrives, and
// the ring goes within a second. It fires when the end was missed - and if a
// call *is* answered and still talking at ninety seconds, taking the ring
// off is right rather than wrong, since the watch should have stopped
// showing an incoming call the moment it was answered.
const int kRingTimeoutMs = 90000;
// Two identical notifications this close together are one notification
// posted twice, not two messages. Long enough to catch a double post, short
// enough that two genuinely identical messages a few seconds apart both get
// through.
const qint64 kDuplicateWindowMs = 3000;
} // namespace

NotifierDaemon::NotifierDaemon(const QString &databasePath, bool dryRun, QObject *parent)
    : QObject(parent)
    , m_store(databasePath)
    , m_dryRun(dryRun)
    , m_adapter(new BluezAdapter(this))
    , m_client(new MdsWhiteboardClient(this))
    , m_monitor(new NotificationMonitor(this))
    , m_calls(new CallMonitor(this))
    , m_contacts(new ContactLookup(this))
    , m_retry(new QTimer(this))
    , m_attachTimeout(new QTimer(this))
    , m_ringTimeout(new QTimer(this))
    , m_link(QFileInfo(databasePath).absolutePath(), WatchLink::Background)
{
    m_retry->setInterval(kRetryMs);
    connect(m_retry, &QTimer::timeout, this, &NotifierDaemon::pump);

    m_ringTimeout->setSingleShot(true);
    m_ringTimeout->setInterval(kRingTimeoutMs);
    connect(m_ringTimeout, &QTimer::timeout, this, [this]() {
        if (m_ringing.isEmpty())
            return;
        // Every ring still on the watch, since there is no telling which
        // one lost its end. In practice there is never more than one.
        const QList<QString> stale = m_ringing.keys();
        for (const QString &path : stale) {
            qWarning().noquote()
                    << QStringLiteral("no end seen for call %1 in %2 s - clearing it anyway")
                               .arg(path)
                               .arg(kRingTimeoutMs / 1000);
            onCallEnded(path);
        }
    });

    m_attachTimeout->setSingleShot(true);
    m_attachTimeout->setInterval(kAttachTimeoutMs);
    connect(m_attachTimeout, &QTimer::timeout, this, [this]() {
        qWarning() << "the watch never finished opening a session; letting go";
        letGo();
        m_retry->start();
    });
}

bool NotifierDaemon::start(QString *error, bool withMonitor)
{
    if (withMonitor) {
        connect(m_monitor, &NotificationMonitor::posted, this, &NotifierDaemon::onPosted);
        connect(m_monitor, &NotificationMonitor::closed, this, &NotifierDaemon::onClosed);
        if (!m_monitor->start(error))
            return false;
        qInfo() << "watching the session bus via"
                << (m_monitor->isMonitor() ? "BecomeMonitor" : "legacy eavesdropping");

        // A second subscription, for the one thing the notification server
        // never hears about: a call that is still ringing. Not fatal if it
        // fails - everything else still works, and saying so beats a daemon
        // that is quietly half deaf.
        connect(m_calls, &CallMonitor::incomingCall, this, &NotifierDaemon::onIncomingCall);
        connect(m_calls, &CallMonitor::callEnded, this, &NotifierDaemon::onCallEnded);
        QString callError;
        if (!m_calls->start(&callError))
            qWarning().noquote() << "not watching for incoming calls:" << callError;

        // Opened here rather than on the first call, so the cost of opening
        // the address book is not paid while a phone is ringing. Failing is
        // survivable: a call then shows its number, which is what it did
        // before this existed.
        QString contactError;
        if (!m_contacts->open(&contactError))
            qWarning().noquote() << "no contact names for calls:" << contactError;

        if (m_dryRun) {
            qInfo() << "dry run: nothing will be sent to a watch";
            return true;
        }
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
        if (ready)
            m_attachTimeout->stop();
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
    const PairedWatch previous = m_watch;

    QString error;
    m_watch = m_store.load(&error);
    if (!error.isEmpty()) {
        qWarning() << "could not read the paired watch:" << error;
        return;
    }
    if (m_watch.address == previous.address)
        return;

    if (m_watch.address.isEmpty()) {
        qInfo() << "no watch is paired in the application yet";
    } else {
        qInfo() << "paired watch:" << m_watch.name << m_watch.address;
    }

    if (previous.address.isEmpty())
        return;

    // The user switched watches in the application. Everything in hand
    // belongs to the old one - the session, the lock, the belief that
    // something is connected - so put it all down. Without this the daemon
    // went on addressing the watch that was swapped out, which is the kind
    // of wrong that looks like it works.
    qInfo() << "the application switched watches; letting the old one go";
    letGo();
    m_connected = false;
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

void NotifierDaemon::onConnectFinished(const QString &objectPath, bool ok, const QString &error,
                                       const QString &errorName)
{
    if (objectPath != m_watch.objectPath)
        return;
    m_connecting = false;
    if (!ok && errorName == QLatin1String(BluezError::kInProgress)) {
        // Somebody else's Connect() is outstanding - the application's, or
        // our own previous one that BlueZ has not finished unwinding. Wait
        // for it rather than calling this a failure; three of these in a row
        // is what a recovering connection looks like.
        qDebug() << "a connection is already being opened; waiting";
        m_retry->start();
        return;
    }
    if (!ok && errorName != QLatin1String(BluezError::kAlreadyConnected)) {
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

    // The same thing twice in a row, within a moment: one notification the
    // phone posted twice. See m_lastContent.
    const QString content = QStringLiteral("%1|%2|%3").arg(
            QString::fromStdString(candidate.appId),
            QString::fromStdString(candidate.title),
            QString::fromStdString(candidate.message));
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (content == m_lastContent && nowMs - m_lastContentAtMs < kDuplicateWindowMs) {
        qDebug().noquote() << QStringLiteral("ignoring notification %1: the same one again")
                                      .arg(notification.id);
        return;
    }
    m_lastContent = content;
    m_lastContentAtMs = nowMs;

    Pending pending;
    pending.queuedAtMs = QDateTime::currentMSecsSinceEpoch();
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
    pending.queuedAtMs = QDateTime::currentMSecsSinceEpoch();
    pending.kind = Pending::Remove;
    pending.removeId = m_sent.take(id);
    if (m_queue.size() >= kMaxQueued)
        m_queue.removeFirst();
    m_queue.append(pending);
    pump();
}

void NotifierDaemon::onIncomingCall(const QString &callPath, const QString &lineId)
{
    // The call's own object path carries a fresh identifier per call, so its
    // hash is a stable id for this call and a different one for the next.
    const quint32 callId = qHash(callPath);
    const QString caller = m_contacts->nameFor(lineId);
    const Ancs::Notification notification = NotificationRouter::incomingCall(
            caller.toStdString(), lineId.toStdString(), callId,
            static_cast<uint32_t>(QDateTime::currentMSecsSinceEpoch() / 1000));

    qInfo().noquote() << QStringLiteral("incoming call -> ancs 1: %1 / %2")
                                 .arg(QString::fromStdString(notification.title),
                                       QString::fromStdString(notification.message));
    if (m_dryRun)
        return;

    Pending pending;
    pending.queuedAtMs = QDateTime::currentMSecsSinceEpoch();
    pending.add = notification;
    m_ringing.insert(callPath, notification.notificationId);
    // Restarted rather than started, so a second call while the first is
    // still ringing does not shorten the net for the second one.
    m_ringTimeout->start();
    if (m_queue.size() >= kMaxQueued)
        m_queue.removeFirst();
    m_queue.append(pending);
    pump();
}

void NotifierDaemon::onCallEnded(const QString &callPath)
{
    if (m_dryRun || !m_ringing.contains(callPath))
        return;
    Pending pending;
    pending.queuedAtMs = QDateTime::currentMSecsSinceEpoch();
    pending.kind = Pending::Remove;
    pending.removeId = m_ringing.take(callPath);
    if (m_ringing.isEmpty())
        m_ringTimeout->stop();
    if (m_queue.size() >= kMaxQueued)
        m_queue.removeFirst();
    m_queue.append(pending);
    pump();
}

void NotifierDaemon::requestRead(const QString &path)
{
    Pending pending;
    pending.kind = Pending::Read;
    pending.path = path;
    pending.queuedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_oneShot = true;
    m_queue.append(pending);
}

void NotifierDaemon::requestWriteEnum(const QString &path, quint8 value)
{
    Pending pending;
    pending.kind = Pending::WriteEnum;
    pending.path = path;
    pending.value = value;
    pending.queuedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_oneShot = true;
    m_queue.append(pending);
}

void NotifierDaemon::letGo()
{
    m_attaching = false;
    m_attachTimeout->stop();
    // Unconditionally: detach() is documented to drop whatever state there
    // is, and "not ready yet" can also mean "half way through attaching".
    m_client->detach();
    m_link.release();
    m_retry->stop();
}

bool NotifierDaemon::watchIsConnected()
{
    if (m_watch.objectPath.isEmpty())
        return false;
    QDBusInterface properties(QStringLiteral("org.bluez"), m_watch.objectPath,
                               QStringLiteral("org.freedesktop.DBus.Properties"),
                               QDBusConnection::systemBus());
    const QDBusReply<QVariant> reply = properties.call(
            QStringLiteral("Get"), QStringLiteral("org.bluez.Device1"),
            QStringLiteral("Connected"));
    return reply.isValid() && reply.value().toBool();
}

// Puts a failed piece of work back at the head of the queue, so the next
// pump offers it again. At the head rather than the tail because order is
// load-bearing: a removal that overtook its own arrival would take nothing
// off the watch and then leave the notification on it for ever.
void NotifierDaemon::requeue(const Pending &pending, const QString &what)
{
    Pending again = pending;
    again.attempts = pending.attempts + 1;
    if (again.attempts >= kMaxAttempts) {
        qWarning().noquote() << QStringLiteral("giving up on %1 after %2 attempts")
                                        .arg(what)
                                        .arg(again.attempts);
        return;
    }
    qInfo().noquote() << QStringLiteral("retrying %1, attempt %2").arg(what).arg(again.attempts + 1);
    // The age check in pump() is the other bound, and the one that matters:
    // queuedAtMs is deliberately not refreshed here, so retrying cannot keep
    // something alive past the two minutes it is worth showing for.
    m_queue.prepend(again);
}

void NotifierDaemon::pump()
{
    if (m_dryRun || m_busy)
        return;

    // Forget anything too old to be worth showing. Done here rather than on
    // a timer so that the queue emptying also stops the retries.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = m_queue.size() - 1; !m_oneShot && i >= 0; --i) {
        if (now - m_queue.at(i).queuedAtMs <= kMaxAgeMs)
            continue;
        if (m_queue.at(i).kind == Pending::Add)
            qInfo() << "giving up on a notification the watch never took";
        m_queue.removeAt(i);
    }

    if (m_queue.isEmpty()) {
        // Nothing to do, so hold nothing: the application should be able to
        // pick the watch up without waiting for us.
        letGo();
        return;
    }

    // The active watch is the application's to choose, and it can change
    // while the daemon is running. Cheap enough to re-read whenever there is
    // something to deliver - one indexed SELECT on a local file - and the
    // alternative is sending to whichever watch was active at boot.
    reloadWatch();

    if (m_watch.objectPath.isEmpty()) {
        // Nothing has been paired in the application yet.
        return;
    }

    // Ground truth, every time. See watchIsConnected().
    m_connected = watchIsConnected();

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

    // The lock comes after the connection, not before it. Claiming it first
    // meant holding it for as long as a connect took - and for ever, if the
    // watch was out of range - which is the one thing the lock exists to
    // avoid doing to the application.
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
            m_attachTimeout->start();
            m_client->attachToDevice(m_watch.objectPath);
        }
        return;
    }

    const Pending pending = m_queue.takeFirst();
    m_busy = true;
    if (pending.kind == Pending::Read) {
        const QString path = pending.path;
        m_client->readValue(path, [this, path](bool ok, const std::vector<uint8_t> &body,
                                                const QString &failure) {
            m_busy = false;
            if (!ok) {
                qCritical().noquote() << QStringLiteral("%1: %2").arg(path, failure);
            } else {
                QString hex;
                for (uint8_t byte : body)
                    hex += QStringLiteral("%1 ").arg(byte, 2, 16, QLatin1Char('0'));
                qInfo().noquote() << QStringLiteral("%1 = %2 bytes: %3")
                                             .arg(path).arg(body.size()).arg(hex.trimmed());
            }
            if (m_oneShot) {
                emit finished(ok);
                return;
            }
            pump();
        });
    } else if (pending.kind == Pending::WriteEnum) {
        const QString path = pending.path;
        const quint8 value = pending.value;
        m_client->putSmallEnum(path, value, [this, path, value](bool ok,
                                                                 const QString &failure) {
            m_busy = false;
            if (ok)
                qInfo().noquote() << QStringLiteral("%1 = %2, written").arg(path).arg(value);
            else
                qCritical().noquote() << QStringLiteral("%1: %2").arg(path, failure);
            if (m_oneShot) {
                emit finished(ok);
                return;
            }
            pump();
        });
    } else if (pending.kind == Pending::Remove) {
        m_client->removeNotification(pending.removeId,
                                      [this, pending](bool ok, const QString &failure) {
            m_busy = false;
            if (ok)
                qInfo() << "took notification" << pending.removeId << "off the watch";
            else
                qDebug() << "removal failed:" << failure;
            if (!ok)
                requeue(pending, QStringLiteral("removal of %1").arg(pending.removeId));
            pump();
        });
    } else {
        m_client->sendNotification(pending.add, [this, pending](bool ok,
                                                                   const QString &failure) {
            m_busy = false;
            // Logged on success too. This is the line that says the whole
            // thing worked, and it used to be the only event in the daemon
            // that produced no output at all.
            if (ok) {
                qInfo() << "the watch took notification" << pending.add.notificationId;
            } else {
                qWarning() << "sending failed:" << failure;
                requeue(pending, QStringLiteral("notification %1")
                                         .arg(pending.add.notificationId));
            }
            pump();
        });
    }
}
