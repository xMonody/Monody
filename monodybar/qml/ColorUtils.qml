import QtQuick

// Deterministic colour from a string, used for fallback icon tiles when no
// themed icon can be loaded.  Shared by the task icons (AppIcons.qml) and
// the launcher app grid (WinStart.qml).
QtObject {
    function hash(s) {
        var h = 0
        for (var i = 0; i < s.length; i++)
            h = (h * 31 + s.charCodeAt(i)) % 360
        return Qt.hsla(h / 360, 0.45, 0.42, 1)
    }
}
