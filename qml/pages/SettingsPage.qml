import QtQuick 2.0
import Sailfish.Silica 1.0

// Everything that was previously scattered across three pull-down menus,
// in one place - plus the preferences that had nowhere to live.
Page {
    id: page

    property string lastError: ""

    Connections {
        target: AppController
        onErrorOccurred: page.lastError = message
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column
            width: parent.width

            PageHeader { title: qsTr("Settings") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: page.lastError.length > 0
                text: page.lastError
                wrapMode: Text.Wrap
                color: Theme.highlightColor
                font.pixelSize: Theme.fontSizeExtraSmall
            }

            // ---- account ----
            SectionHeader { text: qsTr("Suunto account") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                text: AppController.cloudSignedIn
                      ? AppController.cloudEmail
                      : qsTr("Not signed in. The app works without an account - "
                             + "signing in adds cloud workouts and lets you upload.")
                wrapMode: Text.Wrap
                color: AppController.cloudSignedIn ? Theme.primaryColor
                                                    : Theme.secondaryHighlightColor
                font.pixelSize: Theme.fontSizeSmall
            }

            Button {
                x: Theme.horizontalPageMargin
                text: AppController.cloudSignedIn ? qsTr("Sign out") : qsTr("Sign in")
                onClicked: {
                    if (AppController.cloudSignedIn)
                        AppController.logoutFromCloud()
                    else
                        pageStack.push(Qt.resolvedUrl("LoginPage.qml"))
                }
            }

            // ---- watch ----
            SectionHeader { text: qsTr("Watch") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                text: {
                    if (!AppController.watchPaired)
                        return qsTr("No watch paired")
                    var state = AppController.whiteboardReady
                            ? qsTr("connected")
                            : (AppController.watchConnected ? qsTr("connecting…")
                                                             : qsTr("not connected"))
                    return AppController.pairedWatchName + " · " + state
                }
                wrapMode: Text.Wrap
                color: Theme.primaryColor
                font.pixelSize: Theme.fontSizeSmall
            }

            Button {
                x: Theme.horizontalPageMargin
                text: AppController.watchPaired ? qsTr("Change watch") : qsTr("Pair a watch")
                onClicked: pageStack.push(Qt.resolvedUrl("PairingPage.qml"))
            }

            Button {
                // The one watch-side feature that needs no account: the
                // assist-data endpoints take no credential at all.
                x: Theme.horizontalPageMargin
                visible: AppController.whiteboardReady
                enabled: !AppController.gpsUpdateInProgress
                text: AppController.gpsUpdateInProgress
                      ? qsTr("Updating GPS data…")
                      : qsTr("Update GPS data")
                onClicked: AppController.updateWatchGps()
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: AppController.whiteboardReady
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: qsTr("Downloads the satellite predictions the watch uses to get a fix "
                           + "quickly, and writes them to it. Needs no Suunto account.")
            }

            TextSwitch {
                text: qsTr("Sync when the watch connects")
                description: qsTr("Off by default: a sync is minutes of radio time, "
                                  + "and the watch coming into range isn't always a "
                                  + "reason to start one.")
                checked: AppController.syncOnConnect
                onClicked: AppController.syncOnConnect = checked
            }

            // ---- cover ----
            SectionHeader { text: qsTr("Cover") }

            ComboBox {
                width: parent.width
                label: qsTr("Show")
                currentIndex: {
                    var m = AppController.coverMode
                    if (m === "totals") return 1
                    if (m === "sleep") return 2
                    if (m === "nothing") return 3
                    return 0
                }
                menu: ContextMenu {
                    MenuItem { text: qsTr("Latest workout") }
                    MenuItem { text: qsTr("Lifetime totals") }
                    MenuItem { text: qsTr("Last night's sleep") }
                    MenuItem { text: qsTr("Nothing") }
                }
                onCurrentIndexChanged: {
                    var modes = ["latest", "totals", "sleep", "nothing"]
                    AppController.coverMode = modes[currentIndex]
                }
            }

            // ---- about ----
            SectionHeader { text: qsTr("About") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                text: qsTr("Syncs workouts and health data with a Suunto watch over "
                           + "Bluetooth, and with the Suunto cloud. Not affiliated with "
                           + "or endorsed by Suunto.")
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
            }

            Item { width: 1; height: Theme.paddingLarge }
        }

        VerticalScrollDecorator {}
    }
}
