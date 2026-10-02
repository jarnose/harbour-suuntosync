// The notification daemon. Separate from the application on purpose: see
// src/notify/notifierdaemon.h and docs/notifications.md.

#include "notify/filelogger.h"
#include "notify/notifierdaemon.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>
#include <QTimer>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // Exactly the application's own values, because the point is to read
    // the application's database - QStandardPaths builds the path out of
    // these, and Sailjail persists that directory for the app under the
    // same two names (see src/harbour-suuntosync.cpp's comment).
    app.setOrganizationName(QStringLiteral("io.github.jarnose"));
    app.setApplicationName(QStringLiteral("suuntosync"));

    bool dryRun = false;
    QString databasePath;
    QString logPath;
    QString readPath;
    QString writePath;
    int writeValue = -1;
    const QStringList arguments = app.arguments();
    for (int i = 1; i < arguments.size(); ++i) {
        const QString argument = arguments.at(i);
        if (argument == QStringLiteral("--dry-run")) {
            dryRun = true;
        } else if (argument == QStringLiteral("--database") && i + 1 < arguments.size()) {
            databasePath = arguments.at(++i);
        } else if (argument == QStringLiteral("--log") && i + 1 < arguments.size()) {
            logPath = arguments.at(++i);
        } else if (argument == QStringLiteral("--no-log")) {
            logPath = QStringLiteral("-");
        } else if (argument == QStringLiteral("--read") && i + 1 < arguments.size()) {
            readPath = arguments.at(++i);
        } else if (argument == QStringLiteral("--write-enum") && i + 2 < arguments.size()) {
            writePath = arguments.at(++i);
            writeValue = arguments.at(++i).toInt();
        } else {
            qCritical().noquote()
                    << QStringLiteral("usage: %1 [--dry-run] [--database <file>] "
                                       "[--log <file>|--no-log]\n"
                                       "       %1 --read <watch resource path>\n"
                                       "       %1 --write-enum <watch resource path> <0-255>")
                               .arg(arguments.value(0));
            return 2;
        }
    }

    // Before anything that logs. The journal on this device keeps seconds of
    // history (see notify/filelogger.h), so this is where the daemon's own
    // trail actually lives.
    if (logPath.isEmpty())
        logPath = FileLogger::defaultPath();
    if (logPath != QStringLiteral("-") && !FileLogger::install(logPath))
        qWarning().noquote() << QStringLiteral("could not open %1 for logging").arg(logPath);

    if (databasePath.isEmpty()) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        databasePath = QDir(dir).filePath(QStringLiteral("suuntosync.sqlite"));
    }

    NotifierDaemon daemon(databasePath, dryRun);
    QString error;

    // A one-shot probe: the watch half only, no bus monitoring, and the
    // process exits when the answer arrives. This is how a question about
    // one of the watch's own settings gets answered without anybody
    // tapping anything.
    const bool oneShot = !readPath.isEmpty() || !writePath.isEmpty();
    if (oneShot) {
        int status = 1;
        QObject::connect(&daemon, &NotifierDaemon::finished, &app, [&app, &status](bool ok) {
            status = ok ? 0 : 1;
            app.quit();
        });
        if (!daemon.start(&error, false)) {
            qCritical().noquote() << QStringLiteral("could not start: %1").arg(error);
            return 1;
        }
        if (!readPath.isEmpty())
            daemon.requestRead(readPath);
        else
            daemon.requestWriteEnum(writePath, static_cast<quint8>(writeValue));

        // Nothing to wait for for ever: a watch out of range should end the
        // process rather than hold a terminal open.
        QTimer::singleShot(60000, &app, [&app]() {
            qCritical() << "gave up waiting for the watch";
            app.quit();
        });
        app.exec();
        return status;
    }

    if (!daemon.start(&error)) {
        qCritical().noquote() << QStringLiteral("could not start: %1").arg(error);
        return 1;
    }
    return app.exec();
}
