"""Exercise the distributable adapter using temporary databases and loopback HTTP."""
from contextlib import closing
import importlib.util
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, quote, urlparse
from unittest.mock import patch
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "plugins" / "lx-music"
ADAPTER = PACKAGE / "adapter.py"
OPERATIONS = {"status", "launch", "play", "pause", "skip_next", "skip_prev",
              "lyric", "set_volume", "search_play", "list_playlists",
              "playlist_songs", "play_playlist"}


class Backend(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.counts = Counter()
        self.mode = "normal"
        self.state = {"status": "paused", "name": "晴天", "singer": "周杰伦", "volume": 50}
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join(timeout=2)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        request = urlparse(self.path)
        backend = self.server
        backend.counts[request.path] += 1
        write = request.path in {"/play", "/pause", "/volume", "/skip-next", "/skip-prev"}
        if write and backend.mode != "mismatch":
            if request.path == "/play":
                backend.state["status"] = "playing"
            if request.path == "/pause":
                backend.state["status"] = "paused"
            if request.path == "/volume":
                backend.state["volume"] = int(parse_qs(request.query)["volume"][0])
        if write and backend.mode == "disconnect":
            self.connection.shutdown(socket.SHUT_RDWR)
            self.connection.close()
            return
        if backend.mode == "timeout":
            time.sleep(0.3)
        if backend.mode == "slow_headers":
            try:
                for character in b"HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n\r\n{}":
                    self.connection.sendall(bytes([character]))
                    time.sleep(0.04)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        code = 500 if backend.mode == "http_error" else 200
        if backend.mode == "wait_for_launch" and not backend.launch_record.exists():
            code = 503
        if backend.mode == "redirect":
            code = 302
        body = backend.state if request.path == "/status" else {"ok": True}
        if request.path == "/lyric":
            body = {"lyric": "[00:01]窗外的麻雀"}
        encoded = json.dumps(body, ensure_ascii=False).encode("utf-8")
        content_type = "application/json"
        if backend.mode == "text_lyric" and request.path == "/lyric":
            content_type = "text/plain; charset=utf-8"
            encoded = "[00:01]窗外的麻雀\n[00:02]在电线杆上多嘴".encode("utf-8")
        if backend.mode == "text_ack" and write:
            content_type = "text/plain"
            encoded = b"ok"
        if backend.mode == "empty_ack" and write:
            encoded = b""
        if backend.mode == "false_ack" and write:
            encoded = b"false"
        if backend.mode == "bad_json":
            encoded = b"{bad json"
        if backend.mode == "huge":
            encoded = b"x" * (600 * 1024)
        if backend.mode == "invalid_status" and request.path == "/status":
            encoded = b'{"arbitrary": true}'
        try:
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(encoded) + (10 if backend.mode == "truncated" else 0)))
            if code == 302:
                self.send_header("Location", "/play")
            self.end_headers()
            self.wfile.write(encoded)
        except (BrokenPipeError, ConnectionResetError):
            pass


class LxMusicAdapterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="lx adapter ")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.database = self.directory / "music 数据.db"
        with closing(sqlite3.connect(self.database)) as connection, connection:
            connection.executescript('''
                CREATE TABLE my_list(id TEXT PRIMARY KEY, name TEXT, source TEXT, sourceListId TEXT, position INTEGER);
                CREATE TABLE my_list_music_info(id TEXT PRIMARY KEY, name TEXT, singer TEXT);
                CREATE TABLE my_list_music_info_order(listId TEXT, musicInfoId TEXT, "order" INTEGER);
            ''')
            connection.executemany("INSERT INTO my_list VALUES(?,?,?,?,?)", [
                ("cloud", "云端歌单", "wy", "https://music.163.com/playlist?id=12345", 3),
                ("empty", "空歌单", "", "", 2),
                ("quote' OR 1=1 --", "特殊标识", "tx", "42", 1),
            ])
            connection.executemany("INSERT INTO my_list_music_info VALUES(?,?,?)", [
                ("a", "晴天", "周杰伦"), ("b", "稻香", "周杰伦")])
            connection.executemany('INSERT INTO my_list_music_info_order VALUES(?,?,?)', [
                ("cloud", "a", 2), ("cloud", "b", 1), ("quote' OR 1=1 --", "a", 1)])
        self.settings_path = self.directory / "connection.json"
        self.settings = {"database": str(self.database), "apiUrl": "http://127.0.0.1:1",
                         "requestTimeoutMs": 100, "verifyTimeoutMs": 150}
        self.write_settings()

    def write_settings(self):
        self.settings_path.write_text(json.dumps(self.settings), encoding="utf-8")

    def start_backend(self):
        backend = Backend()
        self.addCleanup(backend.close)
        self.settings["apiUrl"] = "http://127.0.0.1:%d" % backend.server_port
        self.write_settings()
        return backend

    def invoke(self, operation=None, arguments=None, raw=None, diagnose=False, environment=None):
        command = [sys.executable, str(ADAPTER), "--settings", str(self.settings_path)]
        command += ["--diagnose"] if diagnose else ["--operation", operation]
        if raw is None:
            raw = json.dumps(arguments or {}, ensure_ascii=False).encode("utf-8")
        completed = subprocess.run(command, input=raw, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, timeout=5, env=environment)
        self.assertEqual(completed.returncode, 0, completed.stderr.decode("utf-8", "replace"))
        self.assertEqual(completed.stderr, b"")
        result = json.loads(completed.stdout.decode("utf-8"))
        self.assertIn(result["state"], {"completed", "failed", "unknown"})
        self.assertIsInstance(result["data"], dict)
        return result

    def controlled_executable(self):
        if os.name == "nt":
            self.skipTest("POSIX controlled executable; Windows process options tested separately")
        record = self.directory / "argv.json"
        executable = self.directory / '假播放器 "$(no-shell)"'
        executable.write_text("#!%s\nimport json, pathlib, sys\npathlib.Path(%r).write_text(json.dumps(sys.argv[1:]), encoding='utf-8')\n" %
                              (sys.executable, str(record)), encoding="utf-8")
        executable.chmod(0o700)
        self.settings["executable"] = str(executable)
        self.write_settings()
        return record

    def wait_record(self, record):
        deadline = time.monotonic() + 2
        while not record.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(record.exists())
        return json.loads(record.read_text(encoding="utf-8"))

    def test_sqlite_ordered_queries_read_only(self):
        before = self.database.read_bytes()
        result = self.invoke("list_playlists")
        self.assertEqual(result["state"], "completed")
        self.assertEqual([row["id"] for row in result["data"]["items"]],
                         ["quote' OR 1=1 --", "empty", "cloud"])
        self.assertEqual(result["data"]["items"][2]["playUrl"], "lxmusic://songlist/play/wy/12345")
        songs = self.invoke("playlist_songs", {"playlist_id": "cloud"})
        self.assertEqual([row["name"] for row in songs["data"]["items"]], ["稻香", "晴天"])
        self.assertEqual(self.database.read_bytes(), before)

    def test_missing_database_no_create(self):
        missing = self.directory / "does-not-exist.db"
        self.settings["database"] = str(missing)
        self.write_settings()
        self.assertEqual(self.invoke("list_playlists")["state"], "failed")
        self.assertFalse(missing.exists())

    def test_unknown_and_empty_playlist(self):
        result = self.invoke("playlist_songs", {"playlist_id": "empty"})
        self.assertEqual(result["state"], "completed")
        self.assertEqual(result["data"]["items"], [])
        missing = self.invoke("playlist_songs", {"playlist_id": "missing"})
        self.assertEqual(missing["state"], "failed")
        self.assertEqual(missing["data"]["error"]["code"], "PLAYLIST_NOT_FOUND")

    def test_parameterized_id(self):
        result = self.invoke("playlist_songs", {"playlist_id": "quote' OR 1=1 --"})
        self.assertEqual(result["state"], "completed")
        self.assertEqual(result["data"]["items"], [{"name": "晴天", "singer": "周杰伦"}])

    def test_current_lx_schema_scopes_duplicate_song_ids_to_playlist(self):
        with closing(sqlite3.connect(self.database)) as connection, connection:
            connection.execute('ALTER TABLE my_list_music_info RENAME TO old_info')
            connection.execute('CREATE TABLE my_list_music_info '
                               '(id TEXT, name TEXT, singer TEXT, listId TEXT, UNIQUE(id,listId))')
            connection.execute('INSERT INTO my_list_music_info SELECT id,name,singer,? FROM old_info', ('cloud',))
            connection.execute('INSERT INTO my_list_music_info VALUES(?,?,?,?)',
                               ('a', '另一个歌单版本', '其他歌手', 'other'))
        result = self.invoke('playlist_songs', {'playlist_id': 'cloud'})
        self.assertEqual(result['state'], 'completed')
        self.assertEqual(result['data']['items'], [
            {'name': '稻香', 'singer': '周杰伦'}, {'name': '晴天', 'singer': '周杰伦'}])

    def test_playlist_scheme_preserves_id_path_and_escapes_special_characters(self):
        with closing(sqlite3.connect(self.database)) as connection, connection:
            connection.execute("INSERT INTO my_list VALUES(?,?,?,?,?)",
                               ("source-path", "路径歌单", "kg", 'id/123 空?#%"', 4))
            connection.execute("INSERT INTO my_list VALUES(?,?,?,?,?)",
                               ("normalized-path", "URL 来源", "kg", "https://example.invalid/?playlistId=id%2F42%3Findex%3D9", 5))
        result = self.invoke("list_playlists")
        self.assertEqual(result["state"], "completed")
        urls = {item["id"]: item["playUrl"] for item in result["data"]["items"]}
        self.assertEqual(urls["source-path"], "lxmusic://songlist/play/kg/id/123%20%E7%A9%BA%3F%23%25%22")
        self.assertEqual(urls["normalized-path"], "lxmusic://songlist/play/kg/id/42%3Findex%3D9")
        self.assertEqual(urlparse(urls["source-path"]).query, "")
        self.assertEqual(urlparse(urls["source-path"]).fragment, "")

    def test_http_status_and_lyric_no_launch(self):
        backend = self.start_backend()
        record = self.controlled_executable()
        self.assertEqual(self.invoke("status")["data"]["status"], backend.state)
        self.assertEqual(self.invoke("lyric")["data"]["lyric"], "[00:01]窗外的麻雀")
        backend.mode = "text_lyric"
        self.assertEqual(self.invoke("lyric")["data"]["lyric"], "[00:01]窗外的麻雀\n[00:02]在电线杆上多嘴")
        self.settings["apiUrl"] = "http://127.0.0.1:1"
        self.write_settings()
        self.assertEqual(self.invoke("status")["state"], "failed")
        self.assertEqual(self.invoke("lyric")["state"], "failed")
        self.assertFalse(record.exists())

    def test_http_invalid_timeout(self):
        backend = self.start_backend()
        for mode in ("bad_json", "http_error", "timeout", "huge", "invalid_status", "redirect"):
            with self.subTest(mode=mode):
                backend.mode = mode
                self.assertEqual(self.invoke("status")["state"], "failed")
        self.assertEqual(backend.counts["/play"], 0)

    def test_http_incomplete_response_and_slow_headers(self):
        backend = self.start_backend()
        backend.mode = "truncated"
        self.assertEqual(self.invoke("status")["state"], "failed")
        self.assertEqual(self.invoke("play")["state"], "unknown")
        self.assertEqual(backend.counts["/play"], 1)
        backend.mode = "slow_headers"
        started = time.monotonic()
        self.assertEqual(self.invoke("status")["state"], "failed")
        self.assertLess(time.monotonic() - started, 1.5)

    def test_play_pause_volume_verified(self):
        backend = self.start_backend()
        for operation, arguments, key, expected, path in [
            ("play", {}, "status", "playing", "/play"),
            ("pause", {}, "status", "paused", "/pause"),
            ("set_volume", {"volume": 73}, "volume", 73, "/volume"),
        ]:
            with self.subTest(operation=operation):
                result = self.invoke(operation, arguments)
                self.assertEqual(result["state"], "completed")
                self.assertEqual(result["data"]["status"][key], expected)
                self.assertEqual(backend.counts[path], 1)

    def test_write_disconnect_once_unknown(self):
        backend = self.start_backend()
        backend.mode = "disconnect"
        self.assertEqual(self.invoke("play")["state"], "unknown")
        self.assertEqual(backend.counts["/play"], 1)
        backend.mode = "normal"
        self.assertEqual(self.invoke("status")["data"]["status"]["status"], "playing")
        self.assertEqual(backend.counts["/play"], 1)
        backend.mode = "mismatch"
        result = self.invoke("pause")
        self.assertEqual(result["state"], "unknown")
        self.assertEqual(result["data"]["status"]["status"], "playing")
        self.assertEqual(backend.counts["/pause"], 1)

    def test_explicit_false_ack_is_unknown_and_never_repeated(self):
        backend = self.start_backend()
        backend.mode = "false_ack"
        result = self.invoke("skip_next")
        self.assertEqual(result["state"], "unknown")
        self.assertEqual(result["data"]["error"]["code"], "API_REJECTED")
        self.assertEqual(backend.counts["/skip-next"], 1)

    def test_skip_acknowledged_once(self):
        backend = self.start_backend()
        before = dict(backend.state)
        for operation, path in (("skip_next", "/skip-next"), ("skip_prev", "/skip-prev")):
            result = self.invoke(operation)
            self.assertEqual(result["state"], "completed")
            self.assertEqual(result["data"]["completion_basis"], "api_acknowledged")
            self.assertEqual(backend.counts[path], 1)
        self.assertEqual(backend.state, before)
        backend.mode = "text_ack"
        self.assertEqual(self.invoke("skip_next")["data"]["acknowledgement"], "ok")
        self.assertEqual(backend.counts["/skip-next"], 2)
        backend.mode = "empty_ack"
        self.assertEqual(self.invoke("skip_prev")["state"], "completed")
        self.assertEqual(backend.counts["/skip-prev"], 2)

    def test_launch_and_scheme_argv(self):
        backend = self.start_backend()
        record = self.controlled_executable()
        self.assertEqual(self.invoke("launch")["state"], "completed")
        self.assertFalse(record.exists(), "Already-ready launch must not create a second player")
        backend.state = {"status": "playing", "name": '晴天 "$(touch pwn)"', "singer": "歌 手", "volume": 50}
        result = self.invoke("search_play", {"song": backend.state["name"], "artist": "歌 手"})
        self.assertEqual(result["state"], "completed")
        keyword = backend.state["name"] + "-歌 手"
        self.assertEqual(self.wait_record(record), ["lxmusic://music/searchPlay/" + quote(keyword, safe="")])
        record.unlink()
        result = self.invoke("play_playlist", {"playlist_id": "cloud"})
        self.assertEqual(result["state"], "completed")
        self.assertEqual(result["data"]["completion_basis"], "playing_observed")
        self.assertEqual(self.wait_record(record), ["lxmusic://songlist/play/wy/12345"])
        record.unlink()
        self.settings["apiUrl"] = "http://127.0.0.1:1"
        self.write_settings()
        self.assertEqual(self.invoke("launch")["state"], "unknown")
        self.assertEqual(self.wait_record(record), [])

    def test_launch_starts_once_and_verifies_api(self):
        backend = self.start_backend()
        record = self.controlled_executable()
        # macOS may inspect a newly created executable before starting it.
        self.settings["verifyTimeoutMs"] = 1500
        self.write_settings()
        backend.mode = "wait_for_launch"
        backend.launch_record = record
        result = self.invoke("launch")
        self.assertEqual(result["state"], "completed")
        self.assertTrue(result["data"]["process_started"])
        self.assertEqual(result["data"]["completion_basis"], "api_ready")
        self.assertEqual(self.wait_record(record), [])

    def test_windows_detached_argument_list(self):
        spec = importlib.util.spec_from_file_location("lx_adapter_under_test", ADAPTER)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        executable = self.directory / 'Player spaced.exe'
        executable.write_bytes(b"controlled placeholder")
        self.settings["executable"] = str(executable)
        called = []
        fake = SimpleNamespace(DEVNULL=-3, DETACHED_PROCESS=8, CREATE_NEW_PROCESS_GROUP=512,
                               STARTF_USESHOWWINDOW=1, SW_HIDE=0,
                               STARTUPINFO=lambda: SimpleNamespace(dwFlags=0),
                               Popen=lambda args, **kwargs: called.append((args, kwargs)) or SimpleNamespace(poll=lambda: None))
        adapter = module.MusicAdapter(module.validate_settings(self.settings))
        argument = 'lxmusic://music/searchPlay/%E6%99%B4%E5%A4%A9%20%22%24%28x%29%22'
        with patch.object(module, "os", SimpleNamespace(name="nt")), patch.object(module, "subprocess", fake):
            adapter.start([argument])
        self.assertTrue(adapter.dispatched)
        self.assertEqual(called[0][0], [str(executable), argument])
        self.assertEqual(called[0][1]["creationflags"], 520)
        self.assertEqual(called[0][1]["cwd"], str(executable.parent))
        self.assertEqual(called[0][1]["startupinfo"].wShowWindow, 0)
        self.assertNotIn("shell", called[0][1])

    def test_windows_batch_file_rejected_before_start(self):
        spec = importlib.util.spec_from_file_location("lx_adapter_batch_test", ADAPTER)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        executable = self.directory / "player.cmd"
        executable.write_text("rem never run", encoding="utf-8")
        self.settings["executable"] = str(executable)
        adapter = module.MusicAdapter(module.validate_settings(self.settings))
        with patch.object(module, "os", SimpleNamespace(name="nt")):
            with self.assertRaises(module.AdapterError) as caught:
                adapter.start(["lxmusic://music/searchPlay/test"])
            self.assertFalse(adapter.diagnose()["data"]["executable"]["available"])
        self.assertEqual(caught.exception.code, "EXECUTABLE_UNAVAILABLE")
        self.assertFalse(adapter.dispatched)

    def test_search_mismatch_unknown(self):
        backend = self.start_backend()
        backend.state["status"] = "playing"
        record = self.controlled_executable()
        result = self.invoke("search_play", {"song": "完全不同的歌"})
        self.assertEqual(result["state"], "unknown")
        self.assertEqual(result["data"]["status"]["name"], "晴天")
        self.assertEqual(len(self.wait_record(record)), 1)

    def test_unplayable_playlist_no_dispatch(self):
        backend = self.start_backend()
        record = self.controlled_executable()
        self.assertEqual(self.invoke("play_playlist", {"playlist_id": "empty"})["state"], "failed")
        self.assertEqual(self.invoke("play_playlist", {"playlist_id": "missing"})["state"], "failed")
        self.assertFalse(record.exists())
        self.assertEqual(sum(backend.counts.values()), 0)

    def test_diagnose_read_only(self):
        self.start_backend()
        record = self.controlled_executable()
        before = self.database.read_bytes()
        result = self.invoke(diagnose=True)
        self.assertEqual(result["state"], "completed")
        self.assertEqual(result["data"]["availability"], "ready")
        self.settings["database"] = str(self.directory / "missing.db")
        self.settings["apiUrl"] = "http://127.0.0.1:1"
        self.write_settings()
        missing = self.invoke(diagnose=True)
        self.assertEqual(missing["data"]["availability"], "partial")
        self.assertFalse(Path(self.settings["database"]).exists())
        self.assertFalse(record.exists())
        self.assertEqual(self.database.read_bytes(), before)
        del self.settings["executable"]
        self.write_settings()
        self.assertEqual(self.invoke(diagnose=True)["data"]["availability"], "unavailable")
        self.settings["database"] = "relative.db"
        self.write_settings()
        invalid = self.invoke(diagnose=True)
        self.assertEqual(invalid["state"], "failed")
        self.assertFalse(invalid["data"]["configuration"]["valid"])
        self.assertIn("python", invalid["data"])
        self.assertIn("platform", invalid["data"])

    def test_database_schema_lock_and_bounded_list(self):
        with closing(sqlite3.connect(self.database)) as connection, connection:
            connection.executemany("INSERT INTO my_list VALUES(?,?,?,?,?)",
                                   [("large-%d" % index, "歌单", "wy", str(index), 10 + index) for index in range(1100)])
        result = self.invoke("list_playlists")
        self.assertEqual(result["state"], "completed")
        self.assertEqual(len(result["data"]["items"]), 1000)
        self.assertTrue(result["data"]["truncated"])
        lock = sqlite3.connect(self.database)
        try:
            lock.execute("BEGIN EXCLUSIVE")
            self.assertEqual(self.invoke("list_playlists")["state"], "failed")
        finally:
            lock.rollback()
            lock.close()
        with closing(sqlite3.connect(self.database)) as connection, connection:
            connection.execute("DROP TABLE my_list")
        self.assertEqual(self.invoke("list_playlists")["state"], "failed")

    def test_proxy_ignored_and_package_self_contained(self):
        self.start_backend()
        environment = dict(os.environ, HTTP_PROXY="http://127.0.0.1:1", http_proxy="http://127.0.0.1:1", NO_PROXY="", no_proxy="")
        self.assertEqual(self.invoke("status", environment=environment)["state"], "completed")
        copied = self.directory / "adapter_copy.py"
        copied.write_bytes(ADAPTER.read_bytes())
        completed = subprocess.run([sys.executable, str(copied), "--settings", str(self.settings_path),
                                    "--operation", "list_playlists"], input=b"{}", cwd=self.directory,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        self.assertEqual(completed.returncode, 0)
        self.assertEqual(json.loads(completed.stdout)["state"], "completed")

    def test_input_bounds_and_json(self):
        backend = self.start_backend()
        for operation, arguments in [
            ("set_volume", {"volume": -1}), ("set_volume", {"volume": 101}),
            ("set_volume", {"volume": True}), ("set_volume", {"volume": "50"}),
            ("set_volume", {"volume": 1.5}), ("search_play", {"song": " "}),
            ("search_play", {"song": "a" * 257}), ("search_play", {"song": "x", "artist": 3}),
            ("playlist_songs", {"playlist_id": "a" * 513}), ("play", {"apiUrl": "http://evil"}),
            ("launch", {"executable": "bad"}), ("unknown", {}),
        ]:
            with self.subTest(operation=operation, arguments=arguments):
                self.assertEqual(self.invoke(operation, arguments)["state"], "failed")
        for raw in (b"{", b"[]", b"null", b'{"volume": NaN}', b"x" * (70 * 1024), b"\xff"):
            self.assertEqual(self.invoke("set_volume", raw=raw)["state"], "failed")
        self.assertEqual(sum(backend.counts.values()), 0)
        for key, value in [("apiUrl", "http://example.com"), ("apiUrl", "http://user@127.0.0.1"),
                           ("apiUrl", "http://127.0.0.1/?a=b"), ("database", "relative.db"),
                           ("requestTimeoutMs", True), ("verifyTimeoutMs", 15001)]:
            original = self.settings.get(key)
            self.settings[key] = value
            self.write_settings()
            self.assertEqual(self.invoke("status")["state"], "failed")
            self.settings[key] = original

    def test_package_operation_coverage(self):
        plugin = json.loads((PACKAGE / "plugin.json").read_text(encoding="utf-8"))
        tool = json.loads((PACKAGE / "tool.json").read_text(encoding="utf-8"))
        skill = json.loads((PACKAGE / "skill.json").read_text(encoding="utf-8"))
        self.assertEqual(plugin["id"], "desktop-pet.lx-music")
        self.assertEqual({entry["id"] for entry in plugin["entries"]}, {tool["id"], skill["id"]})
        self.assertEqual({operation["id"] for operation in tool["definition"]["operations"]}, OPERATIONS)
        self.assertEqual(len(tool["definition"]["operations"]), 12)
        self.assertEqual(skill["dependencies"][0]["id"], tool["id"])
        self.assertEqual(skill["dependencies"][0]["version"], tool["version"])
        self.assertEqual(set(skill["dependencies"][0]["operations"]), OPERATIONS)
        self.assertTrue((PACKAGE / skill["instructions"]).is_file())
        self.assertTrue((PACKAGE / tool["definition"]["instructions"]).is_file())
        for operation in tool["definition"]["operations"]:
            self.assertEqual(operation["result"]["completion"], "reported")
            self.assertEqual(operation["invocation"]["arguments"][-1], operation["id"])
            self.assertFalse(operation["parameters"]["schema"]["additionalProperties"])
            self.assertEqual(operation["effects"]["kind"], "read" if operation["id"] in
                             {"status", "lyric", "list_playlists", "playlist_songs"} else "write")


if __name__ == "__main__":
    unittest.main()
