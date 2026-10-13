import QtQuick 2.0
import Sailfish.Silica 1.0

// One store item in full: its preview, its long description, and the one
// action that applies.
//
// Nothing is fetched here either. The store's single reply carries the
// banner urls, `richText`, the rating, the owner and the version, so the
// per-item endpoint (store/sportsapps/<id>) is not used at all - see
// docs/suuntoplus-and-watchfaces.md.
//
// The item is re-read from AppController on every change rather than kept
// as the snapshot this page was pushed with, so the button follows an add
// or a removal made from here or anywhere else.
Page {
    id: page

    property string itemId
    property var item: ({})

    function reload() {
        item = AppController.storeItem(page.itemId)
    }

    Component.onCompleted: reload()

    Connections {
        target: AppController
        onStoreChanged: page.reload()
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingMedium

            PageHeader {
                title: page.item.title !== undefined ? page.item.title : ""
                description: page.item.subTitle !== undefined ? page.item.subTitle : ""
            }

            // The banner for an app, the rendered face for a watch face.
            // Height follows the image's own shape - the two are nothing
            // like the same aspect - and the whole block disappears when
            // there is no image rather than leaving a gap.
            Image {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                visible: status === Image.Ready
                source: {
                    if (page.item.bannerUrl !== undefined && page.item.bannerUrl !== "")
                        return page.item.bannerUrl
                    return page.item.iconUrl !== undefined ? page.item.iconUrl : ""
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                visible: text !== ""
                text: {
                    var parts = []
                    if (page.item.owner !== undefined && page.item.owner !== "")
                        parts.push(page.item.owner)
                    if (page.item.version !== undefined && page.item.version !== "")
                        parts.push(qsTr("version %1").arg(page.item.version))
                    if (page.item.reviews !== undefined && page.item.reviews > 0) {
                        parts.push(qsTr("%1 from %n review(s)", "", page.item.reviews)
                                    .arg(page.item.avgRating.toFixed(1)))
                    }
                    return parts.join(" · ")
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.highlightColor
                font.pixelSize: Theme.fontSizeSmall
                visible: page.item.inLibrary === true
                text: qsTr("In your library")
            }

            // richText is plain text with newlines, not markup - measured on
            // both an app and a watch face - so it is shown as it stands.
            // Just over half the catalogue has one; `description` is the
            // short line every item has.
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeSmall
                visible: text !== ""
                text: {
                    if (page.item.richText !== undefined && page.item.richText !== "")
                        return page.item.richText
                    return page.item.description !== undefined ? page.item.description : ""
                }
            }

            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                enabled: !AppController.storeBusy && page.item.id !== undefined
                text: page.item.inLibrary === true ? qsTr("Remove from library")
                                                    : qsTr("Add to library")
                onClicked: {
                    if (page.item.inLibrary === true)
                        AppController.removeStoreItem(page.item.id)
                    else
                        AppController.addStoreItem(page.item.id)
                }
            }
        }

        VerticalScrollDecorator {}
    }
}
