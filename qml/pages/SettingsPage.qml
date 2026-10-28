import QtQuick 2.0
import Sailfish.Silica 1.0

// Everything that was previously scattered across three pull-down menus,
// in one place - plus the preferences that had nowhere to live.
Page {
    id: page

    property string lastError: ""

    // What the watch answered to the last WiFi attempt, kept so it stays on
    // screen while reading it.
    property string wifiReport: ""

    // Not a binding: knownWatches() hits SQLite, so it is read when the
    // page opens and when the active watch changes, not on every repaint.
    property var knownWatches: []

    function refreshKnownWatches() {
        knownWatches = AppController.knownWatches()
    }

    // The installer is another process and tells this one nothing, so the
    // "is it there" signal is re-read whenever the page becomes visible -
    // which is when the user is back from it.
    onStatusChanged: {
        if (status === PageStatus.Active)
            AppController.refreshNotifyServiceState()
    }

    Component.onCompleted: {
        refreshKnownWatches()
        // Asking attaches to the watch for the one read and lets go again,
        // so this is gated on the watch being connected rather than on a
        // session already being up - there is none, most of the time.
        if (AppController.watchConnected)
            AppController.readWatchNotificationsEnabled()
    }

    // -1 until the watch has been asked, which only works while connected.
    property int watchNotifications: -1
    property string watchNotificationsDetail: ""


    Connections {
        target: AppController
        onErrorOccurred: page.lastError = message
        onWifiResult: page.wifiReport = message
        onPairedWatchChanged: page.refreshKnownWatches()
        onWatchNotificationsRead: {
            page.watchNotifications = value
            page.watchNotificationsDetail = detail
        }
        onWatchConnectedChanged: {
            if (AppController.watchConnected)
                AppController.readWatchNotificationsEnabled()
            else
                page.watchNotifications = -1
        }
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column
            width: parent.width
            // Buttons have no margins of their own, so a run of them -
            // the watch section has four, the WiFi section six - came out
            // as one solid block. Reported from the forum thread.
            spacing: Theme.paddingMedium

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

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: AppController.cloudSignedIn
                // "or changed" is load-bearing and measured: the cloud's
                // `since` matches a workout's last-modified time, so an
                // ordinary sync does bring back an edit made elsewhere.
                // This text used to offer that as the reason to re-fetch
                // everything, which was wrong.
                text: qsTr("A normal sync asks the cloud only for what is new "
                           + "or changed, including workouts edited elsewhere. "
                           + "Fetch everything again only if this phone's copy "
                           + "looks wrong.")
                wrapMode: Text.Wrap
                color: Theme.secondaryHighlightColor
                font.pixelSize: Theme.fontSizeExtraSmall
            }

            Button {
                x: Theme.horizontalPageMargin
                visible: AppController.cloudSignedIn
                text: AppController.workoutSyncInProgress
                      ? qsTr("Fetching…") : qsTr("Fetch all workouts again")
                enabled: !AppController.workoutSyncInProgress
                onClicked: AppController.resyncCloudWorkouts()
            }

            // ---- watch ----
            SectionHeader { text: qsTr("Watch") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                text: {
                    if (!AppController.watchPaired)
                        return qsTr("No watch paired")
                    var state = AppController.watchConnected
                            ? qsTr("connected")
                            : qsTr("not connected")
                    return AppController.pairedWatchName + " · " + state
                }
                wrapMode: Text.Wrap
                color: Theme.primaryColor
                font.pixelSize: Theme.fontSizeSmall
            }

            // Every watch that has been paired here, as a menu. Switching
            // used to mean forgetting one watch and finding the other in a
            // scan, which is three screens for something done daily when
            // two watches are in use.
            ListItem {
                id: watchSwitcher
                visible: page.knownWatches.length > 1
                contentHeight: Theme.itemSizeSmall
                onClicked: openMenu()

                Label {
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Switch watch")
                    color: watchSwitcher.highlighted ? Theme.highlightColor : Theme.primaryColor
                }

                menu: ContextMenu {
                    Repeater {
                        model: page.knownWatches
                        MenuItem {
                            // The active one is listed too, marked, so the
                            // menu says which watch is in use rather than
                            // only offering the ones that are not.
                            text: modelData.active
                                  ? qsTr("%1 (in use)").arg(modelData.name) : modelData.name
                            onClicked: AppController.switchToWatch(modelData.address)
                        }
                    }
                }
            }

            Button {
                x: Theme.horizontalPageMargin
                text: AppController.watchPaired ? qsTr("Change watch") : qsTr("Pair a watch")
                onClicked: pageStack.push(Qt.resolvedUrl("PairingPage.qml"))
            }

            Button {
                // The switcher's list only ever grew without this. Behind a
                // remorse timer because it is the one watch-side action that
                // throws something away - though not the field table, which
                // is kept per address and costs a minute to fetch again.
                x: Theme.horizontalPageMargin
                visible: AppController.watchPaired
                text: qsTr("Forget this watch")
                onClicked: forgetRemorse.execute(qsTr("Forgetting %1")
                                                  .arg(AppController.pairedWatchName),
                                                 function() { AppController.forgetWatch() })
            }

            RemorsePopup { id: forgetRemorse }

            TextSwitch {
                // The watch's own switch, not a preference of this app's: a
                // watch with this off refuses a notification with 400, which
                // is exactly what a 9 Baro did while its own menu said
                // notifications were on.
                //
                // Shown whenever a watch is paired, and disabled with the
                // reason when the watch has not answered - the first version
                // hid it until the value was known, which made "could not
                // ask" look exactly like "this was never built".
                visible: AppController.watchPaired
                enabled: page.watchNotifications >= 0
                text: qsTr("Accept notifications on the watch")
                description: page.watchNotifications >= 0
                        ? qsTr("The setting on the watch itself. Needed before "
                                + "the notification daemon can send anything.")
                        : qsTr("The watch has not said: %1")
                                .arg(page.watchNotificationsDetail.length > 0
                                      ? page.watchNotificationsDetail
                                      : qsTr("not asked yet"))
                automaticCheck: false
                checked: page.watchNotifications === 1
                onClicked: AppController.setWatchNotificationsEnabled(
                               page.watchNotifications !== 1)
            }

            Button {
                // The one watch-side feature that needs no account: the
                // assist-data endpoints take no credential at all.
                x: Theme.horizontalPageMargin
                visible: AppController.watchConnected
                enabled: !AppController.gpsUpdateInProgress
                text: AppController.gpsUpdateInProgress
                      ? qsTr("Updating GPS data…")
                      : qsTr("Update GPS data")
                onClicked: AppController.updateWatchGps()
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: AppController.watchConnected
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: qsTr("Downloads the satellite predictions the watch uses to get a fix "
                           + "quickly, and writes them to it. Needs no Suunto account.")
            }

            TextSwitch {
                text: qsTr("Sync workouts when the watch connects")
                description: qsTr("Off by default: a sync is minutes of radio time, "
                                  + "and the watch coming into range isn't always a "
                                  + "reason to start one. Health data is not included "
                                  + "- the health page has its own action for that.")
                checked: AppController.syncOnConnect
                onClicked: AppController.syncOnConnect = checked
            }

            // ---- the watch's wifi ----
            SectionHeader { text: qsTr("Watch WiFi") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryColor
                // Said plainly because it is the point of the feature: the
                // watch does the downloading, not the phone.
                text: qsTr("The watch downloads offline maps and GPS predictions over "
                           + "WiFi by itself. The network is stored here and handed to "
                           + "the watch; the passphrase goes into the same vault as the "
                           + "account's and is never shown again.")
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeSmall
                visible: AppController.wifiSsid.length > 0
                color: Theme.highlightColor
                text: AppController.wifiKeyStored
                       ? qsTr("Stored: %1, with a passphrase").arg(AppController.wifiSsid)
                       : qsTr("Stored: %1, open network").arg(AppController.wifiSsid)
            }

            TextField {
                id: wifiSsidField
                width: parent.width
                label: qsTr("Network name (SSID)")
                text: AppController.wifiSsid
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                EnterKey.iconSource: "image://theme/icon-m-enter-next"
                EnterKey.onClicked: wifiKeyField.focus = true
            }

            TextField {
                id: wifiKeyField
                width: parent.width
                label: qsTr("Passphrase")
                placeholderText: AppController.wifiKeyStored
                                  ? qsTr("Stored - type to replace")
                                  : qsTr("Leave empty for an open network")
                echoMode: TextInput.PasswordEchoOnEdit
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                EnterKey.onClicked: focus = false
            }

            Button {
                x: Theme.horizontalPageMargin
                text: qsTr("Save the network")
                enabled: wifiSsidField.text.length > 0
                onClicked: {
                    AppController.setWifiCredentials(wifiSsidField.text, wifiKeyField.text)
                    // Not kept on the page: it is in the vault now, and a
                    // field that still holds it is a field that can show it.
                    wifiKeyField.text = ""
                }
            }

            Button {
                x: Theme.horizontalPageMargin
                text: AppController.wifiBusy ? qsTr("Talking to the watch…")
                                              : qsTr("Send to the watch")
                enabled: !AppController.wifiBusy && AppController.wifiSsid.length > 0
                         && AppController.watchConnected
                onClicked: AppController.sendWifiToWatch()
            }

            Button {
                x: Theme.horizontalPageMargin
                // Needed before the watch can fetch anything itself: the
                // API address and a credential of its own.
                text: qsTr("Give the watch its cloud credentials")
                enabled: !AppController.wifiBusy && AppController.watchConnected
                         && AppController.cloudSignedIn
                onClicked: AppController.sendCloudCredentialsToWatch()
            }

            Button {
                x: Theme.horizontalPageMargin
                // Same mechanism as the weather, one page further: the
                // regions are chosen there and the watch fetches them.
                text: qsTr("Offline maps for the watch")
                enabled: AppController.cloudSignedIn
                onClicked: pageStack.push(Qt.resolvedUrl("OfflineMapsPage.qml"))
            }

            Button {
                x: Theme.horizontalPageMargin
                // Not a command, which the captures settled: it writes the
                // credential the watch needs and subscribes to the
                // resource the official app subscribes to. The watch does
                // the fetching, over its own WiFi.
                text: qsTr("Let the watch fetch its weather")
                enabled: !AppController.wifiBusy && AppController.watchConnected
                         && AppController.cloudSignedIn
                onClicked: AppController.syncWatchWeather()
            }

            Button {
                x: Theme.horizontalPageMargin
                // Reads what the watch holds: when it last synced the
                // weather - the date on its own screen - where it thinks
                // it is, and the current reading.
                text: qsTr("Read the watch's weather state")
                enabled: !AppController.wifiBusy && AppController.watchConnected
                onClicked: AppController.readWatchWeatherState()
            }

            Button {
                x: Theme.horizontalPageMargin
                // The watch is the only one that knows whether its own
                // download worked; this is where it says so.
                text: qsTr("Read the watch's event log")
                enabled: !AppController.wifiBusy && AppController.watchConnected
                onClicked: AppController.readWatchEventLog()
            }

            Button {
                x: Theme.horizontalPageMargin
                text: qsTr("Read the watch's WiFi state")
                enabled: !AppController.wifiBusy && AppController.watchConnected
                onClicked: AppController.readWatchWifiState()
            }

            Button {
                x: Theme.horizontalPageMargin
                visible: AppController.wifiSsid.length > 0
                text: qsTr("Forget the network")
                onClicked: AppController.forgetWifiCredentials()
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                visible: page.wifiReport.length > 0
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryHighlightColor
                // The watch's own answers, byte for byte where they are not
                // decoded yet. This is the measurement - see
                // docs/wifi-and-offline-maps.md - so it is shown rather
                // than swallowed.
                text: page.wifiReport
            }

            // ---- sending workouts onwards ----
            SectionHeader { text: qsTr("Sending workouts onwards") }

            TextSwitch {
                text: qsTr("Send to Suunto after a sync")
                description: qsTr("Uploads every synced workout the Suunto cloud "
                                  + "does not have yet, without asking.")
                enabled: AppController.cloudSignedIn
                checked: AppController.autoUploadSuunto
                onClicked: AppController.autoUploadSuunto = !AppController.autoUploadSuunto
            }

            // ---- cover ----
            // ---- map ----
            SectionHeader { text: qsTr("Map") }

            TextSwitch {
                text: qsTr("Show a base map under the track")
                description: qsTr("Off by default: the tiles come from somebody else's "
                                  + "servers, and a map is a want rather than a need.")
                checked: AppController.baseMapEnabled
                onClicked: AppController.baseMapEnabled = !AppController.baseMapEnabled
            }

            // Only shown when the map is on: a URL field above a switch that
            // ignores it would invite filling in something that does nothing.
            Column {
                width: parent.width
                visible: AppController.baseMapEnabled

                TextField {
                    width: parent.width
                    label: qsTr("Tile address")
                    text: AppController.mapTileUrl
                    inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                    EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                    EnterKey.onClicked: {
                        AppController.mapTileUrl = text
                        focus = false
                    }
                    onActiveFocusChanged: {
                        if (!activeFocus)
                            AppController.mapTileUrl = text
                    }
                }

                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * x
                    wrapMode: Text.Wrap
                    text: qsTr("Either a TileJSON address or a {z}/{x}/{y} template. The "
                               + "default is OpenFreeMap, which needs no account and allows "
                               + "this use; its attribution is drawn on the map. Vector "
                               + "tiles only for now - an address serving images will say so.")
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                }

                Button {
                    x: Theme.horizontalPageMargin
                    visible: AppController.mapTileUrl !== AppController.defaultMapTileUrl
                    text: qsTr("Back to the default source")
                    onClicked: AppController.mapTileUrl = AppController.defaultMapTileUrl
                }
            }

            SectionHeader { text: qsTr("Cover") }

            ComboBox {
                width: parent.width
                label: qsTr("Cover button")
                description: qsTr("What the cover's one button does.")
                currentIndex: {
                    var a = AppController.coverAction
                    if (a === "watchHealth") return 1
                    if (a === "uploadWorkouts") return 2
                    if (a === "uploadHealth") return 3
                    if (a === "all") return 4
                    return 0
                }
                menu: ContextMenu {
                    MenuItem {
                        text: qsTr("Sync workouts from the watch")
                        onClicked: AppController.coverAction = "watchWorkouts"
                    }
                    MenuItem {
                        text: qsTr("Sync health data from the watch")
                        onClicked: AppController.coverAction = "watchHealth"
                    }
                    MenuItem {
                        text: qsTr("Upload workouts to the cloud")
                        onClicked: AppController.coverAction = "uploadWorkouts"
                    }
                    MenuItem {
                        text: qsTr("Upload health data to the cloud")
                        onClicked: AppController.coverAction = "uploadHealth"
                    }
                    MenuItem {
                        text: qsTr("Everything, both directions")
                        onClicked: AppController.coverAction = "all"
                    }
                }
            }

            TextSwitch {
                text: qsTr("Show today's steps too")
                description: qsTr("Under whatever the cover shows - the day's steps "
                                  + "and last night's sleep are not alternatives.")
                checked: AppController.coverSteps
                onClicked: AppController.coverSteps = !AppController.coverSteps
            }

            ComboBox {
                width: parent.width
                label: qsTr("Show")
                currentIndex: {
                    var m = AppController.coverMode
                    if (m === "totals") return 1
                    if (m === "progress") return 2
                    if (m === "sleep") return 3
                    if (m === "nothing") return 4
                    return 0
                }
                menu: ContextMenu {
                    MenuItem { text: qsTr("Latest workout") }
                    MenuItem { text: qsTr("Lifetime totals") }
                    MenuItem { text: qsTr("Fitness, fatigue and form") }
                    MenuItem { text: qsTr("Last night's sleep") }
                    MenuItem { text: qsTr("Nothing") }
                }
                onCurrentIndexChanged: {
                    // Index order is presentation only - the mode is stored
                    // as its own name, so inserting one in the middle does
                    // not reinterpret what somebody already chose.
                    var modes = ["latest", "totals", "progress", "sleep", "nothing"]
                    AppController.coverMode = modes[currentIndex]
                }
            }

            // ---- notifications ----
            //
            // The whole section is absent in a build that carries no
            // service to install, rather than present and inert. See
            // AppController::installNotifyService().
            SectionHeader {
                text: qsTr("Notifications")
                visible: AppController.notifyServiceInstallable
            }

            Label {
                visible: AppController.notifyServiceInstallable
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: AppController.notifyServiceSeen
                      ? qsTr("The notification service is installed. The phone's own "
                             + "notifications are forwarded to the watch.")
                      : qsTr("Forwarding the phone's notifications to the watch needs a "
                             + "small second package. It cannot be part of this app: "
                             + "seeing another app's notification needs a connection the "
                             + "sandbox will not relay. This app carries it and the "
                             + "system installs it - you will be asked to confirm.")
            }

            Button {
                visible: AppController.notifyServiceInstallable
                x: Theme.horizontalPageMargin
                text: AppController.notifyServiceSeen
                      ? qsTr("Reinstall the service")
                      : qsTr("Install the service")
                onClicked: AppController.installNotifyService()
            }

            Label {
                // Said before the attempt, not only after a refusal: an
                // unsigned package is rejected without explaining itself.
                visible: AppController.notifyServiceInstallable
                         && !AppController.notifyServiceSeen
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: qsTr("If the install is refused, allow untrusted software in "
                           + "Settings | Untrusted software, then try again.")
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
