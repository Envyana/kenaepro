"""KenaePRO for Maya: playblast straight into KenaePRO, and keep Maya's frame and KenaePRO's
playhead together.

Install: copy this file and python/kenaepro_client.py into your Maya scripts folder
(~/maya/scripts on Linux), start KenaePRO with its Client API on (File > Preferences > Client API,
or `kenaepro --api-port 4545`), then in Maya's Script Editor (Python):

    import kenaepro_maya
    kenaepro_maya.show()

- "Playblast to KenaePRO" playblasts the playback range to PNG frames and opens them in KenaePRO
  as one image sequence. Set KenaePRO's image sequence frame rate (Preferences) to the scene's.
- "Connect sync" keeps the frames together: scrubbing in Maya moves KenaePRO, and scrubbing or
  playing in KenaePRO moves Maya (polled ten times a second). The mapping is
  review_frame = round((maya_frame - base) * multiplier) + offset, where base is the start of the
  playback range unless set by hand.

Works with Maya's PySide2 or PySide6. Not yet tried inside Maya; the protocol and the frame mapping
are tested outside it (python/test_client.py).
"""

import os
import sys
import tempfile
import time

try:
    import kenaepro_client as kp
except ImportError:  # next to this file, but not on Maya's path
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import kenaepro_client as kp

import maya.cmds as cmds

try:
    from PySide6 import QtCore
except ImportError:
    from PySide2 import QtCore

WINDOW = "kenaeproSyncWindow"
POLL_MS = 100


class _Sync:
    client = None
    state = None
    job = None
    timer = None
    applying = False  # True while this script itself moves Maya's frame


def _message(text):
    cmds.inViewMessage(amg="KenaePRO: %s" % text, pos="topCenter", fade=True)
    print("KenaePRO: %s" % text)


def _field(name):
    return cmds.intField(name, query=True, value=True)


def _client(port):
    if _Sync.client is None or not _Sync.client.connected or _Sync.client.port != port:
        _Sync.client = kp.KenaeProClient(port=port, timeout=0.5).connect()
    return _Sync.client


# --- sync -----------------------------------------------------------------------------------

def _on_time_changed():
    if _Sync.applying or _Sync.state is None:
        return
    target = _Sync.state.push(int(round(cmds.currentTime(query=True))))
    if target is None:
        return
    try:
        _Sync.client.set_frame(target)
    except kp.ApiError:
        pass  # outside the clip in KenaePRO: nothing to show
    except kp.ConnectionLost as error:
        disconnect("lost the connection (%s)" % error)


def _poll():
    if _Sync.state is None:
        return
    try:
        frame, count = _Sync.client.get_frame()
    except (kp.ApiError, kp.ConnectionLost) as error:
        disconnect("lost the connection (%s)" % error)
        return
    current = int(round(cmds.currentTime(query=True)))
    target = _Sync.state.poll(current, frame, count)
    if target is not None:
        _Sync.applying = True
        try:
            cmds.currentTime(target, update=True)
        finally:
            _Sync.applying = False


def connect(port=kp.DEFAULT_PORT, base=None, offset=0, multiplier=1.0):
    """Starts the two-way frame sync. `base` defaults to the start of the playback range."""
    disconnect()
    if base is None:
        base = int(cmds.playbackOptions(query=True, minTime=True))
    try:
        _client(port)
    except (kp.ApiError, kp.ConnectionLost) as error:
        _message(str(error))
        return False
    _Sync.state = kp.SyncState(kp.FrameMap(base, offset, multiplier))
    _Sync.job = cmds.scriptJob(event=["timeChanged", _on_time_changed], protected=True)
    _Sync.timer = QtCore.QTimer()
    _Sync.timer.timeout.connect(_poll)
    _Sync.timer.start(POLL_MS)
    _on_time_changed()  # KenaePRO starts where Maya is
    _message("sync on (port %d)" % port)
    return True


def disconnect(reason=""):
    if _Sync.timer is not None:
        _Sync.timer.stop()
        _Sync.timer = None
    if _Sync.job is not None and cmds.scriptJob(exists=_Sync.job):
        cmds.scriptJob(kill=_Sync.job, force=True)
    _Sync.job = None
    was_on = _Sync.state is not None
    _Sync.state = None
    if _Sync.client is not None:
        _Sync.client.close()
    _Sync.client = None
    if reason or was_on:
        _message(reason or "sync off")


# --- playblast ------------------------------------------------------------------------------

def playblast(port=kp.DEFAULT_PORT, folder=None, scale=100):
    """Playblasts the playback range to PNG frames and opens them in KenaePRO. Returns the source
    id, or None."""
    start = int(cmds.playbackOptions(query=True, minTime=True))
    end = int(cmds.playbackOptions(query=True, maxTime=True))
    scene = os.path.splitext(os.path.basename(cmds.file(query=True, sceneName=True) or "untitled"))[0]
    folder = folder or os.path.join(tempfile.gettempdir(), "kenaepro_playblast")
    if not os.path.isdir(folder):
        os.makedirs(folder)
    base = os.path.join(folder, "%s_%s" % (scene, time.strftime("%Y%m%d_%H%M%S")))
    cmds.playblast(format="image", compression="png", filename=base, startTime=start, endTime=end,
                   framePadding=4, percent=scale, viewer=False, showOrnaments=False,
                   forceOverwrite=True, clearCache=True)
    first = "%s.%04d.png" % (base, start)
    if not os.path.isfile(first):
        _message("the playblast wrote no frames (looked for %s)" % first)
        return None
    try:
        source_id = _client(port).import_file(first, activate=True, sequence=True)
    except (kp.ApiError, kp.ConnectionLost) as error:
        _message(str(error))
        return None
    _message("playblast of frames %d-%d opened" % (start, end))
    return source_id


# --- window ---------------------------------------------------------------------------------

def show():
    if cmds.window(WINDOW, exists=True):
        cmds.deleteUI(WINDOW)
    cmds.window(WINDOW, title="KenaePRO", widthHeight=(300, 210), sizeable=False)
    cmds.columnLayout(adjustableColumn=True, rowSpacing=6, columnOffset=("both", 8))
    cmds.text(label="KenaePRO must run with its Client API on.", align="left")
    cmds.rowLayout(numberOfColumns=2, columnWidth2=(110, 170))
    cmds.text(label="Port")
    cmds.intField("kenaeproPort", value=kp.DEFAULT_PORT, minValue=1, maxValue=65535)
    cmds.setParent("..")
    cmds.button(label="Playblast to KenaePRO", height=30,
                command=lambda *_: playblast(port=_field("kenaeproPort")))
    cmds.separator(style="in")
    cmds.rowLayout(numberOfColumns=2, columnWidth2=(110, 170))
    cmds.text(label="Review frame offset")
    cmds.intField("kenaeproOffset", value=0)
    cmds.setParent("..")
    cmds.rowLayout(numberOfColumns=2, columnWidth2=(150, 150))
    cmds.button(label="Connect sync", command=lambda *_: connect(port=_field("kenaeproPort"),
                                                                  offset=_field("kenaeproOffset")))
    cmds.button(label="Disconnect", command=lambda *_: disconnect())
    cmds.setParent("..")
    cmds.showWindow(WINDOW)
