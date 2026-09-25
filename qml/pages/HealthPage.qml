import QtQuick 2.0
import Sailfish.Silica 1.0

// The watch's round-the-clock data, as the cloud has it. Read-only: this
// shows what the watch already uploaded, so a missing night means the watch
// hasn't synced, not that anything here failed - see
// AppController::syncHealthData().
Page {
    id: page

    property var nights: []
    property var recovery: []
    property var activity: []
    property var stages: []
    property string lastError: ""
    // Not a binding - it changes only when we change it.
    property int pendingUploads: 0

    function reload() {
        // A night per row, so a dozen covers a fortnight. Recovery and
        // activity are sampled every 30 and 10 minutes, so the same count
        // would be half a day - enough for the "latest" readings below
        // without dragging thousands of rows into QML.
        nights = AppController.healthEntries("sleep", 30)
        stages = AppController.healthEntries("sleepstages", 60)
        recovery = AppController.healthEntries("recovery", 48)
        activity = AppController.healthEntries("activity", 144)
        pendingUploads = AppController.pendingHealthUploads()
    }

    Component.onCompleted: reload()

    Connections {
        target: AppController
        onHealthDataChanged: {
            page.lastError = ""
            page.reload()
        }
        onErrorOccurred: page.lastError = message
    }

    // Hertz is the watch's own unit for anything per-minute - see
    // docs/workout-upload.md. Everything shown here converts at the edge
    // rather than in storage, so what is stored stays what was sent.
    function bpm(hz) { return Math.round(hz * 60) }

    function duration(seconds) {
        var h = Math.floor(seconds / 3600)
        var m = Math.round((seconds % 3600) / 60)
        return h > 0 ? qsTr("%1 h %2 min").arg(h).arg(m) : qsTr("%1 min").arg(m)
    }

    function dayOf(ms) {
        return Qt.formatDateTime(new Date(ms), "ddd d.M.")
    }

    // Totals for the newest stored day. A binding rather than a one-shot
    // calculation, so a sync that brings new samples updates the figures
    // instead of leaving yesterday's on screen.
    property var todayTotals: {
        var out = { steps: 0, energy: 0, hr: 0 }
        if (activity.length === 0)
            return out
        var newest = new Date(activity[0].timestamp)
        for (var i = 0; i < activity.length; ++i) {
            var a = activity[i]
            var d = new Date(a.timestamp)
            if (d.getDate() !== newest.getDate() || d.getMonth() !== newest.getMonth())
                continue
            out.steps += a.stepCount || 0
            out.energy += a.energyConsumption || 0
        }
        out.hr = activity[0].hr ? bpm(activity[0].hr) : 0
        return out
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        PullDownMenu {
            busy: AppController.healthSyncInProgress
            MenuItem {
                // Straight from the watch - no Suunto account involved, and
                // it finds nights the watch hasn't uploaded yet.
                visible: AppController.whiteboardReady
                // Sleep, recovery and daily activity - three
                // resources, three mechanisms, one action.
                text: qsTr("Sync health from watch")
                onClicked: AppController.syncWatchHealth()
            }
            MenuItem {
                visible: AppController.cloudSignedIn
                text: qsTr("Sync health data")
                onClicked: AppController.syncHealthData()
            }
            MenuItem {
                // The other direction: push what came off the watch up to
                // Suunto. Hidden when there is nothing waiting rather than
                // offered as a no-op.
                visible: AppController.cloudSignedIn && page.pendingUploads > 0
                text: qsTr("Upload %1 entries to Suunto").arg(page.pendingUploads)
                onClicked: AppController.uploadHealthToCloud()
            }
        }

        Column {
            id: column
            width: parent.width

            PageHeader { title: qsTr("Health") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: page.lastError.length > 0
                text: page.lastError
                wrapMode: Text.Wrap
                color: Theme.highlightColor
                font.pixelSize: Theme.fontSizeExtraSmall
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: page.nights.length === 0 && page.recovery.length === 0
                         && !AppController.healthSyncInProgress
                text: qsTr("Nothing synced yet. Pull down to fetch what the watch has uploaded to the cloud.")
                wrapMode: Text.Wrap
                color: Theme.secondaryHighlightColor
                font.pixelSize: Theme.fontSizeSmall
            }

            // ---- last night ----
            SectionHeader {
                text: qsTr("Last night")
                visible: page.nights.length > 0
            }

            Column {
                width: parent.width
                visible: page.nights.length > 0

                property var n: page.nights.length > 0 ? page.nights[0] : null

                DetailItem {
                    label: qsTr("Slept")
                    value: parent.n ? page.duration(parent.n.duration) : ""
                }
                DetailItem {
                    label: qsTr("Deep")
                    value: parent.n && parent.n.deepSleepDuration !== undefined
                           ? page.duration(parent.n.deepSleepDuration) : ""
                    visible: value.length > 0
                }
                DetailItem {
                    label: qsTr("REM")
                    value: parent.n && parent.n.remSleepDuration !== undefined
                           ? page.duration(parent.n.remSleepDuration) : ""
                    visible: value.length > 0
                }
                DetailItem {
                    label: qsTr("Light")
                    value: parent.n && parent.n.lightSleepDuration !== undefined
                           ? page.duration(parent.n.lightSleepDuration) : ""
                    visible: value.length > 0
                }
                DetailItem {
                    label: qsTr("Quality")
                    value: parent.n && parent.n.quality !== undefined
                           ? Math.round(parent.n.quality * 100) + " %" : ""
                    visible: value.length > 0
                }
                DetailItem {
                    label: qsTr("Avg heart rate")
                    value: parent.n && parent.n.hrAvg ? page.bpm(parent.n.hrAvg) + " bpm" : ""
                    visible: value.length > 0
                }
                DetailItem {
                    label: qsTr("Lowest heart rate")
                    value: parent.n && parent.n.hrMin ? page.bpm(parent.n.hrMin) + " bpm" : ""
                    visible: value.length > 0
                }
                DetailItem {
                    label: qsTr("HRV")
                    value: parent.n && parent.n.avgHrv ? Math.round(parent.n.avgHrv) + " ms" : ""
                    visible: value.length > 0
                }
                DetailItem {
                    label: qsTr("SpO2")
                    value: parent.n && parent.n.maxSpo2
                           ? Math.round(parent.n.maxSpo2 * 100) + " %" : ""
                    visible: value.length > 0
                }
                DetailItem {
                    label: qsTr("Bedtime")
                    value: parent.n
                           ? Qt.formatDateTime(new Date(parent.n.timestamp), "d.M. HH:mm") : ""
                }
            }

            // ---- latest recovery ----
            SectionHeader {
                text: qsTr("Recovery")
                visible: page.recovery.length > 0
            }

            Column {
                width: parent.width
                visible: page.recovery.length > 0
                property var r: page.recovery.length > 0 ? page.recovery[0] : null

                DetailItem {
                    label: qsTr("Resource balance")
                    value: parent.r && parent.r.balance !== undefined
                           ? Math.round(parent.r.balance * 100) + " %" : ""
                }
                DetailItem {
                    label: qsTr("State")
                    // The watch's own enum; no published mapping, so the
                    // number is shown rather than a guessed label.
                    value: parent.r && parent.r.stressState !== undefined
                           ? parent.r.stressState.toString() : ""
                }
                DetailItem {
                    label: qsTr("Measured")
                    value: parent.r
                           ? Qt.formatDateTime(new Date(parent.r.timestamp), "d.M. HH:mm") : ""
                }
            }

            // ---- today's activity ----
            SectionHeader {
                text: qsTr("Activity")
                visible: page.activity.length > 0
            }

            Column {
                width: parent.width
                visible: page.activity.length > 0

                // Totals come from page.todayTotals - see there for why
                // this isn't computed here.
                DetailItem {
                    label: qsTr("Steps today")
                    value: page.todayTotals.steps.toString()
                }
                DetailItem {
                    label: qsTr("Energy today")
                    // The watch reports joules here, as it does for a
                    // workout's Energy field.
                    value: qsTr("%1 kcal").arg(Math.round(page.todayTotals.energy / 4184))
                    visible: page.todayTotals.energy > 0
                }
                DetailItem {
                    label: qsTr("Latest heart rate")
                    value: page.todayTotals.hr + " bpm"
                    visible: page.todayTotals.hr > 0
                }
            }

            // ---- sleep history ----
            SectionHeader {
                text: qsTr("Earlier nights")
                visible: page.nights.length > 1
            }

            Repeater {
                model: page.nights.length > 1 ? page.nights.slice(1) : []

                ListItem {
                    width: page.width
                    contentHeight: Theme.itemSizeSmall

                    Label {
                        x: Theme.horizontalPageMargin
                        anchors.verticalCenter: parent.verticalCenter
                        text: page.dayOf(modelData.timestamp)
                        color: Theme.primaryColor
                        font.pixelSize: Theme.fontSizeSmall
                    }
                    Label {
                        anchors {
                            right: parent.right
                            rightMargin: Theme.horizontalPageMargin
                            verticalCenter: parent.verticalCenter
                        }
                        text: page.duration(modelData.duration)
                              + (modelData.quality !== undefined
                                 ? "  ·  " + Math.round(modelData.quality * 100) + " %" : "")
                        color: Theme.secondaryColor
                        font.pixelSize: Theme.fontSizeSmall
                    }
                }
            }

            Item { width: 1; height: Theme.paddingLarge }
        }

        VerticalScrollDecorator {}
    }
}
