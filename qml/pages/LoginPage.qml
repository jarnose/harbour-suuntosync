import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    property string lastError: ""

    Connections {
        target: AppController
        onErrorOccurred: page.lastError = message
        onCloudAccountChanged: {
            if (AppController.cloudSignedIn)
                pageStack.pop()
        }
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingLarge

            PageHeader {
                title: qsTr("Sign in to Suunto")
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

            TextField {
                id: emailField
                width: parent.width
                label: qsTr("Email")
                placeholderText: label
                inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoAutoUppercase
                EnterKey.iconSource: "image://theme/icon-m-enter-next"
                EnterKey.onClicked: passwordField.focus = true
            }

            PasswordField {
                id: passwordField
                width: parent.width
                label: qsTr("Password")
                placeholderText: label
                EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                EnterKey.enabled: emailField.text.length > 0 && passwordField.text.length > 0
                EnterKey.onClicked: signInButton.clicked()
            }

            Button {
                id: signInButton
                anchors.horizontalCenter: parent.horizontalCenter
                enabled: emailField.text.length > 0 && passwordField.text.length > 0
                         && !AppController.cloudLoginInProgress
                text: AppController.cloudLoginInProgress ? qsTr("Signing in…") : qsTr("Sign in")
                onClicked: {
                    page.lastError = ""
                    AppController.loginToCloud(emailField.text, passwordField.text)
                }
            }
        }
    }
}
