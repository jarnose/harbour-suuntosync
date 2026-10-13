import QtQuick 2.0
import Sailfish.Silica 1.0

// The account's SuuntoPlus library, and which of it the watch has.
//
// "Installed" is read from the watch's own plugin directory, not from its
// /Plugin/List resource. That distinction cost a long evening and is worth
// restating here: /Plugin/List reports less than what is installed - fifteen
// names where the directory held seventeen plugin files, and the official
// Suunto app's own pushes are missing from it too. The directory is the
// truth. See docs/suuntoplus-and-watchfaces.md.
//
// A sports app is kept on the watch as <pluginId>.fea and a watch face as
// <id>.wfa; the zip that was uploaded does not survive the install.
Page {
    id: page

    property var items: []
    property string lastError: ""

    function reload() {
        items = AppController.suuntoPlusItems()
    }

    Component.onCompleted: {
        reload()
        // Only when there is nothing to show. The refresh needs the watch
        // connected and an account, and it reads three things over BLE, so
        // doing it on every open would be rude to a page the user may be
        // opening to look at what is already known.
        if (items.length === 0)
            AppController.refreshSuuntoPlus()
    }

    Connections {
        target: AppController
        onSuuntoPlusChanged: page.reload()
        onErrorOccurred: page.lastError = message
    }

    SilicaListView {
        id: listView
        anchors.fill: parent
        model: page.items

        PullDownMenu {
            busy: AppController.suuntoPlusBusy
            MenuItem {
                text: qsTr("Refresh")
                onClicked: AppController.refreshSuuntoPlus()
            }
        }

        header: Column {
            width: parent.width

            PageHeader { title: qsTr("SuuntoPlus") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryHighlightColor
                visible: page.lastError !== ""
                text: page.lastError
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryColor
                visible: page.items.length === 0 && !AppController.suuntoPlusBusy
                text: AppController.cloudSignedIn
                       ? qsTr("Connect the watch and pull down to refresh.")
                       : qsTr("Sign in to the Suunto cloud to see your library.")
            }
        }

        delegate: ListItem {
            id: item
            contentHeight: content.height + Theme.paddingMedium
            // Nothing to do while a BLE write is in flight, and the watch
            // takes one request at a time anyway.
            enabled: !AppController.suuntoPlusBusy

            Column {
                id: content
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                y: Theme.paddingSmall

                Label {
                    width: parent.width
                    truncationMode: TruncationMode.Fade
                    text: modelData.name
                    color: item.highlighted ? Theme.highlightColor : Theme.primaryColor
                }

                Label {
                    width: parent.width
                    truncationMode: TruncationMode.Fade
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: modelData.installed ? Theme.highlightColor
                                                : Theme.secondaryColor
                    text: {
                        var kind = modelData.isWatchFace ? qsTr("Watch face")
                                                          : qsTr("Sports app")
                        var where = modelData.installed ? qsTr("on the watch")
                                                         : qsTr("not on the watch")
                        return kind + " · " + where
                    }
                }
            }

            menu: ContextMenu {
                MenuItem {
                    visible: !modelData.installed
                    text: qsTr("Install on the watch")
                    onClicked: AppController.installSuuntoPlusItem(modelData.id)
                }
                MenuItem {
                    visible: modelData.installed
                    text: qsTr("Remove from the watch")
                    onClicked: AppController.removeSuuntoPlusItem(modelData.id)
                }
            }
        }

        ViewPlaceholder {
            enabled: AppController.suuntoPlusBusy && page.items.length === 0
            text: qsTr("Reading the watch")
        }

        VerticalScrollDecorator {}
    }
}
