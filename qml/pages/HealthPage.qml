import QtQuick 2.0
import Sailfish.Silica 1.0

// The watch's round-the-clock data, as the cloud has it. Read-only: this
// shows what the watch already uploaded, so a missing night means the watch
// hasn't synced, not that anything here failed - see
// AppController::syncHealthData().
//
// MainPage attaches this page, so it is constructed at app startup and not
// when the user swipes to it. Keep reload() reading the local database only:
// anything slow or networked here would be paid for on every launch, by
// everyone, whether or not they ever swipe. Freshness afterwards comes from
// the healthDataChanged connection below, since the instance outlives a
// swipe away and back.
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
        // Only the newest night is shown on its own; the thirty-day list
        // below gets its sleep from dailyHealth() instead.
        nights = AppController.healthEntries("sleep", 1)
        days = AppController.dailyHealth(30)
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

    // One row per calendar day for the chart and the day list below it.
    // Aggregated in C++ because a day is 144 samples and the store holds
    // two hundred thousand - see AppController::dailyHealth().
    property var days: []

    // Which two things the chart draws. Two series, each with its own
    // scale, because steps and sleep hours share no unit - a single axis
    // would make one of them a flat line.
    property int seriesA: 0
    property int seriesB: 2
    property var seriesKinds: [
        { key: "energyKcal",   label: qsTr("Calories"),     unit: "kcal" },
        { key: "steps",        label: qsTr("Steps"),        unit: "" },
        { key: "sleepSeconds", label: qsTr("Sleep"),        unit: "h" },
        { key: "sleepQuality", label: qsTr("Sleep quality"), unit: "%" }
    ]

    // The chart's own value for a day, in the unit the label promises.
    function seriesValue(day, which) {
        var kind = page.seriesKinds[which].key
        var v = day[kind]
        if (v === undefined || v === null)
            return -1
        if (kind === "sleepSeconds")
            return v > 0 ? v / 3600 : -1
        if (kind === "sleepQuality")
            return v >= 0 ? v * 100 : -1
        if (kind === "energyKcal" || kind === "steps")
            return day.hasActivity ? v : -1
        return v
    }

    // Filled by the canvas as it draws, so the axis labels show the scale
    // each line was actually drawn against.
    property var rangeA: null
    property var rangeB: null

    function axisLabel(value, which) {
        var kind = page.seriesKinds[which]
        var text = value >= 100 ? Math.round(value) : value.toFixed(1)
        return kind.unit.length > 0 ? text + " " + kind.unit : text
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
                visible: AppController.watchConnected
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

            // ---- thirty days, two series ----
            SectionHeader {
                text: qsTr("Last 30 days")
                visible: page.days.length > 0
            }

            // Two pickers rather than four fixed charts: the pair a person
            // wants to see together is theirs to choose, and any two of
            // these share no unit anyway.
            Row {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: page.days.length > 0

                ComboBox {
                    width: parent.width / 2
                    label: qsTr("Line")
                    currentIndex: page.seriesA
                    menu: ContextMenu {
                        Repeater {
                            model: page.seriesKinds
                            MenuItem {
                                text: modelData.label
                                onClicked: page.seriesA = index
                            }
                        }
                    }
                }

                ComboBox {
                    width: parent.width / 2
                    label: qsTr("and")
                    currentIndex: page.seriesB
                    menu: ContextMenu {
                        Repeater {
                            model: page.seriesKinds
                            MenuItem {
                                text: modelData.label
                                onClicked: page.seriesB = index
                            }
                        }
                    }
                }
            }

            Item {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                height: visible ? Theme.itemSizeMedium * 2 : 0
                visible: page.days.length > 0

                Canvas {
                    id: historyCanvas
                    anchors.fill: parent
                    renderStrategy: Canvas.Cooperative

                    // A Canvas repaints only when told to, and all three of
                    // these change under it.
                    Connections {
                        target: page
                        onDaysChanged: historyCanvas.requestPaint()
                        onSeriesAChanged: historyCanvas.requestPaint()
                        onSeriesBChanged: historyCanvas.requestPaint()
                    }

                    // Each series on its own scale, from its own days, so
                    // neither flattens the other. A day with no reading is
                    // -1 and breaks the line rather than being drawn as a
                    // zero - a watch left on the table is not a day of no
                    // steps.
                    function drawSeries(ctx, which, colour) {
                        var values = []
                        for (var i = 0; i < page.days.length; ++i)
                            values.push(page.seriesValue(page.days[i], which))

                        var lo = Number.MAX_VALUE, hi = -Number.MAX_VALUE
                        for (i = 0; i < values.length; ++i) {
                            if (values[i] < 0)
                                continue
                            lo = Math.min(lo, values[i])
                            hi = Math.max(hi, values[i])
                        }
                        if (hi <= -Number.MAX_VALUE)
                            return null
                        // A flat series still deserves a line through the
                        // middle rather than a division by zero.
                        if (hi - lo <= 0) {
                            hi = hi + 1
                            lo = Math.max(0, lo - 1)
                        }

                        var pad = 3
                        var h = height - 2 * pad
                        ctx.lineWidth = 2
                        ctx.lineJoin = "round"
                        ctx.strokeStyle = colour
                        var drawing = false
                        for (i = 0; i < values.length; ++i) {
                            if (values[i] < 0) {
                                if (drawing) {
                                    ctx.stroke()
                                    drawing = false
                                }
                                continue
                            }
                            var x = values.length > 1
                                    ? width * i / (values.length - 1) : width / 2
                            var y = pad + h * (1 - (values[i] - lo) / (hi - lo))
                            if (!drawing) {
                                ctx.beginPath()
                                ctx.moveTo(x, y)
                                drawing = true
                            } else {
                                ctx.lineTo(x, y)
                            }
                        }
                        if (drawing)
                            ctx.stroke()
                        return { min: lo, max: hi }
                    }

                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.reset()
                        if (page.days.length < 2)
                            return
                        page.rangeA = drawSeries(ctx, page.seriesA, Theme.highlightColor)
                        page.rangeB = drawSeries(ctx, page.seriesB, Theme.secondaryColor)
                    }
                }

                // Extremes rather than an axis, as on the workout page: the
                // left pair belongs to the first series, the right pair to
                // the second, in the same colours as the lines.
                Label {
                    anchors { left: parent.left; top: parent.top }
                    visible: page.rangeA !== null
                    text: page.rangeA ? page.axisLabel(page.rangeA.max, page.seriesA) : ""
                    color: Theme.highlightColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                }
                Label {
                    anchors { left: parent.left; bottom: parent.bottom }
                    visible: page.rangeA !== null
                    text: page.rangeA ? page.axisLabel(page.rangeA.min, page.seriesA) : ""
                    color: Theme.highlightColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                }
                Label {
                    anchors { right: parent.right; top: parent.top }
                    visible: page.rangeB !== null
                    text: page.rangeB ? page.axisLabel(page.rangeB.max, page.seriesB) : ""
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                }
                Label {
                    anchors { right: parent.right; bottom: parent.bottom }
                    visible: page.rangeB !== null
                    text: page.rangeB ? page.axisLabel(page.rangeB.min, page.seriesB) : ""
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: page.days.length > 0
                horizontalAlignment: Text.AlignRight
                text: qsTr("%1 - %2").arg(page.dayOf(page.days[0].dayMs))
                                       .arg(page.dayOf(page.days[page.days.length - 1].dayMs))
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
            }

            // ---- days ----
            SectionHeader {
                text: qsTr("Earlier days")
                visible: page.days.length > 1
            }

            Repeater {
                // Newest first and today left out: today's figures are the
                // ones above. A day with no samples at all is skipped here
                // while still breaking the chart's line - the list is a
                // record of what was measured, the chart of when it was
                // not.
                model: page.days.length > 1
                       ? page.days.slice(0, page.days.length - 1).reverse() : []

                ListItem {
                    width: page.width
                    contentHeight: dayContent.height + Theme.paddingMedium
                    // A day with only a night on it is still a day worth
                    // listing - sleep is measured while the step counter
                    // has nothing to say.
                    visible: modelData.hasActivity || modelData.hasSleep

                    Column {
                        id: dayContent
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * x
                        y: Theme.paddingSmall

                        // The day and its activity on one line; the night
                        // under it. Two lines rather than one long string
                        // because four figures do not fit a phone's width,
                        // and the day is the thing being scanned for.
                        Row {
                            width: parent.width

                            Label {
                                width: parent.width / 2
                                text: page.dayOf(modelData.dayMs)
                                color: Theme.primaryColor
                                font.pixelSize: Theme.fontSizeSmall
                            }
                            Label {
                                width: parent.width / 2
                                horizontalAlignment: Text.AlignRight
                                truncationMode: TruncationMode.Fade
                                text: modelData.hasActivity
                                       ? qsTr("%1 steps  ·  %2 kcal")
                                            .arg(Math.round(modelData.steps))
                                            .arg(Math.round(modelData.energyKcal))
                                       : ""
                                color: Theme.secondaryColor
                                font.pixelSize: Theme.fontSizeSmall
                            }
                        }

                        Label {
                            width: parent.width
                            visible: modelData.hasSleep && modelData.sleepSeconds > 0
                            horizontalAlignment: Text.AlignRight
                            text: {
                                var t = page.duration(modelData.sleepSeconds)
                                if (modelData.sleepQuality >= 0)
                                    t += "  ·  " + Math.round(modelData.sleepQuality * 100) + " %"
                                return qsTr("slept %1").arg(t)
                            }
                            color: Theme.secondaryHighlightColor
                            font.pixelSize: Theme.fontSizeExtraSmall
                        }
                    }
                }
            }

            Item { width: 1; height: Theme.paddingLarge }
        }

        VerticalScrollDecorator {}
    }
}
