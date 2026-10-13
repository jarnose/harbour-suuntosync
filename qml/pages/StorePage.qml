import QtQuick 2.0
import Sailfish.Silica 1.0

// The SuuntoPlus store, by category.
//
// The whole catalogue arrives in one request - fourteen categories and the
// full record of every item they name, 230 of them on this account - so a
// category page fetches nothing. Tapping one is free.
//
// Adding an item here puts it in the account's library and nothing more.
// Getting it onto the watch is the SuuntoPlus page's job, which is the same
// division the official app makes.
Page {
    id: page

    property var categories: []
    property string lastError: ""

    function reload() {
        categories = AppController.storeCategories()
    }

    Component.onCompleted: {
        reload()
        if (categories.length === 0)
            AppController.refreshStore()
    }

    Connections {
        target: AppController
        onStoreChanged: page.reload()
        onErrorOccurred: page.lastError = message
    }

    SilicaListView {
        anchors.fill: parent
        model: page.categories

        PullDownMenu {
            busy: AppController.storeBusy
            MenuItem {
                text: qsTr("Refresh")
                onClicked: AppController.refreshStore()
            }
        }

        header: Column {
            width: parent.width

            PageHeader { title: qsTr("Store") }

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
                visible: page.categories.length === 0 && !AppController.storeBusy
                text: AppController.cloudSignedIn
                       ? qsTr("Connect the watch and pull down to refresh. The store "
                               + "answers a different catalogue for each watch model.")
                       : qsTr("Sign in to the Suunto cloud to browse the store.")
            }
        }

        delegate: ListItem {
            contentHeight: Theme.itemSizeSmall

            Label {
                x: Theme.horizontalPageMargin
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 2 * Theme.horizontalPageMargin - count.width
                truncationMode: TruncationMode.Fade
                text: modelData.title
            }

            Label {
                id: count
                anchors {
                    right: parent.right
                    rightMargin: Theme.horizontalPageMargin
                    verticalCenter: parent.verticalCenter
                }
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryColor
                text: modelData.count
            }

            onClicked: pageStack.push(Qt.resolvedUrl("StoreCategoryPage.qml"),
                                       { categoryId: modelData.id,
                                         categoryTitle: modelData.title })
        }

        ViewPlaceholder {
            enabled: AppController.storeBusy && page.categories.length === 0
            text: qsTr("Reading the store")
        }

        VerticalScrollDecorator {}
    }
}
