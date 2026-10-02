// The notification daemon. Separate from the application on purpose: see
// src/notify/notifierdaemon.h and docs/notifications.md.

#include "notify/notifierdaemon.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>

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
    const QStringList arguments = app.arguments();
    for (int i = 1; i < arguments.size(); ++i) {
        const QString argument = arguments.at(i);
        if (argument == QStringLiteral("--dry-run")) {
            dryRun = true;
        } else if (argument == QStringLiteral("--database") && i + 1 < arguments.size()) {
            databasePath = arguments.at(++i);
        } else {
            qCritical().noquote()
                    << QStringLiteral("usage: %1 [--dry-run] [--database <file>]")
                               .arg(arguments.value(0));
            return 2;
        }
    }

    if (databasePath.isEmpty()) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        databasePath = QDir(dir).filePath(QStringLiteral("suuntosync.sqlite"));
    }

    NotifierDaemon daemon(databasePath, dryRun);
    QString error;
    if (!daemon.start(&error)) {
        qCritical().noquote() << QStringLiteral("could not start: %1").arg(error);
        return 1;
    }
    return app.exec();
}
