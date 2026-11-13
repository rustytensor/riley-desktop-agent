// SPDX-License-Identifier: GPL-3.0-or-later
// The loop's window: a task in, the transcript of model turns and actions, the current state, stop and re-enable.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    objectName: "loopWindow"
    required property QtObject controller
    width: 720
    height: 520
    visible: true
    title: "Riley desktop agent"

    function submit() {
        if (taskField.text.trim().length === 0)
            return
        root.controller.run(taskField.text)
        taskField.text = ""
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            TextField {
                id: taskField
                objectName: "taskField"
                Layout.fillWidth: true
                placeholderText: "A task for the agent (segfault stops everything)"
                onAccepted: root.submit()
            }
            Button {
                objectName: "runButton"
                text: "Run"
                enabled: !root.controller.busy
                onClicked: root.submit()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Label {
                objectName: "stateLabel"
                Layout.fillWidth: true
                text: "State: " + root.controller.state
                font.bold: true
                color: root.controller.state === "running" ? "#c0392b" : palette.windowText
            }
            Button {
                objectName: "stopButton"
                text: "Stop"
                onClicked: root.controller.stop()
            }
            Button {
                objectName: "reenableButton"
                text: "Re-enable"
                enabled: !root.controller.busy
                onClicked: root.controller.reenable()
            }
        }

        ListView {
            id: transcriptView
            objectName: "transcriptView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.controller.transcript
            onCountChanged: positionViewAtEnd()
            delegate: Text {
                required property string modelData
                width: ListView.view.width
                text: modelData
                wrapMode: Text.Wrap
                font.family: "monospace"
                color: palette.windowText
            }
        }
    }
}
