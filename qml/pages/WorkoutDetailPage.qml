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
    property double trainingStressScore: 0
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
    // Not a binding: a cloud workout's extensions arrive from the network
    // after the page is already up, so this is reloaded when they land.
    property var details: []

    // Whether this workout can still be pushed to the cloud. Not a binding:
    // it depends on stored state that only changes when we change it.
    property bool canUpload: false

    function reloadDetails() {
        details = workoutKey.length > 0 ? AppController.workoutDetails(workoutKey) : []
        series = workoutKey.length > 0 ? AppController.workoutSeries(workoutKey) : []
        canUpload = workoutKey.length > 0 && AppController.canUploadWorkout(workoutKey)

        // A cloud workout's training metrics arrive with the extensions,
        // after this page is already up. The store is updated too, but
        // filling them in here means they appear now rather than on the
        // next open. Only when empty, so a watch workout's own figures are
        // never overwritten.
        for (var i = 0; i < details.length; ++i) {
            var f = details[i]
            if (epoc === 0 && f.name === "SummaryExtension.peakEpoc")
                epoc = f.value
            else if (peakTrainingEffect === 0 && f.name === "SummaryExtension.pte")
                peakTrainingEffect = f.value
            else if (recoveryTime === 0 && f.name === "SummaryExtension.recoveryTime")
                recoveryTime = f.value
        }
    }

    Component.onCompleted: {
        reloadDetails()
        // No-op for a BLE workout or one already fetched - see
        // AppController::loadCloudDetails().
        AppController.loadCloudDetails(workoutKey)
    }

    // The sample-data download is the one thing on this page that can fail
    // in a way worth telling the user about, and it's a deliberate action
    // with no other feedback, so its error gets a persistent banner rather
    // than a toast - same reasoning as PairingPage.qml's.
    property string lastError: ""

    Connections {
        target: AppController
        onWorkoutDetailsChanged: {
            if (key === page.workoutKey) {
                page.lastError = ""
                page.reloadDetails()
            }
        }
        onErrorOccurred: page.lastError = message
        onWorkoutUploaded: {
            if (key !== page.workoutKey)
                return
            page.lastError = message
            page.canUpload = AppController.canUploadWorkout(page.workoutKey)
        }
    }

    // Per-sample curves, already reduced to a drawable number of points -
    // see AppController::workoutSeries(). Not a binding, for the same
    // reason details isn't: a cloud workout's curves only exist after the
    // pull-down fetch below has run.
    property var series: []
    property var laps: workoutKey.length > 0 ? AppController.workoutLaps(workoutKey) : []
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
        if (trainingStressScore > 0)
            entries.push({ label: qsTr("TSS"), value: trainingStressScore.toFixed(0) })
        if (maxVo2 > 0)
            entries.push({ label: qsTr("Estimated VO2max"), value: qsTr("%1 ml/kg/min").arg(maxVo2.toFixed(1)) })
        if (recoveryTime > 0)
            entries.push({ label: qsTr("Recovery time"), value: formatDuration(recoveryTime) })
        return entries
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        PullDownMenu {
            busy: AppController.cloudSamplesInProgress

            // Only offered where it can actually do something: a watch
            // workout already arrives with its curves, and once a cloud
            // workout's have been downloaded there's nothing to repeat.
            // The download is several megabytes, which is exactly why this
            // is a deliberate tap rather than something the page does on
            // its own when opened.
            MenuItem {
                // Only for a watch-synced workout the cloud hasn't taken
                // yet. Deliberately a separate tap rather than part of the
                // sync: putting a workout on someone's public-ish account
                // is their decision.
                text: qsTr("Upload to Suunto")
                visible: page.canUpload
                onClicked: AppController.uploadWorkoutToCloud(page.workoutKey)
            }
            MenuItem {
                text: qsTr("Download sample data")
                visible: page.source !== "ble" && page.series.length === 0
                         && page.workoutKey.length > 0
                onClicked: AppController.loadCloudSamples(page.workoutKey)
            }
        }

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

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: page.lastError.length > 0
                text: page.lastError
                wrapMode: Text.Wrap
                color: Theme.highlightColor
                font.pixelSize: Theme.fontSizeExtraSmall
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

            // Laps, when the workout has any - a plain outing with no
            // auto-lap and no button presses has none, so this stays out of
            // the way rather than showing a one-row table of the whole
            // workout.
            Item {
                width: 1
                height: page.laps.length > 1 ? Theme.paddingLarge : 0
            }

            Column {
                width: parent.width
                visible: page.laps.length > 1

                Label {
                    x: Theme.horizontalPageMargin
                    text: qsTr("Laps")
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                }

                Repeater {
                    model: page.laps

                    Row {
                        x: Theme.horizontalPageMargin
                        width: page.width - 2 * x
                        spacing: Theme.paddingMedium

                        Label {
                            width: parent.width * 0.12
                            text: modelData.number
                            color: Theme.secondaryColor
                        }
                        Label {
                            width: parent.width * 0.3
                            text: page.formatDuration(modelData.durationSeconds)
                        }
                        Label {
                            width: parent.width * 0.28
                            text: modelData.distanceMeters > 0
                                  ? qsTr("%1 km").arg((modelData.distanceMeters / 1000).toFixed(2))
                                  : ""
                        }
                        Label {
                            width: parent.width * 0.3 - 3 * Theme.paddingMedium
                            text: modelData.type
                            color: Theme.secondaryColor
                            font.pixelSize: Theme.fontSizeExtraSmall
                            truncationMode: TruncationMode.Fade
                        }
                    }
                }
            }

            // Heart rate, altitude and whatever else the workout recorded
            // often enough to be worth a line. Same deal as the route: a
            // plain Canvas, no charting library, no network.
            Repeater {
                model: page.series

                Column {
                    width: page.width
                    spacing: Theme.paddingSmall

                    Item { width: 1; height: Theme.paddingLarge }

                    Label {
                        x: Theme.horizontalPageMargin
                        text: {
                            // The watch's own field names, tidied: strip the
                            // "Sample." prefix and split the camel case.
                            var n = modelData.name.replace("Sample.", "")
                            n = n.replace(/([a-z])([A-Z])/g, "$1 $2")
                            return n
                        }
                        color: Theme.secondaryColor
                        font.pixelSize: Theme.fontSizeExtraSmall
                    }

                    Item {
                        x: Theme.horizontalPageMargin
                        width: page.width - 2 * x
                        height: Theme.itemSizeMedium * 1.5

                        Canvas {
                            id: seriesCanvas
                            anchors.fill: parent
                            renderStrategy: Canvas.Cooperative

                            Connections {
                                target: page
                                onSeriesChanged: seriesCanvas.requestPaint()
                            }

                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.reset()
                                var pts = modelData.points
                                if (!pts || pts.length < 2)
                                    return

                                var lo = modelData.min
                                var hi = modelData.max
                                var span = hi - lo
                                if (span <= 0)
                                    return

                                var pad = 2
                                var h = height - 2 * pad
                                ctx.lineWidth = 2
                                ctx.lineJoin = "round"
                                ctx.strokeStyle = Theme.highlightColor
                                ctx.beginPath()
                                for (var i = 0; i < pts.length; ++i) {
                                    var x = width * i / (pts.length - 1)
                                    var y = pad + h * (1 - (pts[i] - lo) / span)
                                    if (i === 0)
                                        ctx.moveTo(x, y)
                                    else
                                        ctx.lineTo(x, y)
                                }
                                ctx.stroke()
                            }
                        }

                        // Axis extremes rather than a full axis: with 200
                        // averaged buckets the line is indicative, so a
                        // precise grid would overstate what it shows.
                        Label {
                            anchors { left: parent.left; top: parent.top }
                            text: modelData.max.toFixed(modelData.max < 10 ? 1 : 0)
                                  + " " + modelData.unit
                            color: Theme.secondaryColor
                            font.pixelSize: Theme.fontSizeExtraSmall
                        }
                        Label {
                            anchors { left: parent.left; bottom: parent.bottom }
                            text: modelData.min.toFixed(modelData.min < 10 ? 1 : 0)
                                  + " " + modelData.unit
                            color: Theme.secondaryColor
                            font.pixelSize: Theme.fontSizeExtraSmall
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
                // Not anchors.horizontalCenter: a Column positions its
                // children and ignores their anchors (with a warning). It
                // leaves x alone, though.
                x: (parent.width - width) / 2
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
