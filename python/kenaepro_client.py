"""Python client for the KenaePRO Client API.

Drive a running KenaePRO from any Python 3 (a pipeline script, Maya, a render farm hook). Start the
app with its Client API on (File > Preferences > Client API, or `kenaepro --api-port 4545`), then:

    from kenaepro_client import KenaeProClient

    with KenaeProClient(port=4545) as kp:
        source = kp.import_file("/shots/sh010/anim_v003.mp4", activate=True)
        kp.set_range(0, 47)
        kp.create_bookmarks(frames=[0, 12, 24])
        kp.add_notes({12: "arm pops here"})
        kp.set_playing(True)

The same calls work from a shell for a quick look:

    python3 kenaepro_client.py get_frame
    python3 kenaepro_client.py set_frame frame=10

Frames are 0-based. Bookmark and note frames are frames of the source on screen; playback frames
(get_frame, set_frame, ranges) are frames of what the viewer shows, which differ only for timelines.

Plain Python, no dependencies. One JSON object per line in each direction over TCP on 127.0.0.1.
"""

import json
import socket

API_VERSION = 1
DEFAULT_PORT = 4545
DEFAULT_TIMEOUT = 5.0  # seconds; import and open can take a moment on big files


class ApiError(Exception):
    """KenaePRO answered, but refused the command (the message says why)."""


class ConnectionLost(Exception):
    """The socket failed or timed out; connect again."""


class KenaeProClient:
    """One connection to a running KenaePRO."""

    def __init__(self, host="127.0.0.1", port=DEFAULT_PORT, timeout=DEFAULT_TIMEOUT):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.info = {}  # what `initialize` answered: app_name, app_dir, user_dir, temp_dir
        self._sock = None
        self._buffer = b""

    # --- connection --------------------------------------------------------------------------

    @property
    def connected(self):
        return self._sock is not None

    def connect(self):
        self.close()
        try:
            sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        except OSError as error:
            raise ConnectionLost("cannot reach KenaePRO at %s:%s (%s)" % (self.host, self.port, error))
        sock.settimeout(self.timeout)
        self._sock = sock
        self._buffer = b""
        try:
            self.info = self.call("initialize", api_version=API_VERSION)
        except ApiError as error:
            # The app keeps one initialised state per run, so a second connection is told so. It
            # still works.
            if "Already initialized" not in str(error):
                self.close()
                raise
        return self

    def close(self):
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
        self._sock = None
        self._buffer = b""

    def __enter__(self):
        if not self.connected:
            self.connect()
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def call(self, cmd, **params):
        """Sends one command and returns its `result` dict. Parameters that are None are left out."""
        if self._sock is None:
            raise ConnectionLost("not connected")
        request = {key: value for key, value in params.items() if value is not None}
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
            raise ConnectionLost("KenaePRO sent something that is not JSON")
        if not response.get("success"):
            raise ApiError(str(response.get("msg", "command failed")))
        return response.get("result", {})

    def _read_line(self):
        while b"\n" not in self._buffer:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise OSError("KenaePRO closed the connection")
            self._buffer += chunk
        line, _, self._buffer = self._buffer.partition(b"\n")
        return line

    # --- application and project -------------------------------------------------------------

    def echo(self, **values):
        return self.call("echo", **values)

    def get_config(self):
        return self.call("get_config")

    def get_project_path(self):
        return self.call("get_project_path").get("project_path", "")

    def new_project(self):
        self.call("new_project")

    def open_project(self, project_path):
        return self.call("open_project", project_path=project_path).get("project_path", "")

    def save_project(self, project_path=None):
        return self.call("save_project", project_path=project_path).get("project_path", "")

    def import_file(self, file_path, activate=False, sequence=False):
        """Adds a video or image as a source and returns its id. `sequence=True` imports every
        numbered image next to `file_path` as one image sequence."""
        return self.call("import_file", file_path=file_path, activate=activate,
                         sequence=sequence).get("source_id", "")

    def get_folders(self):
        return self.call("get_folders").get("folders", [])

    def get_sources(self):
        return self.call("get_sources").get("sources", [])

    def get_timelines(self):
        return self.call("get_timelines").get("timelines", [])

    def add_folder(self, name, parent_id=None):
        return self.call("add_folder", name=name, parent_id=parent_id).get("id", "")

    def add_timeline(self, name, parent_id=None):
        return self.call("add_timeline", name=name, parent_id=parent_id).get("id", "")

    def remove(self, object_kind, item_id, force=False):
        """`object_kind` is "folder", "source" or "timeline"."""
        self.call("remove", object=object_kind, id=item_id, force=force)

    # --- playback ----------------------------------------------------------------------------

    def get_frame(self):
        """(frame, frame_count) of what the active viewer shows."""
        result = self.call("get_frame")
        return result.get("frame", 0), result.get("frame_count", 0)

    def set_frame(self, frame):
        self.call("set_frame", frame=int(frame))

    def set_playing(self, playing, forwards=True):
        self.call("set_playing", playing=bool(playing), play_forwards=bool(forwards))

    def play(self):
        self.set_playing(True)

    def pause(self):
        self.set_playing(False)

    def get_range(self):
        result = self.call("get_range")
        return result.get("range_start", 0), result.get("range_end", 0)

    def set_range(self, start_frame, end_frame):
        self.call("set_range", start_frame=int(start_frame), end_frame=int(end_frame))

    # --- timelines and viewers ---------------------------------------------------------------

    def get_default_timeline(self):
        return self.call("get_default_timeline").get("id", "")

    def set_default_timeline(self, timeline_id):
        self.call("set_default_timeline", id=timeline_id)

    def get_timeline_elements(self, timeline_id):
        return self.call("get_timeline_elements", timeline_id=timeline_id).get("elements", [])

    def insert_element_in_timeline(self, timeline_id, source_id, index=-1):
        self.call("insert_element_in_timeline", timeline_id=timeline_id, source_id=source_id, index=index)

    def remove_element_from_timeline(self, timeline_id, index):
        self.call("remove_element_from_timeline", timeline_id=timeline_id, index=index)

    def get_active_in_viewer(self, viewer=None):
        """{"kind": "source" | "timeline" | "", "id": ...} of a viewer (0, 1, or None: the active one)."""
        return self.call("get_active_in_viewer", viewer=viewer)

    def set_active_in_viewer(self, item_id, viewer=None):
        """Shows a source or a timeline, in viewer 0 or 1 (None: the active one)."""
        self.call("set_active_in_viewer", id=item_id, viewer=viewer)

    def set_viewer_layout(self, layout):
        """"single", "horizontal" (side by side) or "vertical" (stacked)."""
        self.call("set_viewer_layout", layout=layout)

    # --- the source on screen: bookmarks, notes, timecode --------------------------------------

    def get_bookmarks(self):
        """{"source_id": ..., "frames": [...], "ranges": [[start, end], ...]}"""
        return self.call("get_bookmarks")

    def create_bookmarks(self, frames=(), ranges=()):
        self.call("create_bookmarks", frames=[int(f) for f in frames],
                  ranges=[[int(a), int(b)] for a, b in ranges])

    def clear_bookmarks(self):
        self.call("clear_bookmarks")

    def add_notes(self, notes):
        """`notes` is {frame: text} or a list of (frame, text). An empty text removes a note."""
        items = notes.items() if isinstance(notes, dict) else notes
        self.call("add_notes", notes=[{"frame": int(frame), "text": str(text)} for frame, text in items])

    def get_tc_frame_offset_override(self):
        return self.call("get_tc_frame_offset_override")

    def set_tc_frame_offset_override(self, offset, enabled=True):
        self.call("set_tc_frame_offset_override", offset=int(offset), enabled=bool(enabled))


# --- keeping another application's frame in step (used by the Maya script) --------------------

class FrameMap:
    """Converts between a host's frames (Maya, Blender) and review frames (0-based)."""

    def __init__(self, base=1, offset=0, multiplier=1.0):
        self.base = int(base)
        self.offset = int(offset)
        self.multiplier = float(multiplier) if float(multiplier) > 0 else 1.0

    def to_review(self, host_frame):
        return int(round((host_frame - self.base) * self.multiplier)) + self.offset

    def to_host(self, review_frame):
        return int(round((review_frame - self.offset) / self.multiplier)) + self.base


class SyncState:
    """Two-way frame sync, free of any host so it can be tested.

    `push(host_frame)` when the host's frame changed, `poll(host_frame, review_frame, count)` on a
    timer. Each returns the frame the other side should move to, or None. A frame one side just got
    from the other is never sent back, so the two never ping-pong.
    """

    def __init__(self, frame_map):
        self.map = frame_map
        self.frame_count = None
        self._last_review = None

    def push(self, host_frame):
        review = self.map.to_review(host_frame)
        if self.frame_count:
            review = max(0, min(self.frame_count - 1, review))
        if review == self._last_review:
            return None
        self._last_review = review
        return review

    def poll(self, host_frame, review_frame, frame_count):
        self.frame_count = frame_count
        if review_frame == self._last_review:
            return None
        self._last_review = review_frame
        target = self.map.to_host(review_frame)
        return None if target == host_frame else target


def _main(argv):
    """python3 kenaepro_client.py [--port N] command [key=value ...]  (values are JSON or text)"""
    import sys

    port = DEFAULT_PORT
    if len(argv) >= 2 and argv[0] == "--port":
        port = int(argv[1])
        argv = argv[2:]
    if not argv:
        print(_main.__doc__)
        return 2
    params = {}
    for item in argv[1:]:
        key, _, value = item.partition("=")
        try:
            params[key] = json.loads(value)
        except ValueError:
            params[key] = value
    try:
        with KenaeProClient(port=port) as client:
            print(json.dumps(client.call(argv[0], **params), indent=2))
    except (ApiError, ConnectionLost) as error:
        print("KenaePRO: %s" % error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    import sys

    sys.exit(_main(sys.argv[1:]))
