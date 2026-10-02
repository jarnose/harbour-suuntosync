#include "filelogger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QStandardPaths>
#include <QTextStream>

#include <cstdio>

namespace FileLogger {

namespace {

// Small enough not to matter on a phone, large enough to hold a day of a
// quiet daemon and the whole of a noisy incident.
const qint64 kMaxBytes = 256 * 1024;

QMutex g_mutex;
QString g_path;
QtMessageHandler g_previous = nullptr;

const char *levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return "debug";
    case QtInfoMsg:     return "info";
    case QtWarningMsg:  return "warning";
    case QtCriticalMsg: return "critical";
    case QtFatalMsg:    return "fatal";
    }
    return "?";
}

void rotateIfNeeded()
{
    if (QFileInfo(g_path).size() < kMaxBytes)
        return;
    // One previous generation, overwritten. Two would need a policy and this
    // does not deserve one.
    const QString previous = g_path + QStringLiteral(".1");
    QFile::remove(previous);
    QFile::rename(g_path, previous);
}

void handler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    // stderr as well, always: running the daemon by hand must still show
    // everything, and systemd will take this to the journal for whatever
    // that is worth here.
    if (g_previous)
        g_previous(type, context, message);

    QMutexLocker locker(&g_mutex);
    rotateIfNeeded();
    QFile file(g_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    QTextStream out(&file);
    out.setCodec("UTF-8");
    out << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
        << QLatin1Char(' ') << QLatin1Char('[') << levelName(type) << QLatin1Char(']')
        << QLatin1Char(' ') << message << QLatin1Char('\n');
}

} // namespace

QString defaultPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return QDir(dir).filePath(QStringLiteral("suuntosync-notifyd.log"));
}

bool install(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile probe(path);
    if (!probe.open(QIODevice::WriteOnly | QIODevice::Append))
        return false;
    probe.close();

    g_path = path;
    g_previous = qInstallMessageHandler(handler);
    return true;
}

} // namespace FileLogger
