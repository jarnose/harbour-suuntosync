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
//
// The directory is also what puts plugins on this page that the library has
// never heard of - a development build pushed by hand, which the watch keeps
// as <id>.dev rather than <id>.fea. They show their file name and offer one
// action, removal, because that is all this app knows about them. Before
// that they were invisible here and could not be removed at all, while still
// taking up a slot on the watch.
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
                        // A plugin the watch has but the library does not
                        // know about was put there by something else - a
                        // development build pushed by hand. It says so,
                        // because the only thing this app knows about it is
                        // its file name, and installing it again is not
                        // something this page can offer.
                        text: {
                            // The watch's registry still names it while the
                            // file is gone, which is what a nameless row in
                            // the watch's own SuuntoPlus menu is.
                            if (modelData.registryOnly)
                                return qsTr("In the watch's list with no file left")
                            if (modelData.onWatchOnly)
                                return qsTr("On the watch only, not in your library")
                            return modelData.installed ? qsTr("On the watch")
                                                        : qsTr("Not on the watch")
                        }
                    }
                }

                menu: ContextMenu {
                    MenuItem {
                        visible: !modelData.installed && !modelData.registryOnly
                        text: qsTr("Install on the watch")
                        onClicked: AppController.installSuuntoPlusItem(modelData.id)
                    }
                    // The only thing to do with a leftover: take it out of
                    // the watch's list, which is where it is still drawn.
                    MenuItem {
                        visible: modelData.registryOnly
                        text: qsTr("Remove from the watch's list")
                        onClicked: AppController.removeSuuntoPlusRegistryEntry(modelData.id)
                    }
                    MenuItem {
                        visible: modelData.installed && !modelData.onWatchOnly
                        text: qsTr("Remove from the watch")
                        onClicked: AppController.removeSuuntoPlusItem(modelData.id)
                    }
                    // Same deletion, by file name: this row has no
                    // catalogue id to look one up with.
                    MenuItem {
                        visible: modelData.onWatchOnly
                        text: qsTr("Remove from the watch")
                        onClicked: AppController.removeSuuntoPlusFile(modelData.fileName)
                    }
                    // Only while the watch does not have it. This takes the
                    // item out of the account's library and leaves the watch
                    // alone, so offering it for an installed item would
                    // leave the file on the watch while the row vanished
                    // from this page - the page lists the library. Removing
                    // from the watch first is then the only route, and the
                    // two actions never both apply.
                    MenuItem {
                        visible: !modelData.installed && !modelData.onWatchOnly
                                 && !modelData.registryOnly
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
