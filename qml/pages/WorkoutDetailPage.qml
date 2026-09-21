import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    // Set by MainPage.qml's pageStack.push(url, properties) - deliberately
    // just the summary fields WorkoutListModel already has in memory, no
    // separate GET /v1/workouts/{key} detail fetch yet (that would add
    // extensions/tss/hrdata - out of scope until something needs them).
    property string activityName: ""
    property double startTime: 0
    property double totalTime: 0
    property double totalDistance: 0
    property double totalAscent: 0
    property double totalDescent: 0

    function formatDuration(seconds) {
        var h = Math.floor(seconds / 3600)
        var m = Math.floor((seconds % 3600) / 60)
        var s = Math.floor(seconds % 60)
        return h > 0
                ? qsTr("%1h %2min %3s").arg(h).arg(m).arg(s)
                : qsTr("%1min %2s").arg(m).arg(s)
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
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                columns: 2
                spacing: Theme.paddingLarge

                Column {
                    width: (parent.width - Theme.paddingLarge) / 2
                    Label {
                        text: qsTr("Distance")
                        color: Theme.secondaryColor
                        font.pixelSize: Theme.fontSizeExtraSmall
                    }
                    Label {
                        text: qsTr("%1 km").arg((page.totalDistance / 1000).toFixed(2))
                        font.pixelSize: Theme.fontSizeLarge
                    }
                }
                Column {
                    width: (parent.width - Theme.paddingLarge) / 2
                    Label {
                        text: qsTr("Duration")
                        color: Theme.secondaryColor
                        font.pixelSize: Theme.fontSizeExtraSmall
                    }
                    Label {
                        text: page.formatDuration(page.totalTime)
                        font.pixelSize: Theme.fontSizeLarge
                    }
                }
                Column {
                    width: (parent.width - Theme.paddingLarge) / 2
                    Label {
                        text: qsTr("Ascent")
                        color: Theme.secondaryColor
                        font.pixelSize: Theme.fontSizeExtraSmall
                    }
                    Label {
                        text: qsTr("%1 m").arg(page.totalAscent.toFixed(0))
                        font.pixelSize: Theme.fontSizeLarge
                    }
                }
                Column {
                    width: (parent.width - Theme.paddingLarge) / 2
                    Label {
                        text: qsTr("Descent")
                        color: Theme.secondaryColor
                        font.pixelSize: Theme.fontSizeExtraSmall
                    }
                    Label {
                        text: qsTr("%1 m").arg(page.totalDescent.toFixed(0))
                        font.pixelSize: Theme.fontSizeLarge
                    }
                }
            }
        }
    }
}
