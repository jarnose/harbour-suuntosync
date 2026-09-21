import QtQuick 2.0
import Sailfish.Silica 1.0

CoverBackground {
    id: cover

    Label {
        anchors.centerIn: parent
        text: "Suunto Sync"
        color: Theme.primaryColor
        font.pixelSize: Theme.fontSizeMedium
        wrapMode: Text.WordWrap
        width: parent.width - 2 * Theme.paddingMedium
        horizontalAlignment: Text.AlignHCenter
    }
}
