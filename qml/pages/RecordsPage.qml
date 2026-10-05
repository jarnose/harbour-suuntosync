import QtQuick 2.0
import Sailfish.Silica 1.0
import "ActivityTypes.js" as ActivityTypes

// The account's personal bests, as the cloud keeps them: all-time and, in a
// separate set the response sends alongside, this year.
//
// A deliberate restraint runs through this page. The cloud sends each record
// as a bare number with a type name, and only some of those types have a
// unit this project has actually established: the distances and the
// durations do, from arithmetic that only works out one way (5 km in 1552.9
// of something is 5:11 per km in seconds and nonsense in anything else). The
// speeds and the pace do not - the running pace record reads 3.22 all-time
// against 2.64 this year, and in minutes per kilometre that would make this
// year faster than the all-time best, which cannot be. So those are shown as
// the figure the server sent, with no unit invented for them. See
// docs/workout-upload.md.
Page {
    id: page

    property var groups: []

    function reload() {
        groups = AppController.personalRecords()
    }

    Component.onCompleted: {
        reload()
        // Only when there is nothing cached. An all-time best changes a few
        // times a year, so re-asking on every open would be a request for
        // an answer we already have - the pull-down is there for when
        // something has just been set.
        if (groups.length === 0)
            AppController.syncPersonalRecords()
    }

    Connections {
        target: AppController
        onPersonalRecordsChanged: page.reload()
    }

    // Record type -> what to call it. In the page rather than in
    // ActivityTypes.js because these need qsTr(), which a ".pragma library"
    // has no context for.
    function recordLabel(type) {
        switch (type) {
        case "LongestDistance":  return qsTr("Longest distance")
        case "LongestDuration":  return qsTr("Longest duration")
        case "MaxAscent":        return qsTr("Most ascent")
        case "FastestPace":      return qsTr("Fastest pace")
        case "MaxSpeed":         return qsTr("Top speed")
        case "MaxAvgSpeed":      return qsTr("Best average speed")
        case "MaxAvgPower":      return qsTr("Best average power")
        case "HalfMarathon":     return qsTr("Half marathon")
        case "FullMarathon":     return qsTr("Marathon")
        default:
            // KM5, KM10, KM20, KM40, KM180 - a distance split, named by its
            // own number rather than by a table that would need extending
            // every time the cloud adds one.
            var km = type.match(/^KM([0-9]+)$/)
            if (km)
                return qsTr("%1 km").arg(km[1])
            // Anything this build has not seen shows its own name, which
            // beats hiding a record because it is new.
            return type.replace(/([a-z])([A-Z])/g, "$1 $2")
        }
    }

    // Which of the three units this project is willing to state, or none.
    function recordKind(type) {
        switch (type) {
        case "LongestDistance": return "distance"
        case "MaxAscent":       return "ascent"
        case "LongestDuration":
        case "HalfMarathon":
        case "FullMarathon":    return "duration"
        default:
            return /^KM[0-9]+$/.test(type) ? "duration" : "raw"
        }
    }

    function formatDuration(seconds) {
        var h = Math.floor(seconds / 3600)
        var m = Math.floor((seconds % 3600) / 60)
        var s = Math.floor(seconds % 60)
        function pad(n) { return n < 10 ? "0" + n : String(n) }
        return h > 0 ? h + ":" + pad(m) + ":" + pad(s) : m + ":" + pad(s)
    }

    function formatRecord(type, value) {
        switch (recordKind(type)) {
        case "distance": return qsTr("%1 km").arg((value / 1000).toFixed(2))
        case "ascent":   return qsTr("%1 m").arg(value.toFixed(0))
        case "duration": return formatDuration(value)
        default:
            // No unit, on purpose - see the note at the top of this file.
            return (Math.abs(value - Math.round(value)) < 0.005)
                    ? Math.round(value).toString()
                    : value.toFixed(2)
        }
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        PullDownMenu {
            busy: AppController.recordsSyncInProgress

            MenuItem {
                text: AppController.recordsSyncInProgress
                      ? qsTr("Fetching…") : qsTr("Fetch records")
                enabled: !AppController.recordsSyncInProgress
                onClicked: AppController.syncPersonalRecords()
            }
        }

        Column {
            id: column
            width: page.width

            PageHeader { title: qsTr("Records") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: AppController.recordsFetchedAt > 0
                //: %1 is a date, e.g. 5.10.2026
                text: qsTr("Fetched %1").arg(
                          Qt.formatDateTime(new Date(AppController.recordsFetchedAt), "d.M.yyyy"))
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                visible: page.groups.length === 0
                text: AppController.recordsSyncInProgress
                      ? qsTr("Fetching…")
                      : (AppController.cloudSignedIn
                         ? qsTr("No records yet. Pull down to fetch them.")
                         : qsTr("Sign in to the Suunto cloud to see records."))
                color: Theme.secondaryHighlightColor
                font.pixelSize: Theme.fontSizeSmall
            }

            Repeater {
                model: page.groups

                Column {
                    width: parent.width

                    SectionHeader {
                        // The cloud's own activity-id space, not the
                        // watch's - these come from the cloud.
                        text: ActivityTypes.name(modelData.activityId, "cloud")
                    }

                    Repeater {
                        model: modelData.rows

                        Column {
                            // The outer modelData, named: the inner Repeater
                            // below shadows it with its own, and a silently
                            // undefined property is exactly the QML failure
                            // mode that shows as a blank row.
                            property var record: modelData

                            width: parent.width
                            spacing: Theme.paddingSmall

                            Label {
                                x: Theme.horizontalPageMargin
                                width: parent.width - 2 * x
                                text: page.recordLabel(record.type)
                                color: Theme.primaryColor
                                font.pixelSize: Theme.fontSizeSmall
                                truncationMode: TruncationMode.Fade
                            }

                            Repeater {
                                // The two sets, each omitted when the
                                // account has no record of that type in it.
                                model: [
                                    { show: record.hasAllTime,
                                      label: qsTr("All time"),
                                      value: record.value,
                                      date: record.date },
                                    { show: record.hasYear,
                                      label: qsTr("This year"),
                                      value: record.yearValue,
                                      date: record.yearDate }
                                ]

                                Row {
                                    property var row: modelData
                                    x: Theme.horizontalPageMargin
                                    width: parent.width - 2 * x
                                    // A Column already skips an invisible
                                    // child, so no height override here.
                                    visible: row.show === true
                                    spacing: Theme.paddingMedium

                                    Label {
                                        width: parent.width * 0.26
                                        text: row.label
                                        color: Theme.secondaryColor
                                        font.pixelSize: Theme.fontSizeExtraSmall
                                    }
                                    Label {
                                        width: parent.width * 0.34
                                        horizontalAlignment: Text.AlignRight
                                        text: page.formatRecord(record.type, row.value)
                                        color: Theme.highlightColor
                                        font.pixelSize: Theme.fontSizeExtraSmall
                                    }
                                    Label {
                                        width: parent.width * 0.4 - 2 * Theme.paddingMedium
                                        horizontalAlignment: Text.AlignRight
                                        text: row.date > 0
                                              ? Qt.formatDateTime(new Date(row.date), "d.M.yyyy")
                                              : ""
                                        color: Theme.secondaryColor
                                        font.pixelSize: Theme.fontSizeExtraSmall
                                    }
                                }
                            }

                            Item { width: 1; height: Theme.paddingSmall }
                        }
                    }
                }
            }

            Item { width: 1; height: Theme.paddingLarge }
        }

        VerticalScrollDecorator {}
    }
}
