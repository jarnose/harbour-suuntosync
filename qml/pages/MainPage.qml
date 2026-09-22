import QtQuick 2.0
import Sailfish.Silica 1.0
import "ActivityTypes.js" as ActivityTypes

Page {
    id: page

    property string lastError: ""

    // Both id vocabularies, complete, extracted from the official Android
    // app's own ActivityMapping enum - see docs/activity-types.md. The two
    // are genuinely different (Cycling is 4 on the watch and 2 in the
    // cloud), which is why source has to be passed in.
    function activityName(id, source) {
        return ActivityTypes.name(id, source)
    }

    function formatDuration(seconds) {
        var h = Math.floor(seconds / 3600)
        var m = Math.floor((seconds % 3600) / 60)
        return h > 0 ? qsTr("%1h %2min").arg(h).arg(m) : qsTr("%1min").arg(m)
    }

    Component.onCompleted: AppController.loadCachedWorkouts()

    Connections {
        target: AppController
        onErrorOccurred: page.lastError = message
    }

    SilicaListView {
        id: listView
        anchors.fill: parent
        model: AppController.workoutModel

        PullDownMenu {
            MenuItem {
                text: AppController.watchPaired ? qsTr("Change watch") : qsTr("Pair a watch")
                onClicked: pageStack.push(Qt.resolvedUrl("PairingPage.qml"))
            }
            MenuItem {
                text: AppController.cloudSignedIn ? qsTr("Sign out of Suunto") : qsTr("Sign in to Suunto")
                onClicked: {
                    if (AppController.cloudSignedIn)
                        AppController.logoutFromCloud()
                    else
                        pageStack.push(Qt.resolvedUrl("LoginPage.qml"))
                }
            }
            MenuItem {
                visible: AppController.cloudSignedIn
                text: AppController.workoutSyncInProgress ? qsTr("Syncing…") : qsTr("Sync workouts")
                enabled: !AppController.workoutSyncInProgress
                onClicked: AppController.syncCloudWorkouts()
            }
            MenuItem {
                visible: AppController.whiteboardReady
                text: AppController.workoutSyncInProgress ? qsTr("Syncing…") : qsTr("Sync from watch")
                enabled: !AppController.workoutSyncInProgress
                onClicked: AppController.syncWatchWorkouts()
            }
        }

        header: Column {
            width: parent.width

            PageHeader {
                title: AppController.appName
            }

            Label {
                visible: page.lastError.length > 0
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.WordWrap
                color: Theme.errorColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: page.lastError
            }

            Label {
                visible: AppController.watchPaired
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                truncationMode: TruncationMode.Fade
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: AppController.watchConnected
                      ? qsTr("%1 · Connected").arg(AppController.pairedWatchName)
                      : qsTr("%1 · Not connected").arg(AppController.pairedWatchName)
            }

            Label {
                visible: AppController.cloudSignedIn
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                truncationMode: TruncationMode.Fade
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("Signed in as %1").arg(AppController.cloudEmail)
            }
        }

        delegate: ListItem {
            id: delegateItem
            contentHeight: Theme.itemSizeMedium

            Column {
                anchors {
                    left: parent.left
                    right: parent.right
                    leftMargin: Theme.horizontalPageMargin
                    rightMargin: Theme.horizontalPageMargin
                    verticalCenter: parent.verticalCenter
                }
                spacing: Theme.paddingSmall

                Row {
                    width: parent.width
                    spacing: Theme.paddingSmall

                    Label {
                        text: page.activityName(model.activityId, model.source)
                        color: delegateItem.highlighted ? Theme.highlightColor : Theme.primaryColor
                    }
                    Label {
                        text: Qt.formatDateTime(new Date(model.startTime), "d.M.yyyy HH:mm")
                        color: Theme.secondaryColor
                        font.pixelSize: Theme.fontSizeExtraSmall
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                Label {
                    text: qsTr("%1 km · %2")
                          .arg((model.totalDistance / 1000).toFixed(2))
                          .arg(page.formatDuration(model.totalTime))
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                }
            }

            onClicked: pageStack.push(Qt.resolvedUrl("WorkoutDetailPage.qml"), {
                activityName: page.activityName(model.activityId, model.source),
                startTime: model.startTime,
                totalTime: model.totalTime,
                totalDistance: model.totalDistance,
                totalAscent: model.totalAscent,
                totalDescent: model.totalDescent,
                maxSpeed: model.maxSpeed,
                energyConsumption: model.energyConsumption,
                stepCount: model.stepCount,
                avgHeartRate: model.avgHeartRate,
                maxHeartRate: model.maxHeartRate,
                epoc: model.epoc,
                peakTrainingEffect: model.peakTrainingEffect,
                recoveryTime: model.recoveryTime,
                maxVo2: model.maxVo2,
                trainingLoad: model.trainingLoad,
                workoutKey: model.key,
            })
        }

        ViewPlaceholder {
            enabled: listView.count === 0
            text: qsTr("No workouts yet")
            hintText: (AppController.cloudSignedIn || AppController.whiteboardReady)
                      ? qsTr("Pull down and sync to fetch your workout history")
                      : qsTr("Pair a Suunto watch or sign in to your Suunto account to get started")
        }
    }
}
