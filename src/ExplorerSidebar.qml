import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs as Dialogs

Rectangle {
    id: sidebar
    objectName: "explorerSidebar"
    required property var window
    property url selectedUrl
    property bool selectedDirectory: false
    property bool dragging: false
    signal openFile(url file)
    color: window.pageColor

    component SidebarButton: Button {
        id: button
        implicitHeight: window.scaledSize(36)
        background: Rectangle {
            radius: window.scaledSize(4)
            color: button.down ? window.selectionFill : Qt.tint(sidebar.color, button.hovered ? "#20ffffff" : "#0cffffff")
            border.width: 1
            border.color: Qt.tint(sidebar.color, "#24ffffff")
        }
        contentItem: Label {
            text: button.text
            color: window.textColor
            font.pixelSize: window.scaledSize(13)
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }

    Item {
        id: dragGhost
        parent: Overlay.overlay
        property url sourceUrl
        width: window.scaledSize(190)
        height: window.scaledSize(34)
        visible: sidebar.dragging
        z: 1000
        Drag.active: sidebar.dragging
        Drag.source: dragGhost
        Drag.keys: ["omawrite-entry"]
        Drag.supportedActions: Qt.MoveAction
        Drag.hotSpot.x: 8
        Drag.hotSpot.y: 8
        Rectangle {
            anchors.fill: parent
            radius: 4
            color: window.selectionFill
            border.color: backend.themeAccent
            Label {
                anchors.fill: parent
                anchors.margins: 6
                text: explorer.nameForUrl(dragGhost.sourceUrl)
                color: window.textColor
                elide: Text.ElideRight
            }
        }
    }

    function beginCreate(folder) {
        entryDialog.mode = folder ? "folder" : "file";
        entryDialog.target = selectedUrl.toString().length === 0 ? explorer.folderUrl
            : selectedDirectory ? selectedUrl : explorer.parentFolder(selectedUrl);
        entryName.text = folder ? "" : "Untitled.md";
        entryDialog.open();
    }

    function rootMenu() {
        sidebar.selectedUrl = explorer.folderUrl;
        sidebar.selectedDirectory = true;
        entryMenu.popup();
    }

    function dropIntoRoot(drop) {
        if (explorer.moveEntry(drop.source.sourceUrl, explorer.folderUrl)) {
            sidebar.selectedUrl = backend.fileUrl.toString().length > 0
                ? backend.fileUrl : explorer.folderUrl;
            sidebar.selectedDirectory = sidebar.selectedUrl === explorer.folderUrl;
            drop.accept(Qt.MoveAction);
        }
    }

    Connections {
        target: explorer
        function onFolderChanged() {
            sidebar.selectedUrl = explorer.folderUrl;
            sidebar.selectedDirectory = true;
        }
    }
    Connections {
        target: backend
        function onFileUrlChanged() {
            sidebar.selectedUrl = backend.fileUrl;
            sidebar.selectedDirectory = false;
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            objectName: "workspaceHeader"
            Layout.fillWidth: true
            implicitHeight: window.scaledSize(40)
            Label {
                anchors.fill: parent
                anchors.leftMargin: window.scaledSize(12)
                anchors.rightMargin: window.scaledSize(13)
                text: explorer.folderName + "/"
                color: workspaceMouse.containsMouse ? window.textColor : window.mutedColor
                font.family: backend.editorFont
                font.pixelSize: window.scaledSize(12)
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideMiddle
            }
            MouseArea {
                id: workspaceMouse
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: function(mouse) {
                    if (mouse.button === Qt.RightButton) sidebar.rootMenu();
                    else folderDialog.open();
                }
            }
            ToolTip.visible: workspaceMouse.containsMouse
            ToolTip.delay: 600
            ToolTip.text: "Choose workspace · Drop files here to move to the top level"
            DropArea {
                anchors.fill: parent
                keys: ["omawrite-entry"]
                onDropped: function(drop) { sidebar.dropIntoRoot(drop); }
                Rectangle {
                    anchors.fill: parent
                    visible: parent.containsDrag
                    color: window.selectionFill
                    opacity: 0.25
                }
            }
        }
        TreeView {
            id: tree
            objectName: "explorerTree"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: explorer.treeModel
            reuseItems: false
            rootIndex: explorer.folderIndex
            columnWidthProvider: function(column) { return tree.width; }
            ScrollBar.vertical: ScrollBar {}
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: function(point) {
                    var position = tree.mapToItem(tree.contentItem, point.position.x, point.position.y);
                    if (tree.cellAtPosition(position).y < 0) sidebar.rootMenu();
                }
            }
            DropArea {
                anchors.fill: parent
                z: -1
                keys: ["omawrite-entry"]
                onDropped: function(drop) { sidebar.dropIntoRoot(drop); }
            }
            delegate: ItemDelegate {
                id: entry
                required property int row
                required property int column
                required property int depth
                required property bool expanded
                required property bool hasChildren
                required property string fileName
                required property string filePath
                objectName: "entry:" + fileName
                readonly property url entryUrl: explorer.urlForPath(filePath)
                readonly property bool folder: explorer.isDirectory(filePath)
                implicitWidth: tree.width
                implicitHeight: window.scaledSize(26)
                leftPadding: window.scaledSize(12) + entry.depth * window.scaledSize(16)
                rightPadding: window.scaledSize(12)
                topPadding: 0
                bottomPadding: 0
                highlighted: entryUrl === sidebar.selectedUrl
                background: Rectangle {
                    color: window.selectionFill
                    opacity: folderDrop.containsDrag ? 0.4
                         : entry.highlighted ? 0.12 : dragMouse.containsMouse ? 0.2 : 0
                }
                contentItem: Label {
                    text: entry.fileName + (entry.folder ? "/" : "")
                    color: !entry.folder && entry.entryUrl === backend.fileUrl ? backend.themeAccent
                         : entry.folder ? window.mutedColor : window.textColor
                    font.family: backend.editorFont
                    font.pixelSize: window.scaledSize(13)
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
                MouseArea {
                    id: dragMouse
                    z: 1
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    hoverEnabled: true
                    drag.target: dragGhost
                    drag.onActiveChanged: sidebar.dragging = drag.active
                    onPressed: function(mouse) {
                        var point = mapToItem(dragGhost.parent, mouse.x, mouse.y);
                        dragGhost.x = point.x - 8;
                        dragGhost.y = point.y - 8;
                        dragGhost.sourceUrl = entry.entryUrl;
                    }
                    onReleased: {
                        if (sidebar.dragging) dragGhost.Drag.drop();
                    }
                    onCanceled: sidebar.dragging = false
                    onClicked: function(mouse) {
                        if (mouse.button === Qt.RightButton || entry.folder) {
                            sidebar.selectedUrl = entry.entryUrl;
                            sidebar.selectedDirectory = entry.folder;
                        }
                        if (mouse.button === Qt.RightButton) entryMenu.popup();
                        else if (entry.folder) tree.toggleExpanded(entry.row);
                        else {
                            sidebar.openFile(entry.entryUrl);
                            if (backend.fileUrl === entry.entryUrl) {
                                sidebar.selectedUrl = entry.entryUrl;
                                sidebar.selectedDirectory = false;
                            }
                        }
                    }
                }
                DropArea {
                    id: folderDrop
                    anchors.fill: parent
                    enabled: entry.folder
                    keys: ["omawrite-entry"]
                    onDropped: function(drop) {
                        if (explorer.moveEntry(drop.source.sourceUrl, entry.entryUrl)) {
                            sidebar.selectedUrl = entry.entryUrl;
                            sidebar.selectedDirectory = true;
                            tree.expand(entry.row);
                            drop.accept(Qt.MoveAction);
                        }
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: tree.rows === 0
                text: "Nothing here yet"
                horizontalAlignment: Text.AlignHCenter
                color: window.mutedColor
                font.family: backend.editorFont
                font.pixelSize: window.scaledSize(13)
            }
        }
        Rectangle {
            objectName: "rootDropTarget"
            visible: sidebar.dragging
            Layout.fillWidth: true
            implicitHeight: window.scaledSize(34)
            radius: window.scaledSize(4)
            color: rootDrop.containsDrag ? window.selectionFill : "transparent"
            border.width: sidebar.dragging ? 1 : 0
            border.color: backend.themeAccent
            Label {
                anchors.fill: parent
                anchors.margins: 6
                text: "↩ Move to top level"
                color: rootDrop.containsDrag ? window.textColor : window.mutedColor
                font.pixelSize: window.scaledSize(11)
                verticalAlignment: Text.AlignVCenter
            }
            DropArea {
                id: rootDrop
                anchors.fill: parent
                keys: ["omawrite-entry"]
                onDropped: function(drop) { sidebar.dropIntoRoot(drop); }
            }
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                onClicked: sidebar.rootMenu()
            }
        }
        Label {
            Layout.fillWidth: true
            Layout.leftMargin: window.scaledSize(12)
            Layout.rightMargin: window.scaledSize(12)
            text: explorer.error
            visible: text.length > 0
            wrapMode: Text.Wrap
            color: window.textColor
            font.pixelSize: window.scaledSize(11)
        }
    }
    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: window.mutedColor
        opacity: 0.2
    }
    Menu {
        id: entryMenu
        objectName: "explorerContextMenu"
        // Keep the popup in QML so desktop/native menus cannot override its theme.
        popupType: Popup.Item
        width: window.scaledSize(176)
        padding: window.scaledSize(4)
        overlap: 0
        enter: Transition {}
        exit: Transition {}
        background: Rectangle {
            color: window.pageColor
            border.width: 1
            border.color: Qt.rgba(window.mutedColor.r, window.mutedColor.g, window.mutedColor.b, 0.3)
        }
        Instantiator {
            model: sidebar.selectedUrl.toString().length > 0 && sidebar.selectedUrl !== explorer.folderUrl ? 4 : 2
            onObjectAdded: function(index, item) { entryMenu.insertItem(index, item); }
            onObjectRemoved: function(index, item) { entryMenu.removeItem(item); }
            delegate: MenuItem {
                id: menuEntry
                required property int index
                objectName: index === 3 ? "deleteEntryMenuItem" : index === 2 ? "renameEntryMenuItem" : "createEntryMenuItem:" + index
                text: ["New file", "New folder", "Rename", "Move to Trash"][index]
                implicitHeight: window.scaledSize(26)
                leftPadding: window.scaledSize(10)
                rightPadding: window.scaledSize(10)
                topPadding: 0
                bottomPadding: 0
                indicator: null
                arrow: null
                contentItem: Text {
                    text: menuEntry.text
                    color: menuEntry.highlighted ? backend.themeAccent : window.textColor
                    font.family: backend.editorFont
                    font.pixelSize: window.scaledSize(12)
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
                background: Rectangle {
                    color: window.selectionFill
                    opacity: menuEntry.down ? 0.35 : menuEntry.highlighted ? 0.18 : 0
                }
                onTriggered: {
                    if (index < 2) {
                        sidebar.beginCreate(index === 1);
                    } else if (index === 2) {
                        entryDialog.mode = "rename";
                        entryDialog.target = sidebar.selectedUrl;
                        entryName.text = explorer.nameForUrl(sidebar.selectedUrl);
                        entryDialog.open();
                    } else {
                        deleteDialog.target = sidebar.selectedUrl;
                        deleteDialog.folder = sidebar.selectedDirectory;
                        deleteDialog.open();
                    }
                }
            }
        }
    }
    Dialog {
        id: deleteDialog
        property url target
        property bool folder: false
        modal: true
        title: folder ? "Delete folder" : "Delete file"
        anchors.centerIn: Overlay.overlay
        width: Math.min(window.width - 40, window.scaledSize(380))
        contentItem: ColumnLayout {
            Label {
                Layout.fillWidth: true
                text: "Move “" + explorer.nameForUrl(deleteDialog.target) + "”"
                    + (deleteDialog.folder ? " and its contents" : "") + " to Trash?"
                wrapMode: Text.Wrap
                color: window.textColor
            }
            Label {
                Layout.fillWidth: true
                text: explorer.error
                visible: text.length > 0
                wrapMode: Text.Wrap
                color: window.textColor
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                SidebarButton { text: "Cancel"; onClicked: deleteDialog.close() }
                SidebarButton {
                    text: "Move to Trash"
                    onClicked: {
                        if (explorer.trashEntry(deleteDialog.target)) {
                            sidebar.selectedUrl = explorer.folderUrl;
                            sidebar.selectedDirectory = true;
                            deleteDialog.close();
                        }
                    }
                }
            }
        }
    }
    Dialogs.FolderDialog {
        id: folderDialog
        title: "Open notes folder"
        currentFolder: explorer.folderUrl
        onAccepted: explorer.chooseFolder(selectedFolder)
    }
    Dialog {
        id: entryDialog
        property string mode: "file"
        property url target
        title: mode === "rename" ? "Rename" : mode === "folder" ? "New folder" : "New file"
        modal: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(window.width - 40, window.scaledSize(380))
        onOpened: {
            entryName.forceActiveFocus();
            entryName.selectAll();
        }
        function submit() {
            if (mode === "rename") {
                if (explorer.renameEntry(target, entryName.text)) close();
            } else {
                var created = explorer.createEntry(target, entryName.text, mode === "folder");
                if (created.toString().length > 0) {
                    close();
                    if (mode === "file") sidebar.openFile(created);
                }
            }
        }
        contentItem: ColumnLayout {
            TextField {
                id: entryName
                Layout.fillWidth: true
                placeholderText: entryDialog.mode === "folder" ? "Folder name" : "File name.md or name.txt"
                onAccepted: entryDialog.submit()
            }
            Label {
                Layout.fillWidth: true
                text: explorer.error
                visible: text.length > 0
                wrapMode: Text.Wrap
                color: window.textColor
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                SidebarButton { text: "Cancel"; onClicked: entryDialog.close() }
                SidebarButton {
                    text: entryDialog.mode === "rename" ? "Rename" : "Create"
                    enabled: entryName.text.trim().length > 0
                    onClicked: entryDialog.submit()
                }
            }
        }
    }
}
