import Ame.Ui 1.0

Item {
    id: root
    property int count: 0
    readonly property int doubled: root.count * 2
    property bool active: true
    width: 360
    height: 240

    Rectangle {
        anchors.fill: parent
        color: "#182333"
    }
    // Declared early but ordered behind the column: negative z only competes
    // with siblings, and children always paint above their parent.
    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: 320
        height: 200
        z: -1
        color: "#1f2d40"
    }
    Column {
        x: 16
        y: 16
        width: 328
        spacing: 12

        Text {
            text: "Ame QML counter"
            color: "#ffffff"
        }
        Text {
            id: valueLabel
            text: "Count: " + root.count + " / doubled: " + root.doubled
            color: "#c6d8ed"
        }
        Button {
            id: increment
            width: 328
            height: 48
            text: "Increment"
            color: root.active ? "#295b99" : "#555555"
            enabled: root.active
            onClicked: { root.count += 1; }
        }
        Button {
            id: reset
            width: 328
            height: 40
            text: "Reset"
            color: "#38506b"
            onClicked: { root.count = 0; }
        }
    }

    Rectangle {
        id: badge
        anchors.centerIn: parent
        width: 96
        height: 24
        z: 5
        color: "#20364d"
        Text {
            anchors.centerIn: parent
            text: root.active ? "active" : "paused"
            color: "#9fd0ff"
        }
    }
}
