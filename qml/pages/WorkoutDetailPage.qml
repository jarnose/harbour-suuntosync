import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    // Set by MainPage.qml's pageStack.push(url, properties) - all from
    // WorkoutListModel's already-in-memory fields (the /v1/workouts *list*
    // response already carries these, no separate GET /v1/workouts/{key}
    // detail fetch needed - see workout.h). 0 means "absent" for the
    // optional ones (maxSpeed/energyConsumption/stepCount/heart rate) -
    // statEntries() below omits a row rather than showing a bogus "0".
    property string activityName: ""
    property double startTime: 0
    property double totalTime: 0
    property double totalDistance: 0
    property double totalAscent: 0
    property double totalDescent: 0
    property double maxSpeed: 0
    property double energyConsumption: 0
    property int stepCount: 0
    property double avgHeartRate: 0
    property double maxHeartRate: 0

    function formatDuration(seconds) {
        var h = Math.floor(seconds / 3600)
        var m = Math.floor((seconds % 3600) / 60)
        var s = Math.floor(seconds % 60)
        return h > 0
                ? qsTr("%1h %2min %3s").arg(h).arg(m).arg(s)
                : qsTr("%1min %2s").arg(m).arg(s)
    }

    function statEntries() {
        var entries = [
            { label: qsTr("Distance"), value: qsTr("%1 km").arg((totalDistance / 1000).toFixed(2)) },
            { label: qsTr("Duration"), value: formatDuration(totalTime) },
            { label: qsTr("Ascent"), value: qsTr("%1 m").arg(totalAscent.toFixed(0)) },
            { label: qsTr("Descent"), value: qsTr("%1 m").arg(totalDescent.toFixed(0)) },
        ]
        if (avgHeartRate > 0)
            entries.push({ label: qsTr("Avg heart rate"), value: qsTr("%1 bpm").arg(avgHeartRate.toFixed(0)) })
        if (maxHeartRate > 0)
            entries.push({ label: qsTr("Max heart rate"), value: qsTr("%1 bpm").arg(maxHeartRate.toFixed(0)) })
        if (maxSpeed > 0)
            entries.push({ label: qsTr("Max speed"), value: qsTr("%1 km/h").arg((maxSpeed * 3.6).toFixed(1)) })
        if (energyConsumption > 0)
            entries.push({ label: qsTr("Energy"), value: qsTr("%1 kcal").arg(energyConsumption.toFixed(0)) })
        if (stepCount > 0)
            entries.push({ label: qsTr("Steps"), value: stepCount.toString() })
        return entries
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column
            width: parent.width

            PageHeader {
                title: page.activityName
                description: Qt.formatDateTime(new Date(page.startTime), "d.M.yyyy HH:mm")
            }

            Grid {
                id: grid
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                columns: 2
                spacing: Theme.paddingLarge

                Repeater {
                    model: page.statEntries()

                    Column {
                        width: (grid.width - Theme.paddingLarge) / 2
                        Label {
                            text: modelData.label
                            color: Theme.secondaryColor
                            font.pixelSize: Theme.fontSizeExtraSmall
                        }
                        Label {
                            text: modelData.value
                            font.pixelSize: Theme.fontSizeLarge
                        }
                    }
                }
            }
        }
    }
}
