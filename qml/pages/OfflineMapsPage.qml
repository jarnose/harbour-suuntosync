import QtQuick 2.0
import Sailfish.Silica 1.0

// The watch's offline maps.
//
// Nothing here downloads anything. The regions are chosen in the account's
// map library over HTTPS, the watch is told the library changed, and the
// watch fetches the tiles itself over its own WiFi - the same division of
// labour the weather sync has. So "added" means the watch will start when
// it next has a network, not that anything has arrived.
//
// Sizes come from the catalogue: `transferSize` is what comes down the
// wire and `storageSize` is what it occupies on the watch. Both are
// summed over the region's sources, because a region offers several and
// the watch's own capabilities decide which.
Page {
    id: page

    property var regions: []
    property var library: []
    property string lastError: ""
    property bool showLibraryOnly: false

    function reload() {
        regions = AppController.mapCatalogue()
        library = AppController.mapLibrary()
    }

    function megabytes(bytes) {
        if (!bytes || bytes <= 0)
            return ""
        return (bytes / (1024 * 1024)).toFixed(bytes > 100 * 1024 * 1024 ? 0 : 1) + " MB"
    }

    // The cloud's own status vocabulary, all three measured: a region just
    // added reads REQUESTED, one the watch is fetching reads IN_PROGRESS,
    // and one it has reads FINISHED - with the watch's own
    // /MapsDownload/NumberOfAreas counting exactly the FINISHED ones.
    //
    // IN_PROGRESS is in here because the first version guessed DOWNLOADING
    // and the watch said otherwise. Anything not in this list is shown
    // verbatim: the server's own word beats a wrong translation of it.
    function statusText(status) {
        if (status === "FINISHED")
            return qsTr("On the watch")
        if (status === "REQUESTED")
            return qsTr("Waiting for the watch")
        if (status === "IN_PROGRESS")
            return qsTr("Downloading")
        return status
    }

    function startsGroup(index) {
        if (page.showLibraryOnly)
            return false
        return index === 0 || page.rows[index - 1].groupName !== page.rows[index].groupName
    }

    // A heading per group is what makes the whole catalogue navigable, but
    // while searching it is in the way: a match of three regions from three
    // groups would be nine rows of which six are headings.
    property bool showHeadings: filter.trim() === "" && !showLibraryOnly

    property string filter: ""

    // One list, three sources: everything, only what the account has, or
    // whatever the search matches. The filter looks at the region's name
    // and at its group's, so typing a country finds the areas inside it
    // as well as the country itself.
    property var rows: {
        var source = showLibraryOnly ? library : regions
        var needle = filter.trim().toLowerCase()
        if (needle === "")
            return source
        var out = []
        for (var i = 0; i < source.length; ++i) {
            var row = source[i]
            if (row.name.toLowerCase().indexOf(needle) >= 0
                    || (row.groupName && row.groupName.toLowerCase().indexOf(needle) >= 0))
                out.push(row)
        }
        return out
    }

    Component.onCompleted: {
        reload()
        if (regions.length === 0)
            AppController.refreshMaps()
    }

    Connections {
        target: AppController
        onMapsChanged: page.reload()
        onErrorOccurred: page.lastError = message
    }

    // The search field and the notes sit **outside** the list on purpose.
    // As the list's own header they were recreated every time the model
    // changed - which a filter does on every keystroke - so the field was
    // destroyed mid-word and the keyboard closed with it.
    Column {
        id: topArea
        anchors.top: parent.top
        width: parent.width

        PageHeader { title: qsTr("Offline maps") }

        // Every country in the world is a long list to thumb through.
        SearchField {
            width: parent.width
            placeholderText: qsTr("Search regions")
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            onTextChanged: page.filter = text
            EnterKey.iconSource: "image://theme/icon-m-enter-close"
            EnterKey.onClicked: focus = false
        }

        Label {
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * Theme.horizontalPageMargin
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.secondaryColor
            visible: page.filter.trim() === ""
            text: qsTr("The watch downloads the maps itself over WiFi. Adding a region "
                       + "puts it in your map library and tells the watch to go and "
                       + "look; nothing is downloaded by the phone. "
                       + "The watch then asks you to accept the download on its own "
                       + "screen, and shows the progress there.")
        }

        Label {
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * Theme.horizontalPageMargin
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.highlightColor
            visible: page.lastError !== ""
            text: page.lastError
        }

        Label {
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * Theme.horizontalPageMargin
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.secondaryColor
            visible: page.filter.trim() !== "" && page.rows.length > 0
            text: qsTr("%n region(s)", "", page.rows.length)
        }

        Label {
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * Theme.horizontalPageMargin
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.secondaryColor
            visible: page.rows.length === 0 && !AppController.mapsBusy
                     && page.filter.trim() !== ""
            text: qsTr("Nothing matches that.")
        }

        Label {
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * Theme.horizontalPageMargin
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.secondaryColor
            visible: page.rows.length === 0 && !AppController.mapsBusy
                     && page.filter.trim() === ""
            text: AppController.cloudSignedIn
                   ? qsTr("Connect the watch and pull down to refresh.")
                   : qsTr("Sign in to the Suunto cloud to see the regions.")
        }
    }

    SilicaListView {
        id: listView
        anchors.top: topArea.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        clip: true
        model: page.rows

        PullDownMenu {
            busy: AppController.mapsBusy
            MenuItem {
                text: qsTr("Refresh")
                onClicked: AppController.refreshMaps()
            }
            MenuItem {
                text: page.showLibraryOnly ? qsTr("Show every region")
                                            : qsTr("Show only mine")
                onClicked: page.showLibraryOnly = !page.showLibraryOnly
            }
        }

        delegate: Column {
            width: listView.width

            SectionHeader {
                text: modelData.groupName
                visible: page.showHeadings && page.startsGroup(index)
                          && modelData.groupName !== ""
            }

            ListItem {
                id: item
                width: parent.width
                contentHeight: content.height + Theme.paddingMedium
                enabled: !AppController.mapsBusy

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
                        color: modelData.inLibrary ? Theme.highlightColor
                                                    : Theme.secondaryColor
                        text: {
                            var parts = []
                            // "In your library" is what the status says in
                            // the library view, so it is not said twice.
                            if (modelData.inLibrary && !page.showLibraryOnly)
                                parts.push(qsTr("In your library"))
                            if (modelData.status)
                                parts.push(page.statusText(modelData.status))
                            var total = page.megabytes(modelData.transferSize)
                            // Part way down, the two numbers together say
                            // more than either alone.
                            if (modelData.downloadedSize > 0
                                    && modelData.status !== "FINISHED" && total) {
                                parts.push(page.megabytes(modelData.downloadedSize)
                                            + " / " + total)
                            } else if (total) {
                                parts.push(total)
                            }
                            return parts.join(" · ")
                        }
                    }

                    Label {
                        width: parent.width
                        visible: modelData.description !== ""
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        font.pixelSize: Theme.fontSizeExtraSmall
                        color: Theme.secondaryColor
                        text: modelData.description
                    }
                }

                menu: ContextMenu {
                    MenuItem {
                        visible: !modelData.inLibrary && !page.showLibraryOnly
                        text: qsTr("Add and tell the watch")
                        onClicked: AppController.addMapRegion(modelData.id)
                    }
                    MenuItem {
                        visible: modelData.inLibrary || page.showLibraryOnly
                        text: qsTr("Remove from the library")
                        onClicked: AppController.removeMapRegion(modelData.id)
                    }
                }
            }
        }

        ViewPlaceholder {
            enabled: AppController.mapsBusy && page.rows.length === 0
            text: qsTr("Asking the cloud")
        }

        VerticalScrollDecorator {}
    }
}
