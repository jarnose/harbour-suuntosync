import QtQuick 2.0
import Sailfish.Silica 1.0

// One store category's items. Nothing is fetched here: the catalogue came
// in a single reply, and this page reads it back out of AppController.
//
// An item's kind is deliberately not shown. The store's own word for it
// cannot be trusted as a label - a watch face is a "sportsapp" here, with
// the watch-face category as the only thing that tells them apart - and
// what the item really is becomes clear on the SuuntoPlus page once it is
// in the library, where the library's own type decides the heading.
Page {
    id: page

    property string categoryId
    property string categoryTitle
    property var items: []

    function reload() {
        items = AppController.storeItems(page.categoryId)
    }

    Component.onCompleted: reload()

    Connections {
        target: AppController
        onStoreChanged: page.reload()
    }

    SilicaListView {
        anchors.fill: parent
        model: page.items

        header: PageHeader {
            title: page.categoryTitle
            description: qsTr("%n item(s)", "", page.items.length)
        }

        delegate: ListItem {
            id: item
            contentHeight: content.height + Theme.paddingMedium
            enabled: !AppController.storeBusy

            Column {
                id: content
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                y: Theme.paddingSmall

                Label {
                    width: parent.width
                    truncationMode: TruncationMode.Fade
                    text: modelData.title
                    color: item.highlighted ? Theme.highlightColor : Theme.primaryColor
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: item.highlighted ? Theme.secondaryHighlightColor
                                             : Theme.secondaryColor
                    visible: text !== ""
                    text: modelData.subTitle !== "" ? modelData.subTitle
                                                     : modelData.description
                }

                Label {
                    width: parent.width
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: Theme.highlightColor
                    visible: modelData.inLibrary
                    text: qsTr("In your library")
                }
            }

            menu: ContextMenu {
                MenuItem {
                    visible: !modelData.inLibrary
                    text: qsTr("Add to library")
                    onClicked: AppController.addStoreItem(modelData.id)
                }
                MenuItem {
                    visible: modelData.inLibrary
                    text: qsTr("Remove from library")
                    onClicked: AppController.removeStoreItem(modelData.id)
                }
            }
        }

        VerticalScrollDecorator {}
    }
}
