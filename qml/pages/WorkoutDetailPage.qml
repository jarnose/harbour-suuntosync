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
    property double epoc: 0
    property double peakTrainingEffect: 0
    property double recoveryTime: 0
    property double maxVo2: 0
    property double trainingLoad: 0
    // WorkoutStore key, so the route can be looked up. Empty for a workout
    // opened from somewhere that doesn't know it.
    property string workoutKey: ""
    property string source: ""

    // [{x, y}] in 0..1, already aspect-corrected and fitted - see
    // AppController::workoutRoute(). Empty unless this is a BLE workout
    // synced after route storage existed.
    property var route: workoutKey.length > 0 ? AppController.workoutRoute(workoutKey) : []

    // Everything else the watch recorded - see
    // AppController::workoutDetails(). Collapsed by default: it's a hundred
    //-odd fields and the curated stats above are what anyone actually
    // wants, but throwing the rest away after going to the trouble of
    // decoding it would be silly.
    property var details: workoutKey.length > 0 ? AppController.workoutDetails(workoutKey) : []
    property bool detailsExpanded: false

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
        ]
        // Everything below is optional, 0 meaning absent rather than a
        // genuine zero - which is also exactly how the watch's own schema
        // marks these fields (nillable=0). A BLE workout gets ascent and
        // descent from the watch's /Summary when that fetch succeeded, and
        // from its own altitude series otherwise; the training metrics come
        // only from /Summary, so a cloud workout never has them.
        if (totalAscent > 0)
            entries.push({ label: qsTr("Ascent"), value: qsTr("%1 m").arg(totalAscent.toFixed(0)) })
        if (totalDescent > 0)
            entries.push({ label: qsTr("Descent"), value: qsTr("%1 m").arg(totalDescent.toFixed(0)) })
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
        if (peakTrainingEffect > 0)
            entries.push({ label: qsTr("Peak training effect"), value: peakTrainingEffect.toFixed(1) })
        if (epoc > 0)
            entries.push({ label: qsTr("EPOC"), value: qsTr("%1 ml/kg").arg(epoc.toFixed(1)) })
        if (trainingLoad > 0)
            entries.push({ label: qsTr("Training load"), value: trainingLoad.toFixed(0) })
        if (maxVo2 > 0)
            entries.push({ label: qsTr("Estimated VO2max"), value: qsTr("%1 ml/kg/min").arg(maxVo2.toFixed(1)) })
        if (recoveryTime > 0)
            entries.push({ label: qsTr("Recovery time"), value: formatDuration(recoveryTime) })
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
                description: {
                    var when = Qt.formatDateTime(new Date(page.startTime), "d.M.yyyy HH:mm")
                    if (page.source === "ble")
                        return qsTr("%1 · from watch").arg(when)
                    if (page.source.length > 0)
                        return qsTr("%1 · from Suunto cloud").arg(when)
                    return when
                }
            }

            // The recorded GPS track, drawn as a plain polyline. Deliberately
            // no map tiles underneath: that would mean a tile provider,
            // network traffic and attribution, whereas the route's own shape
            // is what makes a workout recognisable and it costs nothing to
            // draw offline.
            Item {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                height: visible ? width * 0.75 : 0
                visible: page.route.length > 1

                Canvas {
                    id: routeCanvas
                    anchors.fill: parent
                    renderStrategy: Canvas.Cooperative

                    // A Canvas only repaints when asked - it has no idea
                    // page.route is something onPaint reads.
                    Connections {
                        target: page
                        onRouteChanged: routeCanvas.requestPaint()
                    }

                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.reset()
                        var pts = page.route
                        if (pts.length < 2)
                            return

                        // Inset so the stroke isn't clipped at the edges.
                        var pad = Theme.paddingMedium
                        var w = width - 2 * pad
                        var h = height - 2 * pad
                        // The projection fits a square, so keep it square
                        // here too and centre it in whatever box we got.
                        var size = Math.min(w, h)
                        var ox = pad + (w - size) / 2
                        var oy = pad + (h - size) / 2

                        ctx.lineWidth = 3
                        ctx.lineJoin = "round"
                        ctx.lineCap = "round"
                        ctx.strokeStyle = Theme.highlightColor
                        ctx.beginPath()
                        ctx.moveTo(ox + pts[0].x * size, oy + pts[0].y * size)
                        for (var i = 1; i < pts.length; ++i)
                            ctx.lineTo(ox + pts[i].x * size, oy + pts[i].y * size)
                        ctx.stroke()

                        // Start and finish, so the direction is readable.
                        function dot(p, colour) {
                            ctx.fillStyle = colour
                            ctx.beginPath()
                            ctx.arc(ox + p.x * size, oy + p.y * size, 5, 0, 2 * Math.PI)
                            ctx.fill()
                        }
                        dot(pts[0], Theme.primaryColor)
                        dot(pts[pts.length - 1], Theme.highlightColor)
                    }
                }
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

            // Everything the watch recorded beyond the stats above. Behind
            // a tap because it is a hundred-odd fields in the watch's own
            // naming, useful to have but not to lead with.
            Item { width: 1; height: Theme.paddingLarge }

            Button {
                visible: page.details.length > 0
                anchors.horizontalCenter: parent.horizontalCenter
                text: page.detailsExpanded
                      ? qsTr("Hide all recorded fields")
                      : qsTr("All recorded fields (%1)").arg(page.details.length)
                onClicked: page.detailsExpanded = !page.detailsExpanded
            }

            Column {
                width: parent.width
                visible: page.detailsExpanded

                Repeater {
                    model: page.detailsExpanded ? page.details : []

                    Row {
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * x
                        spacing: Theme.paddingMedium

                        Label {
                            width: parent.width * 0.6
                            text: modelData.name
                            color: Theme.secondaryColor
                            font.pixelSize: Theme.fontSizeExtraSmall
                            wrapMode: Text.WrapAnywhere
                        }
                        Label {
                            width: parent.width * 0.4 - Theme.paddingMedium
                            horizontalAlignment: Text.AlignRight
                            font.pixelSize: Theme.fontSizeExtraSmall
                            text: {
                                // Integers stay integers; everything else
                                // gets two decimals, which suits the range
                                // these fields span (0.02 to 650000).
                                var v = modelData.value
                                var shown = (Math.abs(v - Math.round(v)) < 0.005)
                                        ? Math.round(v).toString()
                                        : v.toFixed(2)
                                return modelData.unit.length > 0
                                        ? shown + " " + modelData.unit
                                        : shown
                            }
                        }
                    }
                }
            }

            Item { width: 1; height: Theme.paddingLarge }
        }
    }
}
