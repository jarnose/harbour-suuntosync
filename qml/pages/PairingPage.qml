import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    // Debugging aid while the BlueZ D-Bus path is unproven on this SDK
    // target/Sailjail sandbox (see Phase 5 in the plan) - surfaces
    // AppController::errorOccurred() persistently (not a fading toast) so a
    // failed GetManagedObjects/StartDiscovery call is visible instead of
    // looking identical to "scanned fine, found nothing".
    property string lastError: ""

    Connections {
        target: AppController
        onErrorOccurred: page.lastError = message
        // Phase 6 validation probe result (testWhiteboard()) - reuses the
        // same banner, not a separate one, since only one is ever relevant
        // to look at at a time here.
        onWhiteboardTestResult: page.lastError = summary
        // Same reuse as whiteboardTestResult - see testLogbookFetch()'s doc
        // comment in appcontroller.h for what this probe actually does.
        onLogbookTestResult: page.lastError = summary
    }

    Component.onCompleted: AppController.refreshDevices()

    onStatusChanged: {
        // Scanning burns battery and radio time on both ends - only run it
        // while this page is actually the one the user is looking at.
        if (status === PageStatus.Active)
            AppController.startScan()
        else if (status === PageStatus.Inactive)
            AppController.stopScan()
    }

    SilicaListView {
        id: listView
        anchors.fill: parent
        model: AppController.deviceModel

        header: Column {
            width: parent.width

            PageHeader {
                title: qsTr("Pair a watch")
            }

            Label {
                visible: page.lastError.length > 0
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.WordWrap
                // Shared between real errors and testWhiteboard()'s success
                // summary (starts with "OK ") - only tint it red for an
                // actual failure.
                color: page.lastError.indexOf("OK ") === 0 ? Theme.secondaryColor : Theme.errorColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: page.lastError
            }

            // Next validation probe after Test Whiteboard - see
            // AppController::testLogbookFetch()'s doc comment for exactly
            // what this exercises and what's still experimental about it.
            // Needs a real logbook id (the numeric suffix of
            // /Logbook/byId/<id>/Data - itself that workout's Unix start
            // timestamp in seconds) - typed in by hand for now since
            // there's no on-device /Entries listing UI yet, only
            // testWhiteboard()'s raw-bytes probe of that same resource.
            Row {
                visible: AppController.watchConnected
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                spacing: Theme.paddingSmall

                TextField {
                    id: logbookIdField
                    width: parent.width - testLogbookButton.width - parent.spacing
                    label: qsTr("Logbook id")
                    placeholderText: qsTr("e.g. 1785740504")
                    inputMethodHints: Qt.ImhDigitsOnly
                    EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                    EnterKey.enabled: text.length > 0
                    EnterKey.onClicked: testLogbookButton.clicked()
                }

                Button {
                    id: testLogbookButton
                    anchors.verticalCenter: logbookIdField.verticalCenter
                    enabled: logbookIdField.text.length > 0
                    text: qsTr("Fetch and save")
                    onClicked: AppController.testLogbookFetch(logbookIdField.text)
                }
            }

            Row {
                visible: AppController.watchConnected
                // Ask the watch whether it has a resource at all. A model
                // that does not answers with the six-byte f5 error, so a
                // question that used to need adb and a capture takes two
                // seconds here.
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                spacing: Theme.paddingSmall

                TextField {
                    id: probePathField
                    width: parent.width - probeButton.width - parent.spacing
                    label: qsTr("Resource path")
                    placeholderText: qsTr("e.g. /Device/GNSS/ExtendedEphemerisData/Format")
                    EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                    EnterKey.enabled: text.length > 0
                    EnterKey.onClicked: probeButton.clicked()
                }

                Button {
                    id: probeButton
                    anchors.verticalCenter: probePathField.verticalCenter
                    enabled: probePathField.text.length > 0
                    text: qsTr("Probe")
                    onClicked: AppController.probePath(probePathField.text)
                }
            }

            Button {
                // The watch's own field table. Needed per watch model: a
                // Suunto 9 Baro transfers workouts perfectly and decodes
                // them to all zeros, because the compiled-in table is a
                // Race's. Takes any logbook id from this watch.
                x: Theme.horizontalPageMargin
                enabled: logbookIdField.text.length > 0
                text: qsTr("Fetch descriptors")
                onClicked: AppController.testDescriptorsFetch(logbookIdField.text)
            }
        }

        PullDownMenu {
            MenuItem {
                text: qsTr("Refresh")
                onClicked: AppController.refreshDevices()
            }
            MenuItem {
                // Phase 6 validation probe - see AppController::testWhiteboard().
                // Not gated on whiteboardReady too: GATT service discovery can
                // still be resolving right after Connect() succeeds, and
                // testWhiteboard() itself reports that case cleanly instead of
                // this item just doing nothing.
                visible: AppController.watchConnected
                text: qsTr("Test Whiteboard (GET /Logbook/Entries)")
                onClicked: AppController.testWhiteboard()
            }
            MenuItem {
                // See AppController::testEntriesFetch()'s doc comment -
                // the cheap experiment towards listing workouts without a
                // hand-typed id: tries the same bulk-fetch shortcut that
                // already works for /Data, directly against /Entries.
                visible: AppController.watchConnected
                text: qsTr("Test /Entries shortcut")
                onClicked: AppController.testEntriesFetch()
            }
            MenuItem {
                // Item 3: reading sleep from the watch directly rather
                // than via the cloud. Renders the watch's timeline file and
                // pages it off - see AppController::testHealthResourceFetch().
                visible: AppController.watchConnected
                text: qsTr("Fetch sleep timeline")
                onClicked: AppController.testHealthResourceFetch("Sleep")
            }
            MenuItem {
                // Daily activity. This used to ask for a rendered file
                // called mdsAct.sbm, guessed by analogy with sleep - the
                // capture showed no such file exists. libmds.so gave the
                // real answer: /Activity/TrendData, one integer cursor,
                // records straight in the reply. See
                // AppController::testActivityTrendFetch().
                visible: AppController.watchConnected
                text: qsTr("Fetch activity trend")
                onClicked: AppController.testActivityTrendFetch()
            }
        }

        delegate: ListItem {
            id: delegateItem
            contentHeight: Theme.itemSizeMedium
            enabled: model.paired

            Column {
                anchors {
                    left: parent.left
                    right: parent.right
                    leftMargin: Theme.horizontalPageMargin
                    rightMargin: Theme.horizontalPageMargin
                    verticalCenter: parent.verticalCenter
                }
                spacing: Theme.paddingSmall

                Label {
                    text: model.name
                    width: parent.width
                    truncationMode: TruncationMode.Fade
                    color: delegateItem.enabled ? Theme.primaryColor : Theme.secondaryColor
                }
                Label {
                    text: model.paired
                          ? (model.connected ? qsTr("Paired · Connected") : qsTr("Paired"))
                          : qsTr("Not paired - pair it in Settings > Bluetooth first")
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                    width: parent.width
                    truncationMode: TruncationMode.Fade
                }
            }

            onClicked: AppController.selectWatch(model.objectPath, model.address, model.name)
        }

        ViewPlaceholder {
            enabled: listView.count === 0
            text: qsTr("No Suunto watch found yet")
            hintText: qsTr("Make sure the watch is on and nearby, and paired in Settings > Bluetooth")
        }
    }
}
