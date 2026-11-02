"""Tests of the add-on's plain-Python part against a fake Client API server.

Run: python3 -I blender/test_sync_protocol.py   (no Blender needed)
"""
import json
import os
import socket
import sys
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kenaepro_sync as sync  # noqa: E402  (bpy is absent here, only the plain part loads)


class FakeApp:
    """Speaks the app's wire format: one JSON object per line, {"success", "result" | "msg"}."""

    def __init__(self):
        self.server = socket.socket()
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(4)
        self.port = self.server.getsockname()[1]
        self.frame = 0
        self.frame_count = 100
        self.initialized = False
        self.requests = []
        self.drop_next = False
        self._stop = False
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def close(self):
        self._stop = True
        # On Linux close() alone leaves a socket that another thread is accept()ing on still
        # listening; shutdown() wakes the accept and stops new connections. macOS may refuse it.
        try:
            self.server.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.server.close()

    def _serve(self):
        while not self._stop:
            try:
                conn, _ = self.server.accept()
            except OSError:
                return
            threading.Thread(target=self._client, args=(conn,), daemon=True).start()

    def _client(self, conn):
        buffer = b""
        with conn:
            while True:
                try:
                    chunk = conn.recv(4096)
                except OSError:
                    return
                if not chunk:
                    return
                buffer += chunk
                while b"\n" in buffer:
                    line, _, buffer = buffer.partition(b"\n")
                    request = json.loads(line)
                    self.requests.append(request)
                    if self.drop_next:
                        self.drop_next = False
                        return  # closes the connection without answering
                    conn.sendall(json.dumps(self._answer(request)).encode() + b"\n")

    def _answer(self, request):
        cmd = request["cmd"]
        if cmd == "initialize":
            if self.initialized:
                return {"success": False, "msg": "Already initialized"}
            self.initialized = True
            return {"success": True, "result": {"app_name": "KenaePRO"}}
        if cmd == "get_frame":
            return {"success": True, "result": {"frame": self.frame, "frame_count": self.frame_count}}
        if cmd == "set_frame":
            frame = request["frame"]
            if not 0 <= frame < self.frame_count:
                return {"success": False, "msg": "Frame %d is outside 0..%d" % (frame, self.frame_count - 1)}
            self.frame = frame
            return {"success": True, "result": {}}
        return {"success": False, "msg": "Unknown command: %s" % cmd}


class FrameMapTest(unittest.TestCase):
    def test_identity(self):
        m = sync.FrameMap(base=1, offset=0, multiplier=1.0)
        self.assertEqual(m.to_review(1), 0)
        self.assertEqual(m.to_review(25), 24)
        self.assertEqual(m.to_blender(24), 25)

    def test_offset_and_rate(self):
        m = sync.FrameMap(base=1001, offset=10, multiplier=0.5)  # 24 fps review of a 48 fps scene
        self.assertEqual(m.to_review(1001), 10)
        self.assertEqual(m.to_review(1049), 34)
        self.assertEqual(m.to_blender(34), 1049)
        for review in range(0, 200):  # the round trip never drifts
            self.assertEqual(m.to_review(m.to_blender(review)), review)

    def test_bad_multiplier_is_ignored(self):
        self.assertEqual(sync.FrameMap(1, 0, 0).multiplier, 1.0)
        self.assertEqual(sync.FrameMap(1, 0, -3).multiplier, 1.0)


class SyncStateTest(unittest.TestCase):
    def test_push_skips_echo_and_clamps(self):
        state = sync.SyncState(sync.FrameMap(1, 0, 1.0))
        state.frame_count = 10
        self.assertEqual(state.push(4), 3)
        self.assertIsNone(state.push(4))  # nothing changed
        self.assertEqual(state.push(500), 9)  # held inside the clip
        self.assertEqual(state.push(-7), 0)

    def test_poll_does_not_echo_what_was_just_pushed(self):
        state = sync.SyncState(sync.FrameMap(1, 0, 1.0))
        state.frame_count = 100
        self.assertEqual(state.push(11), 10)
        self.assertIsNone(state.poll(11, 10, 100))  # the app now shows what we sent

    def test_poll_moves_blender_once(self):
        state = sync.SyncState(sync.FrameMap(1, 0, 1.0))
        self.assertEqual(state.poll(1, 20, 100), 21)
        self.assertIsNone(state.poll(21, 20, 100))  # already followed
        self.assertEqual(state.frame_count, 100)
        self.assertIsNone(state.poll(21, 20, 100))

    def test_blender_already_there(self):
        state = sync.SyncState(sync.FrameMap(1, 0, 1.0))
        self.assertIsNone(state.poll(21, 20, 100))


class ClientTest(unittest.TestCase):
    def setUp(self):
        self.app = FakeApp()
        self.client = sync.ApiClient("127.0.0.1", self.app.port, timeout=2.0)

    def tearDown(self):
        self.client.close()
        self.app.close()

    def test_connect_initializes_and_talks(self):
        self.client.connect()
        self.assertTrue(self.client.connected)
        self.assertEqual(self.app.requests[0], {"cmd": "initialize", "api_version": sync.API_VERSION})
        self.app.frame = 7
        self.assertEqual(self.client.call("get_frame"), {"frame": 7, "frame_count": 100})
        self.client.call("set_frame", frame=42)
        self.assertEqual(self.app.frame, 42)

    def test_refused_command_is_an_api_error(self):
        self.client.connect()
        with self.assertRaises(sync.ApiError) as raised:
            self.client.call("set_frame", frame=1000)
        self.assertIn("outside", str(raised.exception))
        self.assertTrue(self.client.connected)  # a refusal does not drop the connection

    def test_reconnect_accepts_already_initialized(self):
        self.client.connect()
        self.client.close()
        self.assertFalse(self.client.connected)
        self.client.connect()  # the app answers "Already initialized"; that is fine
        self.assertTrue(self.client.connected)
        self.assertEqual(self.client.call("get_frame")["frame_count"], 100)

    def test_unreachable_port(self):
        port = self.app.port
        self.app.close()
        with self.assertRaises(sync.ConnectionLost):
            sync.ApiClient("127.0.0.1", port, timeout=0.5).connect()

    def test_dropped_connection_is_reported_and_closes(self):
        self.client.connect()
        self.app.drop_next = True
        with self.assertRaises(sync.ConnectionLost):
            self.client.call("get_frame")
        self.assertFalse(self.client.connected)
        with self.assertRaises(sync.ConnectionLost):
            self.client.call("get_frame")  # no half-open socket is reused

    def test_two_sides_converge_without_ping_pong(self):
        """A short simulation of what the timer and the frame handler do together."""
        self.client.connect()
        state = sync.SyncState(sync.FrameMap(1, 0, 1.0))
        blender = 1

        def timer():
            nonlocal blender
            result = self.client.call("get_frame")
            target = state.poll(blender, result["frame"], result["frame_count"])
            if target is not None:
                blender = target  # frame_set; its handler is suppressed while applying

        def handler():
            review = state.push(blender)
            if review is not None:
                self.client.call("set_frame", frame=review)

        timer()  # connect: the app's frame wins
        self.assertEqual(blender, 1)
        blender = 31  # the artist scrubs in Blender
        handler()
        self.assertEqual(self.app.frame, 30)
        sent = len(self.app.requests)
        timer()  # nothing to follow: the app shows what we sent
        timer()
        self.assertEqual(blender, 31)
        self.assertEqual([r["cmd"] for r in self.app.requests[sent:]], ["get_frame", "get_frame"])

        self.app.frame = 55  # the reviewer scrubs in the app
        timer()
        self.assertEqual(blender, 56)
        handler()  # frame_change_post fires after frame_set; the state knows this frame
        self.assertEqual(self.app.requests[-1]["cmd"], "get_frame")  # nothing was sent back


if __name__ == "__main__":
    unittest.main()
