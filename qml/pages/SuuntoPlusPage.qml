import QtQuick 2.0
import Sailfish.Silica 1.0

// The account's SuuntoPlus library, and which of it the watch has.
//
// Three kinds live here, grouped under their own headings: watch faces,
// sports apps and guides (workout plans). The cloud keeps guides in a
// listing of their own - features/items never answers a `sequence` - so a
// guide on the watch was invisible on this page until that second request
// was made. AppController does both and hands over one list sorted by kind.
//
// "Installed" is read from the watch's own plugin directory, not from its
// /Plugin/List resource. That distinction cost a long evening and is worth
// restating here: /Plugin/List reports less than what is installed - fifteen
// names where the directory held seventeen plugin files, and the official
// Suunto app's own pushes are missing from it too. The directory is the
// truth. See docs/suuntoplus-and-watchfaces.md.
//
// A sports app is kept on the watch as <pluginId>.fea, a watch face as
// <id>.wfa and a guide as <id>.gui; the zip that was uploaded does not
// survive the install.
Page {
    id: page

    property var items: []
    property string lastError: ""

    function reload() {
        items = AppController.suuntoPlusItems()
    }

    // The cloud's own type names. "feature" covers anything that is not a
    // watch face or a guide, so it is the fallback rather than a case.
    function kindTitle(type) {
        if (type === "watchface")
            return qsTr("Watch faces")
        if (type === "sequence")
            return qsTr("Guides")
        return qsTr("Sports apps")
    }

    function startsGroup(index) {
        return index === 0 || page.items[index - 1].type !== page.items[index].type
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
            MenuItem {
                text: qsTr("Store")
                onClicked: pageStack.push(Qt.resolvedUrl("StorePage.qml"))
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

        // A Column, not the ListItem itself, so each group can carry its own
        // heading. SectionHeader is drawn for the first row of a kind and
        // left invisible for the rest, which a Column leaves out of its
        // layout entirely - the ListView's own section support is no use
        // here, because a plain JavaScript array exposes no roles for
        // section.property to read.
        delegate: Column {
            width: listView.width

            SectionHeader {
                text: page.kindTitle(modelData.type)
                visible: page.startsGroup(index)
            }

            ListItem {
                id: item
                width: parent.width
                contentHeight: content.height + Theme.paddingMedium
                // Nothing to do while a BLE write is in flight, and the
                // watch takes one request at a time anyway.
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
                        color: item.highlighted ? Theme.highlightColor
                                                 : Theme.primaryColor
                    }

                    Label {
                        width: parent.width
                        truncationMode: TruncationMode.Fade
                        font.pixelSize: Theme.fontSizeExtraSmall
                        color: modelData.installed ? Theme.highlightColor
                                                    : Theme.secondaryColor
                        text: modelData.installed ? qsTr("On the watch")
                                                   : qsTr("Not on the watch")
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
                    // Deliberately not chained to the one above: this takes
                    // the item out of the account's library and leaves the
                    // watch alone, so an item removed while installed stays
                    // on the watch - and stops being listed here, because
                    // this page lists the library.
                    MenuItem {
                        text: qsTr("Remove from library")
                        onClicked: AppController.removeSuuntoPlusFromLibrary(modelData.id)
                    }
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
