#ifdef QT_QML_DEBUG
#include <QtQuick>
#endif

#include <sailfishapp.h>
#include <QGuiApplication>
#include <QQuickView>
#include <QQmlContext>
#include <QScopedPointer>
#include <QLocale>
#include <QTranslator>

#include "controller/appcontroller.h"

int main(int argc, char *argv[])
{
    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));

    QTranslator translator;
    const QString translationsDir = SailfishApp::pathTo(QStringLiteral("translations")).toLocalFile();
    if (translator.load(QLocale(), QStringLiteral("harbour-suuntosync"), QStringLiteral("-"),
                         translationsDir)) {
        app->installTranslator(&translator);
    }

    // Sailjail whitelists (and persists) ~/.local/share/<OrganizationName>/
    // <ApplicationName> based on the .desktop file's [X-Sailjail] values -
    // QStandardPaths::AppDataLocation (used for the local SQLite caches) has
    // to be built from exactly those same values, or the app ends up
    // reading/writing a directory Sailjail never actually persists across
    // launches. Keep these two values in lockstep with
    // harbour-suuntosync.desktop's [X-Sailjail] section (see the equivalent
    // comment/lesson-learned in harbour-otpcove's main.cpp).
    app->setOrganizationName(QStringLiteral("io.github.jarnose"));
    app->setApplicationName(QStringLiteral("suuntosync"));

    QScopedPointer<QQuickView> view(SailfishApp::createView());

    AppController controller;
    view->rootContext()->setContextProperty(QStringLiteral("AppController"), &controller);

    view->setSource(SailfishApp::pathToMainQml());
    view->show();

    return app->exec();
}
