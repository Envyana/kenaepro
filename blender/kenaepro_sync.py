"""KenaePRO sync: keeps Blender's current frame and KenaePRO's playhead together.

Install: Blender > Edit > Preferences > Add-ons > Install... and pick this file, then enable
"Animation: KenaePRO sync". Start KenaePRO with its Client API on (File > Preferences >
Client API, or `kenaepro --api-port 4545`). Open the "KenaePRO" tab in a 3D viewport's
sidebar (N key) and press Connect.

How the frames line up (both counts start at the first frame of what is shown):

    review_frame  = round((blender_frame - base) * multiplier) + offset
    blender_frame = round((review_frame - offset) / multiplier) + base

`base` is the scene's start frame (or a frame you choose), `offset` is the review frame that `base`
matches, and `multiplier` converts between frame rates (24 fps review of a 48 fps scene: 0.5).

The add-on uses only commands the app's Client API already has: initialize, get_frame, set_frame
and set_playing. Blender talks to the app on its main thread with a short timeout, so a stuck app
disconnects the add-on instead of freezing Blender.

The part above the `bpy` section is plain Python and is tested outside Blender
(blender/test_sync_protocol.py).
"""

bl_info = {
    "name": "KenaePRO sync",
    "author": "KenaePRO contributors",
    "version": (0, 3, 0),
    "blender": (3, 0, 0),
    "location": "3D Viewport > Sidebar > KenaePRO",
    "description": "Keep the current frame in step with a running KenaePRO",
    "category": "Animation",
}

import json
import socket

try:  # Blender provides bpy; the plain-Python tests do not.
    import bpy
    from bpy.app.handlers import persistent
except ImportError:  # pragma: no cover - exercised only outside Blender
    bpy = None

API_VERSION = 1
DEFAULT_PORT = 4545
SOCKET_TIMEOUT = 0.5  # seconds; the API answers locally, so anything longer means trouble


class ApiError(Exception):
    """The app answered, but refused the command."""


class ConnectionLost(Exception):
    """The socket failed or timed out; the connection is no longer usable."""


class ApiClient:
    """One connection to the app: one JSON object per line in each direction."""

    def __init__(self, host="127.0.0.1", port=DEFAULT_PORT, timeout=SOCKET_TIMEOUT):
        self.host = host
        self.port = port
        self.timeout = timeout
        self._sock = None
        self._buffer = b""

    @property
    def connected(self):
        return self._sock is not None

    def connect(self):
        self.close()
        try:
            sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        except OSError as error:
            raise ConnectionLost("cannot reach %s:%s (%s)" % (self.host, self.port, error))
        sock.settimeout(self.timeout)
        self._sock = sock
        self._buffer = b""
        try:
            self.call("initialize", api_version=API_VERSION)
        except ApiError as error:
            # The app keeps one initialised state for the whole process, so a second connection
            # (after a reconnect, or from a script) is told so. It still works.
            if "Already initialized" not in str(error):
                self.close()
                raise

    def close(self):
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
        self._sock = None
        self._buffer = b""

    def call(self, cmd, **params):
        """Sends one command and returns its `result` dict."""
        if self._sock is None:
            raise ConnectionLost("not connected")
        request = dict(params)
        request["cmd"] = cmd
        try:
            self._sock.sendall(json.dumps(request).encode("utf-8") + b"\n")
            line = self._read_line()
        except (OSError, socket.timeout) as error:
            self.close()
            raise ConnectionLost(str(error) or error.__class__.__name__)
        try:
            response = json.loads(line.decode("utf-8"))
        except ValueError:
            self.close()
            raise ConnectionLost("the app sent something that is not JSON")
        if not response.get("success"):
            raise ApiError(str(response.get("msg", "command failed")))
        return response.get("result", {})

    def _read_line(self):
        while b"\n" not in self._buffer:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise OSError("the app closed the connection")
            self._buffer += chunk
        line, _, self._buffer = self._buffer.partition(b"\n")
        return line


class FrameMap:
    """Converts between Blender frames and review frames (0-based, in what the viewer shows)."""

    def __init__(self, base=1, offset=0, multiplier=1.0):
        self.base = int(base)
        self.offset = int(offset)
        self.multiplier = float(multiplier) if float(multiplier) > 0 else 1.0

    def to_review(self, blender_frame):
        return int(round((blender_frame - self.base) * self.multiplier)) + self.offset

    def to_blender(self, review_frame):
        return int(round((review_frame - self.offset) / self.multiplier)) + self.base


class SyncState:
    """The two-way logic, free of Blender so it can be tested.

    `push(blender_frame)` is called when Blender's frame changed; `poll(blender_frame)` on a timer.
    Each returns the frame the *other* side should move to, or None. A frame that one side just
    received from the other is never sent back (that would make the two ping-pong).
    """

    def __init__(self, frame_map):
        self.map = frame_map
        self.frame_count = None  # known after the first poll
        self._last_review = None  # the review frame both sides last agreed on

    def push(self, blender_frame):
        review = self.map.to_review(blender_frame)
        if self.frame_count:
            review = max(0, min(self.frame_count - 1, review))
        if review == self._last_review:
            return None
        self._last_review = review
        return review

    def poll(self, blender_frame, review_frame, frame_count):
        self.frame_count = frame_count
        if review_frame == self._last_review:
            return None
        self._last_review = review_frame
        target = self.map.to_blender(review_frame)
        return None if target == blender_frame else target


if bpy is not None:
    # --- Blender part ------------------------------------------------------------------------

    _client = None
    _state = None
    _scene_name = None
    _applying = False  # True while the add-on itself moves Blender's frame
    _was_playing = False
    _last_error = ""
    _poll_interval = 0.1

    def _redraw():
        for window in bpy.context.window_manager.windows:
            for area in window.screen.areas:
                if area.type == "VIEW_3D":
                    area.tag_redraw()

    def _settings(scene):
        return scene.kenaepro_sync

    def _build_state(scene):
        s = _settings(scene)
        base = scene.frame_start if s.use_scene_start else s.base_frame
        return SyncState(FrameMap(base, s.offset, s.multiplier))

    def _disconnect(message=""):
        global _client, _state, _scene_name, _last_error
        if _client is not None:
            _client.close()
        _client = None
        _state = None
        _scene_name = None
        if message:
            _last_error = message
        if bpy.app.timers.is_registered(_poll):
            bpy.app.timers.unregister(_poll)
        _redraw()

    def _scene():
        return bpy.data.scenes.get(_scene_name) if _scene_name else None

    def _poll():
        """Timer: follow the app's playhead (and play/pause when mirroring)."""
        global _was_playing, _applying
        scene = _scene()
        if _client is None or scene is None:
            return None
        try:
            s = _settings(scene)
            playing = bool(bpy.context.screen and bpy.context.screen.is_animation_playing)
            if s.mirror_play and playing != _was_playing:
                _client.call("set_playing", playing=playing)
                _was_playing = playing
                if not playing:
                    _push(scene)  # land on the frame Blender stopped at
            elif not s.mirror_play:
                _was_playing = playing
            if s.mirror_play and playing:
                return _poll_interval  # the app plays by itself; do not pull it back

            result = _client.call("get_frame")
            target = _state.poll(scene.frame_current, int(result.get("frame", 0)), int(result.get("frame_count", 0)))
            if target is not None:
                _applying = True
                try:
                    scene.frame_set(target)
                finally:
                    _applying = False
        except (ConnectionLost, ApiError) as error:
            _disconnect("lost the connection: %s" % error)
            return None
        return _poll_interval

    def _push(scene):
        review = _state.push(scene.frame_current)
        if review is None:
            return
        try:
            _client.call("set_frame", frame=review)
        except ApiError:
            pass  # for example an empty project: the frame is simply not shown
        except ConnectionLost as error:
            _disconnect("lost the connection: %s" % error)

    @persistent
    def _on_frame_change(scene, depsgraph=None):
        if _client is None or _applying or scene.name != _scene_name:
            return
        s = _settings(scene)
        if s.mirror_play and bpy.context.screen and bpy.context.screen.is_animation_playing:
            return  # the app plays on its own while Blender plays
        _push(scene)

    class KenaeProSyncSettings(bpy.types.PropertyGroup):
        host: bpy.props.StringProperty(name="Host", default="127.0.0.1")
        port: bpy.props.IntProperty(name="Port", default=DEFAULT_PORT, min=1, max=65535)
        use_scene_start: bpy.props.BoolProperty(
            name="Scene start is review frame",
            description="Count from the scene's start frame; otherwise from the base frame below",
            default=True,
        )
        base_frame: bpy.props.IntProperty(name="Base frame", default=1)
        offset: bpy.props.IntProperty(
            name="Offset",
            description="Review frame (0-based) that the base frame matches",
            default=0,
        )
        multiplier: bpy.props.FloatProperty(
            name="Rate ratio",
            description="Review frames per Blender frame (24 fps review of a 48 fps scene: 0.5)",
            default=1.0,
            min=0.001,
            soft_max=8.0,
        )
        mirror_play: bpy.props.BoolProperty(
            name="Mirror play/pause",
            description="Start and stop the app's playback with Blender's",
            default=False,
        )

    class KENAEPRO_OT_connect(bpy.types.Operator):
        bl_idname = "kenaepro.connect"
        bl_label = "Connect"
        bl_description = "Connect to a running KenaePRO and keep the frames in step"

        def execute(self, context):
            global _client, _state, _scene_name, _last_error, _was_playing, _applying
            _disconnect()
            s = _settings(context.scene)
            client = ApiClient(s.host, s.port)
            try:
                client.connect()
                result = client.call("get_frame")
            except (ConnectionLost, ApiError) as error:
                _last_error = str(error)
                self.report({"ERROR"}, "KenaePRO: %s" % error)
                _redraw()
                return {"CANCELLED"}
            _client = client
            _scene_name = context.scene.name
            _state = _build_state(context.scene)
            _state.frame_count = int(result.get("frame_count", 0)) or None
            _was_playing = False
            _last_error = ""
            # The app's frame wins on connect, so that opening a review does not move it.
            target = _state.poll(context.scene.frame_current, int(result.get("frame", 0)),
                                 int(result.get("frame_count", 0)))
            if target is not None:
                _applying = True
                try:
                    context.scene.frame_set(target)
                finally:
                    _applying = False
            if not bpy.app.timers.is_registered(_poll):
                bpy.app.timers.register(_poll, first_interval=_poll_interval)
            _redraw()
            return {"FINISHED"}

    class KENAEPRO_OT_disconnect(bpy.types.Operator):
        bl_idname = "kenaepro.disconnect"
        bl_label = "Disconnect"

        def execute(self, context):
            _disconnect()
            return {"FINISHED"}

    class KENAEPRO_OT_push(bpy.types.Operator):
        bl_idname = "kenaepro.push"
        bl_label = "Send frame"
        bl_description = "Move KenaePRO to Blender's current frame"

        @classmethod
        def poll(cls, context):
            return _client is not None

        def execute(self, context):
            _state._last_review = None  # send even if nothing seems to have changed
            _push(context.scene)
            return {"FINISHED"}

    class KENAEPRO_OT_pull(bpy.types.Operator):
        bl_idname = "kenaepro.pull"
        bl_label = "Get frame"
        bl_description = "Move Blender to KenaePRO's current frame"

        @classmethod
        def poll(cls, context):
            return _client is not None

        def execute(self, context):
            global _applying
            try:
                result = _client.call("get_frame")
            except (ConnectionLost, ApiError) as error:
                _disconnect("lost the connection: %s" % error)
                return {"CANCELLED"}
            review = int(result.get("frame", 0))
            _state._last_review = review
            _applying = True
            try:
                context.scene.frame_set(_state.map.to_blender(review))
            finally:
                _applying = False
            return {"FINISHED"}

    class KENAEPRO_PT_sync(bpy.types.Panel):
        bl_label = "KenaePRO sync"
        bl_idname = "KENAEPRO_PT_sync"
        bl_space_type = "VIEW_3D"
        bl_region_type = "UI"
        bl_category = "KenaePRO"

        def draw(self, context):
            layout = self.layout
            s = _settings(context.scene)
            connected = _client is not None
            row = layout.row()
            row.label(text="Connected" if connected else "Not connected",
                      icon="LINKED" if connected else "UNLINKED")
            if _last_error and not connected:
                layout.label(text=_last_error, icon="ERROR")

            col = layout.column()
            col.enabled = not connected
            col.prop(s, "host")
            col.prop(s, "port")
            if connected:
                layout.operator("kenaepro.disconnect", icon="CANCEL")
            else:
                layout.operator("kenaepro.connect", icon="PLAY")

            box = layout.box()
            box.label(text="Frame mapping")
            box.prop(s, "use_scene_start")
            if not s.use_scene_start:
                box.prop(s, "base_frame")
            box.prop(s, "offset")
            box.prop(s, "multiplier")
            box.prop(s, "mirror_play")
            if connected:
                row = layout.row(align=True)
                row.operator("kenaepro.push")
                row.operator("kenaepro.pull")
                layout.label(text="Changes to the mapping apply on the next Connect.")

    _classes = (
        KenaeProSyncSettings,
        KENAEPRO_OT_connect,
        KENAEPRO_OT_disconnect,
        KENAEPRO_OT_push,
        KENAEPRO_OT_pull,
        KENAEPRO_PT_sync,
    )

    def register():
        for cls in _classes:
            bpy.utils.register_class(cls)
        bpy.types.Scene.kenaepro_sync = bpy.props.PointerProperty(type=KenaeProSyncSettings)
        if _on_frame_change not in bpy.app.handlers.frame_change_post:
            bpy.app.handlers.frame_change_post.append(_on_frame_change)

    def unregister():
        _disconnect()
        if _on_frame_change in bpy.app.handlers.frame_change_post:
            bpy.app.handlers.frame_change_post.remove(_on_frame_change)
        del bpy.types.Scene.kenaepro_sync
        for cls in reversed(_classes):
            bpy.utils.unregister_class(cls)

    if __name__ == "__main__":
        register()
