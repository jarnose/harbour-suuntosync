import QtQuick 2.0
import Sailfish.Silica 1.0
import "../pages/ActivityTypes.js" as ActivityTypes

// What the cover shows is a setting, because there is no one right answer:
// somebody training for a distance wants lifetime totals, somebody tracking
// recovery wants last night, and somebody who just uses the app to sync
// wants neither. See AppController::coverMode().
CoverBackground {
    id: cover

    property var summary: ({})

    function refresh() {
        summary = AppController.coverSummary()
    }

    Component.onCompleted: refresh()

    Connections {
        target: AppController
        // There is no "workouts changed" signal, so this keys off the sync
        // finishing - which is when the list has actually changed. Checked
        // against AppController's signal list rather than assumed; an
        // invented signal name fails silently in QML.
        onWorkoutSyncInProgressChanged: {
            if (!AppController.workoutSyncInProgress)
                cover.refresh()
        }
        onHealthDataChanged: cover.refresh()
        onCoverModeChanged: cover.refresh()
    }

    function formatDuration(seconds) {
        var h = Math.floor(seconds / 3600)
        var m = Math.floor((seconds % 3600) / 60)
        return h > 0 ? h + " h " + m + " min" : m + " min"
    }

    Label {
        id: title
        anchors {
            top: parent.top
            topMargin: Theme.paddingLarge
            horizontalCenter: parent.horizontalCenter
        }
        text: "Suunto Sync"
        color: Theme.secondaryColor
        font.pixelSize: Theme.fontSizeExtraSmall
        visible: AppController.coverMode !== "nothing"
    }

    // ---- latest workout ----
    Column {
        anchors {
            centerIn: parent
            verticalCenterOffset: Theme.paddingSmall
        }
        width: parent.width - 2 * Theme.paddingMedium
        spacing: Theme.paddingSmall
        visible: AppController.coverMode === "latest" && cover.summary.hasWorkout === true

        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: cover.summary.hasWorkout
                  ? ActivityTypes.name(cover.summary.activityId, cover.summary.source)
                  : ""
            color: Theme.primaryColor
            font.pixelSize: Theme.fontSizeSmall
            truncationMode: TruncationMode.Fade
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: cover.summary.hasWorkout
                  ? (cover.summary.distance > 0
                     ? (cover.summary.distance / 1000).toFixed(1) + " km"
                     : cover.formatDuration(cover.summary.duration))
                  : ""
            color: Theme.highlightColor
            font.pixelSize: Theme.fontSizeLarge
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: cover.summary.hasWorkout
                  ? Qt.formatDateTime(new Date(cover.summary.startTime), "d.M.")
                  : ""
            color: Theme.secondaryColor
            font.pixelSize: Theme.fontSizeExtraSmall
        }
    }

    // ---- lifetime totals ----
    Column {
        anchors {
            centerIn: parent
            verticalCenterOffset: Theme.paddingSmall
        }
        width: parent.width - 2 * Theme.paddingMedium
        spacing: Theme.paddingSmall
        visible: AppController.coverMode === "totals" && cover.summary.hasWorkout === true

        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: cover.summary.hasWorkout
                  ? (cover.summary.totalDistance / 1000).toFixed(0) + " km"
                  : ""
            color: Theme.highlightColor
            font.pixelSize: Theme.fontSizeLarge
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: cover.summary.hasWorkout
                  ? cover.formatDuration(cover.summary.totalTime) : ""
            color: Theme.primaryColor
            font.pixelSize: Theme.fontSizeExtraSmall
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            //: %1 is a number of workouts
            text: cover.summary.hasWorkout
                  ? qsTr("%1 workouts").arg(cover.summary.count) : ""
            color: Theme.secondaryColor
            font.pixelSize: Theme.fontSizeExtraSmall
        }
    }

    // ---- last night ----
    Column {
        anchors {
            centerIn: parent
            verticalCenterOffset: Theme.paddingSmall
        }
        width: parent.width - 2 * Theme.paddingMedium
        spacing: Theme.paddingSmall
        visible: AppController.coverMode === "sleep" && cover.summary.hasSleep === true

        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: cover.summary.hasSleep
                  ? cover.formatDuration(cover.summary.sleepDuration) : ""
            color: Theme.highlightColor
            font.pixelSize: Theme.fontSizeLarge
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            // Quality is 0..1 and can be absent - the watch does not always
            // measure it, and an absent value must not show as 0 %.
            text: (cover.summary.hasSleep && cover.summary.sleepQuality > 0)
                  ? Math.round(cover.summary.sleepQuality * 100) + " %"
                  : ""
            color: Theme.primaryColor
            font.pixelSize: Theme.fontSizeExtraSmall
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: cover.summary.hasSleep
                  ? Qt.formatDateTime(new Date(cover.summary.sleepStart), "d.M.") : ""
            color: Theme.secondaryColor
            font.pixelSize: Theme.fontSizeExtraSmall
        }
    }

    // Nothing to show yet - better than an empty cover that looks broken.
    Label {
        anchors.centerIn: parent
        width: parent.width - 2 * Theme.paddingMedium
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        visible: AppController.coverMode !== "nothing"
                 && ((AppController.coverMode === "sleep" && cover.summary.hasSleep !== true)
                     || (AppController.coverMode !== "sleep"
                         && cover.summary.hasWorkout !== true))
        text: qsTr("Nothing synced yet")
        color: Theme.secondaryColor
        font.pixelSize: Theme.fontSizeExtraSmall
    }

    CoverActionList {
        id: syncAction
        enabled: AppController.whiteboardReady && !AppController.workoutSyncInProgress

        CoverAction {
            iconSource: "image://theme/icon-cover-sync"
            onTriggered: AppController.syncWatchWorkouts()
        }
    }
}
