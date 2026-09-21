import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    property string lastError: ""

    // Deliberately incomplete: only the activity IDs actually seen in
    // testing/suuntool's own example output are named - Suunto's full
    // activity-type table isn't ported. Falls back to a numeric label.
    function activityName(id) {
        var names = {
            1: qsTr("Running"),
            2: qsTr("Cycling"),
            11: qsTr("Hiking"),
            22: qsTr("Trail running"),
        }
        return names[id] || qsTr("Activity %1").arg(id)
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
                        text: page.activityName(model.activityId)
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
                activityName: page.activityName(model.activityId),
                startTime: model.startTime,
                totalTime: model.totalTime,
                totalDistance: model.totalDistance,
                totalAscent: model.totalAscent,
                totalDescent: model.totalDescent,
            })
        }

        ViewPlaceholder {
            enabled: listView.count === 0
            text: qsTr("No workouts yet")
            hintText: AppController.cloudSignedIn
                      ? qsTr("Pull down and sync to fetch your workout history")
                      : qsTr("Pair a Suunto watch or sign in to your Suunto account to get started")
        }
    }
}
