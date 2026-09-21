import QtQuick 2.0
import Sailfish.Silica 1.0

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

        PullDownMenu {
            MenuItem {
                text: AppController.watchPaired ? qsTr("Change watch") : qsTr("Pair a watch")
                onClicked: pageStack.push(Qt.resolvedUrl("PairingPage.qml"))
            }
            MenuItem {
                text: AppController.cloudSignedIn ? qsTr("Sign out of Suunto") : qsTr("Sign in to Suunto")
                onClicked: {
                    if (AppController.cloudSignedIn)
                        AppController.logoutFromCloud()
                    else
                        pageStack.push(Qt.resolvedUrl("LoginPage.qml"))
                }
            }
        }

        Column {
            id: column
            width: parent.width

            PageHeader {
                title: AppController.appName
            }

            Label {
                visible: page.lastError.length > 0
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.WordWrap
                color: Theme.errorColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: page.lastError
            }

            Label {
                visible: AppController.watchPaired
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                truncationMode: TruncationMode.Fade
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: AppController.watchConnected
                      ? qsTr("%1 · Connected").arg(AppController.pairedWatchName)
                      : qsTr("%1 · Not connected").arg(AppController.pairedWatchName)
            }

            Label {
                visible: AppController.cloudSignedIn
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                truncationMode: TruncationMode.Fade
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("Signed in as %1").arg(AppController.cloudEmail)
            }

            ViewPlaceholder {
                enabled: true
                text: qsTr("No workouts yet")
                hintText: AppController.watchPaired || AppController.cloudSignedIn
                          ? qsTr("Workout sync isn't implemented yet")
                          : qsTr("Pair a Suunto watch or sign in to your Suunto account to get started")
            }
        }
    }
}
