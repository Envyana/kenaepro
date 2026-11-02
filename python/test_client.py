"""Tests for kenaepro_client.py against a fake KenaePRO on a local socket.

Run: python3 -I python/test_client.py
"""

import json
import os
import socket
import sys
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import kenaepro_client as kp  # noqa: E402


class FakeApp:
    """Answers one connection: records each request and replies from `handler`."""

    def __init__(self, handler):
        self.handler = handler
        self.requests = []
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(1)
        self.port = self.server.getsockname()[1]
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        conn, _ = self.server.accept()
        buffer = b""
        with conn:
            while True:
                chunk = conn.recv(65536)
                if not chunk:
                    return
                buffer += chunk
                while b"\n" in buffer:
                    line, _, buffer = buffer.partition(b"\n")
                    request = json.loads(line.decode("utf-8"))
                    self.requests.append(request)
                    conn.sendall(json.dumps(self.handler(request)).encode("utf-8") + b"\n")

    def close(self):
        self.server.close()


def ok(result=None):
    return {"success": True, "result": result or {}}


class ClientTests(unittest.TestCase):
    def test_initializes_and_sends_parameters(self):
        def handler(request):
            if request["cmd"] == "initialize":
                return ok({"app_name": "KenaePRO"})
            if request["cmd"] == "import_file":
                return ok({"source_id": "src1"})
            if request["cmd"] == "get_frame":
                return ok({"frame": 7, "frame_count": 96})
            return ok()

        app = FakeApp(handler)
        try:
            with kp.KenaeProClient(port=app.port, timeout=2) as client:
                self.assertEqual(client.info["app_name"], "KenaePRO")
                self.assertEqual(client.import_file("/a/b.0001.png", activate=True, sequence=True), "src1")
                self.assertEqual(client.get_frame(), (7, 96))
                client.add_notes({12: "arm pops"})
                client.create_bookmarks(frames=[1, 2], ranges=[(5, 9)])
                client.get_active_in_viewer()  # viewer None is left out of the request
        finally:
            app.close()

        sent = {r["cmd"]: r for r in app.requests}
        self.assertEqual(sent["initialize"]["api_version"], kp.API_VERSION)
        self.assertEqual(sent["import_file"], {"cmd": "import_file", "file_path": "/a/b.0001.png",
                                               "activate": True, "sequence": True})
        self.assertEqual(sent["add_notes"]["notes"], [{"frame": 12, "text": "arm pops"}])
        self.assertEqual(sent["create_bookmarks"]["ranges"], [[5, 9]])
        self.assertNotIn("viewer", sent["get_active_in_viewer"])

    def test_refusal_raises_api_error_and_second_initialize_is_fine(self):
        def handler(request):
            if request["cmd"] == "initialize":
                return {"success": False, "msg": "Already initialized"}
            return {"success": False, "msg": "No source is shown"}

        app = FakeApp(handler)
        try:
            client = kp.KenaeProClient(port=app.port, timeout=2).connect()
            with self.assertRaises(kp.ApiError) as caught:
                client.get_bookmarks()
            self.assertIn("No source", str(caught.exception))
            self.assertTrue(client.connected)  # a refusal keeps the connection
            client.close()
        finally:
            app.close()

    def test_unreachable_app(self):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
        sock.close()  # nothing listens here now
        with self.assertRaises(kp.ConnectionLost):
            kp.KenaeProClient(port=port, timeout=0.5).connect()

    def test_frame_map_and_sync(self):
        frame_map = kp.FrameMap(base=1001, offset=0, multiplier=0.5)
        self.assertEqual(frame_map.to_review(1001), 0)
        self.assertEqual(frame_map.to_review(1011), 5)
        self.assertEqual(frame_map.to_host(5), 1011)

        state = kp.SyncState(kp.FrameMap(base=1, offset=0))
        self.assertEqual(state.push(10), 9)
        self.assertIsNone(state.push(10))  # nothing new
        self.assertIsNone(state.poll(10, 9, 50))  # the app is where Maya sent it: no echo back
        self.assertEqual(state.poll(10, 20, 50), 21)
        self.assertEqual(state.push(500), 49)  # held to the clip once its length is known


if __name__ == "__main__":
    result = unittest.main(exit=False, verbosity=1).result
    sys.exit(0 if result.wasSuccessful() else 1)
