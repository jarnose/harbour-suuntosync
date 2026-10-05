import QtQuick 2.0
import Sailfish.Silica 1.0
import "ActivityTypes.js" as ActivityTypes

Page {
    id: page

    // How many watch-synced workouts the cloud hasn't taken yet. Not a
    // binding: it changes only when a sync or an upload changes it.
    property int pendingUploads: 0

    function refreshPending() {
        pendingUploads = AppController.pendingWorkoutUploads()
    }

    // This month's training, read once rather than bound: it changes only
    // when a sync or a deletion changes the list.
    property var month: ({ count: 0, seconds: 0, month: "" })

    function refreshMonth() {
        month = AppController.monthSummary()
    }

    function formatHours(seconds) {
        var h = Math.floor(seconds / 3600)
        var m = Math.round((seconds % 3600) / 60)
        if (m === 60) { h += 1; m = 0 }
        return h > 0 ? qsTr("%1 h %2 min").arg(h).arg(m) : qsTr("%1 min").arg(m)
    }

    Connections {
        target: AppController
        onWorkoutSyncInProgressChanged: {
            if (!AppController.workoutSyncInProgress) {
                page.refreshPending()
                page.refreshMonth()
            }
        }
        // Uploading changes what is pending, not what was trained.
        onWorkoutUploaded: page.refreshPending()
        onCloudAccountChanged: page.refreshPending()
        onWorkoutDeleted: {
            page.refreshPending()
            page.refreshMonth()
        }
    }

    property string lastError: ""

    // The other remembered watch, when there is exactly one - which is the
    // case that deserves a one-tap switch. With none there is nothing to
    // offer, and with several the choice belongs in Settings rather than in
    // a pull-down entry that would have to name all of them.
    property var otherWatch: null

    function refreshOtherWatch() {
        var known = AppController.knownWatches()
        var others = []
        for (var i = 0; i < known.length; ++i) {
            if (!known[i].active)
                others.push(known[i])
        }
        otherWatch = others.length === 1 ? others[0] : null
    }

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

    Component.onCompleted: {
        AppController.loadCachedWorkouts()
        refreshPending()
        refreshMonth()
        refreshOtherWatch()
    }

    Connections {
        target: AppController
        onErrorOccurred: page.lastError = message
        onPairedWatchChanged: page.refreshOtherWatch()
    }

    SilicaListView {
        id: listView
        anchors.fill: parent
        model: AppController.workoutModel

        PullDownMenu {
            MenuItem {
                // Account and watch used to be two separate pull-down
                // entries here; they live in Settings now.
                text: qsTr("Settings")
                onClicked: pageStack.push(Qt.resolvedUrl("SettingsPage.qml"))
            }
            MenuItem {
                // Sleep and recovery, from the cloud or straight off the
                // watch - so this is useful without an account too.
                text: qsTr("Health")
                onClicked: pageStack.push(Qt.resolvedUrl("HealthPage.qml"))
            }
            MenuItem {
                // Two watches in the house means switching between them is
                // a routine thing, not a setup step, so it belongs here
                // rather than three taps deep in the pairing page.
                visible: page.otherWatch !== null
                text: page.otherWatch
                      ? qsTr("Switch to %1").arg(page.otherWatch.name) : ""
                onClicked: AppController.switchToWatch(page.otherWatch.address)
            }
            // The three transfers, named by direction. "Sync workouts" and
            // "Sync from watch" said nothing about which way data moved,
            // and with three of them that stopped being guessable.
            MenuItem {
                visible: AppController.cloudSignedIn
                text: AppController.workoutSyncInProgress
                      ? qsTr("Syncing…") : qsTr("Sync workouts from cloud")
                enabled: !AppController.workoutSyncInProgress
                onClicked: AppController.syncCloudWorkouts()
            }
            MenuItem {
                visible: AppController.whiteboardReady
                text: AppController.workoutSyncInProgress
                      ? qsTr("Syncing…") : qsTr("Sync from watch to phone")
                enabled: !AppController.workoutSyncInProgress
                onClicked: AppController.syncWatchWorkouts()
            }
            MenuItem {
                // Hidden when there is nothing waiting rather than offered
                // as a no-op, and it says how many - uploading creates real
                // workouts on someone's account, so the count matters.
                visible: AppController.cloudSignedIn && page.pendingUploads > 0
                text: qsTr("Send %1 workouts from phone to cloud").arg(page.pendingUploads)
                enabled: !AppController.workoutSyncInProgress
                onClicked: AppController.uploadAllWorkouts()
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

            // This month, the way Suunto's own app leads with it: how much
            // and how often. Hidden when the month is empty rather than
            // showing a pair of zeroes.
            Item {
                width: parent.width
                height: page.month.count > 0 ? monthRow.height + 2 * Theme.paddingLarge : 0
                visible: page.month.count > 0

                Row {
                    id: monthRow
                    anchors.centerIn: parent
                    spacing: Theme.paddingLarge * 2

                    Column {
                        Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: page.formatHours(page.month.seconds)
                            font.pixelSize: Theme.fontSizeLarge
                            color: Theme.highlightColor
                        }
                        Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: qsTr("this month")
                            font.pixelSize: Theme.fontSizeExtraSmall
                            color: Theme.secondaryColor
                        }
                    }

                    Column {
                        Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: page.month.count
                            font.pixelSize: Theme.fontSizeLarge
                            color: Theme.highlightColor
                        }
                        Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            // Deliberately not "sessions in October": the
                            // month name would have to be inflected in
                            // Finnish - lokakuu becomes lokakuussa - and
                            // QLocale only gives the nominative. "This
                            // month" on the other figure says which month
                            // it is anyway.
                            text: page.month.count === 1
                                  ? qsTr("session") : qsTr("sessions")
                            font.pixelSize: Theme.fontSizeExtraSmall
                            color: Theme.secondaryColor
                        }
                    }
                }
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
                    // A workout synced from the watch and the cloud's copy of
                    // the same ride are separate rows (different key spaces,
                    // see workoutFromDecoded()) and only the watch one has
                    // the training metrics and the route - so say which is
                    // which rather than leaving them indistinguishable.
                    Label {
                        visible: model.source === "ble"
                        text: qsTr("watch")
                        color: Theme.highlightColor
                        font.pixelSize: Theme.fontSizeExtraSmall
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    // Whether the cloud has this one. Only watch workouts
                    // get the marker: a cloud workout is in the cloud by
                    // definition, so the question only arises here.
                    //
                    // icon-s-cloud-upload is the only cloud glyph the
                    // Silica theme has that isn't branded for somebody's
                    // file-sync service - and it lives in icons-monochrome,
                    // so it takes a colour. There is no crossed-out cloud,
                    // hence the strike drawn on top for the negative state.
                    Item {
                        width: cloudIcon.width
                        height: cloudIcon.height
                        visible: model.source === "ble"
                        anchors.verticalCenter: parent.verticalCenter

                        Image {
                            id: cloudIcon
                            source: "image://theme/icon-s-cloud-upload?"
                                    + (model.uploadedToCloud ? Theme.highlightColor
                                                             : Theme.secondaryColor)
                        }
                        // Diagonal bar for "not up there yet". Drawn in the
                        // accent colour over the dimmed glyph so the two
                        // states differ by colour as well as by the bar -
                        // at this size one cue on its own is easy to miss.
                        Rectangle {
                            anchors.centerIn: parent
                            width: Math.round(parent.width * 1.25)
                            height: Math.max(1, Math.round(parent.height / 12))
                            radius: height / 2
                            rotation: -45
                            color: Theme.highlightColor
                            visible: !model.uploadedToCloud
                        }
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
                trainingStressScore: model.trainingStressScore,
                workoutKey: model.key,
                source: model.source,
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
