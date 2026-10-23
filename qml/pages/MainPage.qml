import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0
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

    // Suunto's "Progress": chronic and acute load and the balance. Shown
    // only with a season behind it - a CTL with a fortnight of history is a
    // number, not an estimate.
    property var progress: ({ ctl: 0, atl: 0, tsb: 0, days: 0 })

    function refreshMonth() {
        month = AppController.monthSummary()
        progress = AppController.trainingProgress()
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

    // Both id vocabularies, complete, extracted from the official Android
    // app's own ActivityMapping enum - see docs/activity-types.md. The two
    // are genuinely different (Cycling is 4 on the watch and 2 in the
    // cloud), which is why source has to be passed in.
    function activityName(id, source) {
        return ActivityTypes.name(id, source)
    }

    // A chain runs several steps, so the entry says which step is running
    // rather than merely that something is.
    property bool syncBusy: AppController.workoutSyncInProgress
                            || AppController.healthSyncInProgress
                            || AppController.uploadInProgress

    function syncBusyText() {
        if (AppController.workoutSyncInProgress) {
            return AppController.workoutSyncKind === "cloud"
                   ? qsTr("Syncing workouts from the cloud…")
                   : qsTr("Syncing workouts from the watch…")
        }
        if (AppController.healthSyncInProgress)
            return qsTr("Syncing health data…")
        return qsTr("Uploading to the cloud…")
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
    }

    // Health is the one page that is useful without an account and without
    // opening a workout, so it gets the swipe rather than a pull-down entry.
    //
    // This cannot go in Component.onCompleted. MainPage is the
    // ApplicationWindow's initialPage, so it is constructed *inside* the page
    // stack's own push, and PageStack.js refuses any stack modification
    // started from there - modifyPageStack() throws "Cannot pushAttached
    // while operation is in progress: push". The throw is swallowed as a QML
    // error, which is why the symptom was a missing swipe indicator and
    // nothing else: every earlier line of onCompleted had already run.
    // Waiting for Active puts the call after that push has finished.
    //
    // The nextPage() guard means a swipe back to the list does not rebuild
    // HealthPage, so its state and its database reads survive the round
    // trip, while anything that does take the slot above this page gets
    // re-attached on return.
    onStatusChanged: {
        if (status === PageStatus.Active && !pageStack.nextPage(page)) {
            pageStack.pushAttached(Qt.resolvedUrl("HealthPage.qml"))
        }
        // The list opened and came back from Health scrolled away from the
        // top, so the title and the watch's status were off screen until
        // you scrolled up - reported from the forum thread. The header is
        // this list's own, and it grows after the first layout as the data
        // arrives: the error line, the watch line, the fitness figures and
        // the month's totals each appear when their source answers, and a
        // ListView keeps its content offset while the content above it
        // gets taller.
        //
        // **The mechanism is a reading, not a measurement** - it fits the
        // code and the symptom, and this is a mitigation either way: on
        // becoming active, after the relayout, put the view back at the
        // top. Deferred by a zero-length timer because doing it inside the
        // status change happens before the header has resized.
        // Activating, not just Active: on Active the page is already on
        // screen and the correction is a visible jump - a tenth of a
        // second of it, reported. During the transition it is not seen.
        if (status === PageStatus.Activating || status === PageStatus.Active)
            pinTimer.restart()
    }

    Timer {
        id: pinTimer
        interval: 0
        onTriggered: listView.positionViewAtBeginning()
    }

    Connections {
        target: AppController
        onErrorOccurred: page.lastError = message
    }

    Component {
        id: fitPicker

        FilePickerPage {
            nameFilters: ["*.fit", "*.FIT"]
            title: qsTr("Choose a FIT file")
            onSelectedContentPropertiesChanged: {
                if (selectedContentProperties)
                    AppController.importFitFile(selectedContentProperties.filePath)
            }
        }
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
                // Unlike Health, which is one swipe away, this one is the
                // cloud's alone: the records
                // are the account's own, computed server-side over a
                // history that reaches back further than any watch keeps.
                visible: AppController.cloudSignedIn
                text: qsTr("Records")
                onClicked: pageStack.push(Qt.resolvedUrl("RecordsPage.qml"))
            }
            MenuItem {
                // Unlike Health, which is one swipe away, this one is the
                // cloud's alone: the records
                // are the account's own, computed server-side over a
                // history that reaches back further than any watch keeps.
                visible: AppController.cloudSignedIn
                text: qsTr("SuuntoPlus")
                onClicked: pageStack.push(Qt.resolvedUrl("SuuntoPlusPage.qml"))
            }
            // **One sync entry, not two, and which one depends on what is
            // reachable.** Two entries that both said "Syncing…" were
            // reported as confusing from the forum thread, and the deeper
            // problem was that neither touched health data: a person who
            // synced from here still had to open the Health page and sync
            // again there.
            //
            // So: no watch means the cloud chain, no account means the
            // watch chain, both means everything in both directions. Each
            // chain skips the steps it cannot run, which is what makes one
            // entry honest in every state.
            MenuItem {
                text: page.syncBusy ? page.syncBusyText() : qsTr("Sync everything")
                enabled: !page.syncBusy
                visible: AppController.watchConnected && AppController.cloudSignedIn
                onClicked: AppController.runSyncChain("all")
            }
            MenuItem {
                text: page.syncBusy ? page.syncBusyText() : qsTr("Sync from the watch")
                enabled: !page.syncBusy
                visible: AppController.watchConnected && !AppController.cloudSignedIn
                onClicked: AppController.runSyncChain("watch")
            }
            MenuItem {
                text: page.syncBusy ? page.syncBusyText() : qsTr("Sync with the cloud")
                enabled: !page.syncBusy
                visible: !AppController.watchConnected && AppController.cloudSignedIn
                onClicked: AppController.runSyncChain("cloud")
            }
            MenuItem {
                // A FIT file from anywhere - Edge Sync writes them to
                // ~/Documents/EdgeSync, and a Garmin Connect or Strava
                // export is the same format.
                text: qsTr("Import a FIT file")
                onClicked: pageStack.push(fitPicker)
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

            // CTL, ATL and the balance between them. The stress figures are
            // Suunto's own, so these are comparable with its app rather
            // than a second opinion computed from duration.
            Item {
                width: parent.width
                height: visible ? progressColumn.height + Theme.paddingLarge : 0
                visible: page.progress.days >= 42

                Column {
                    id: progressColumn
                    width: parent.width

                Row {
                    id: progressRow
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: Theme.paddingLarge * 2

                    Repeater {
                        model: [
                            { value: Math.round(page.progress.ctl), label: qsTr("Fitness") },
                            { value: Math.round(page.progress.atl), label: qsTr("Fatigue") },
                            { value: page.progress.tsb >= 0
                                     ? "+" + Math.round(page.progress.tsb)
                                     : Math.round(page.progress.tsb),
                              label: qsTr("Form") }
                        ]
                        Column {
                            Label {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: modelData.value
                                font.pixelSize: Theme.fontSizeMedium
                                color: Theme.primaryColor
                            }
                            Label {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: modelData.label
                                font.pixelSize: Theme.fontSizeExtraSmall
                                color: Theme.secondaryColor
                            }
                        }
                    }
                }

                // Only the cloud scores a workout, so watch-synced ones
                // move nothing here. Without this line the figures just
                // look wrong: three sessions read off the watch, the
                // official app twenty points higher, and no hint why.
                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * x
                    visible: page.progress.unscored > 0
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: Theme.highlightColor
                    text: qsTr("%n workout(s) newer than these figures have no training "
                               + "stress score yet - sync the cloud.", "",
                               page.progress.unscored)
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
                    // An imported file is a third origin, and it matters
                    // which: this one was recorded by something that is not
                    // the watch at all, so its fields are whatever that
                    // device wrote and the cloud has it only if we sent it.
                    Label {
                        visible: model.source === "fit"
                        text: qsTr("imported")
                        color: Theme.secondaryHighlightColor
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
                        visible: model.source === "ble" || model.source === "fit"
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
            hintText: (AppController.cloudSignedIn || AppController.watchConnected)
                      ? qsTr("Pull down and sync to fetch your workout history")
                      : qsTr("Pair a Suunto watch or sign in to your Suunto account to get started")
        }
    }
}
