# import unittest
# from unittest.mock import patch

# import light_music_server as lms


# class TestCommandExecutor(unittest.TestCase):
#     def setUp(self):
#         self.cfg = lms.ServiceConfig(
#             provider="netease_windows",
#             netease_client_path=r"D:\CloudMusic\cloudmusic.exe",
#             concurrency_policy="replace",
#         )
#         self.executor = lms.CommandExecutor(self.cfg)

#     @patch("light_music_server._find_netease_window", return_value=None)
#     @patch("light_music_server._is_process_running", return_value=False)
#     def test_next_track_window_not_found(self, _mock_running, _mock_find):
#         res = self.executor.submit("next_track", {}, "req-1")
#         self.assertFalse(res["success"])
#         self.assertEqual(res["command"], "next_track")
#         self.assertEqual(res["data"]["control"], "next_track")
#         self.assertEqual(res["data"]["control_method_used"], "shortcut_ctrl_right_only")
#         self.assertFalse(res["data"]["process_detected"])
#         self.assertEqual(res["error"], "netease_window_not_found")

#     @patch("light_music_server._activate_window", return_value=True)
#     @patch("light_music_server._send_ctrl_right_shortcut", return_value=True)
#     @patch("light_music_server._send_ctrl_right_to_window", return_value=True)
#     @patch("light_music_server._find_netease_window", return_value=1001)
#     def test_next_track_with_shortcut_only(
#         self,
#         _mock_find,
#         _mock_postmessage,
#         _mock_sendinput,
#         _mock_activate,
#     ):
#         res = self.executor.submit("next_track", {}, "req-1b")
#         self.assertTrue(res["success"])
#         self.assertFalse(res["fallback_used"])
#         self.assertEqual(res["data"]["control_method_used"], "shortcut_ctrl_right_only")
#         self.assertTrue(res["data"]["process_detected"])
#         self.assertTrue(res["data"]["shortcut_sent"])
#         self.assertTrue(res["data"]["window_activated"])
#         self.assertTrue(res["data"]["postmessage_sent"])
#         self.assertFalse(res["data"]["sendinput_1_sent"])
#         self.assertFalse(res["data"]["sendinput_2_sent"])

#     @patch("light_music_server._is_process_running", return_value=True)
#     @patch("light_music_server._activate_window", return_value=True)
#     @patch("light_music_server._send_ctrl_right_to_window", return_value=True)
#     @patch("light_music_server._find_netease_window", return_value=1001)
#     def test_next_track_debounce_blocks_duplicate(
#         self,
#         _mock_find,
#         mock_postmessage,
#         _mock_activate,
#         _mock_running,
#     ):
#         first = self.executor.submit("next_track", {}, "req-debounce-1")
#         second = self.executor.submit("next_track", {}, "req-debounce-2")

#         self.assertTrue(first["success"])
#         self.assertFalse(second["success"])
#         self.assertEqual(second["error"], "next_track_debounced")
#         self.assertFalse(second["data"]["shortcut_sent"])
#         self.assertEqual(mock_postmessage.call_count, 1)

#     def test_play_song_disabled(self):
#         res = self.executor.submit("play_song", {"song_id": "12345"}, "req-2")
#         self.assertFalse(res["success"])
#         self.assertIn("command_disabled_supported", res["error"])

#     def test_switch_playlist_disabled(self):
#         res = self.executor.submit("switch_playlist", {"playlist_id": "987"}, "req-5")
#         self.assertFalse(res["success"])
#         self.assertIn("command_disabled_supported", res["error"])

#     @patch("light_music_server._launch_client_if_needed", return_value=(False, "client_not_configured_or_not_found"))
#     def test_launch_client_not_ready(self, _mock_ready):
#         res = self.executor.submit("launch_netease", {}, "req-6")
#         self.assertFalse(res["success"])
#         self.assertEqual(res["error"], "client_not_configured_or_not_found")

#     @patch("light_music_server._is_process_running", return_value=True)
#     def test_is_netease_running(self, _mock_running):
#         res = self.executor.submit("is_netease_running", {}, "req-7")
#         self.assertTrue(res["success"])
#         self.assertTrue(res["data"]["running"])
#         self.assertEqual(res["data"]["checked_times"], 1)
#         self.assertEqual(res["data"]["retries"], 3)
#         self.assertEqual(res["data"]["interval_sec"], 5.0)

#     @patch("light_music_server.time.sleep")
#     @patch("light_music_server._is_process_running", side_effect=[False, False, True])
#     def test_is_netease_running_retry_then_success(self, _mock_running, _mock_sleep):
#         res = self.executor.submit("is_netease_running", {}, "req-7b")
#         self.assertTrue(res["success"])
#         self.assertTrue(res["data"]["running"])
#         self.assertEqual(res["data"]["checked_times"], 3)
#         self.assertEqual(_mock_sleep.call_count, 2)

#     @patch("light_music_server.time.sleep")
#     @patch("light_music_server._is_process_running", return_value=False)
#     def test_is_netease_running_retry_fail(self, _mock_running, _mock_sleep):
#         res = self.executor.submit("is_netease_running", {}, "req-7c")
#         self.assertTrue(res["success"])
#         self.assertFalse(res["data"]["running"])
#         self.assertEqual(res["data"]["checked_times"], 3)
#         self.assertEqual(_mock_sleep.call_count, 2)

#     @patch("light_music_server._launch_client_if_needed", return_value=(True, "already_running"))
#     def test_launch_netease(self, _mock_launch):
#         res = self.executor.submit("launch_netease", {}, "req-8")
#         self.assertTrue(res["success"])
#         self.assertTrue(res["data"]["already_running"])

#     @patch(
#         "light_music_server._collect_window_snapshot",
#         return_value={"foreground_hwnd": 1, "total_windows": 2, "matched_count": 1, "matched_windows": [], "sample_windows": []},
#     )
#     def test_debug_netease_windows(self, _mock_snapshot):
#         res = self.executor.submit("debug_netease_windows", {}, "req-9")
#         self.assertTrue(res["success"])
#         self.assertEqual(res["data"]["total_windows"], 2)
#         self.assertIn("matched_count", res["data"])


# class TestFlaskRoutes(unittest.TestCase):
#     def setUp(self):
#         lms.app.config["TESTING"] = True
#         self.client = lms.app.test_client()

#     def test_health(self):
#         r = self.client.get("/health")
#         self.assertEqual(r.status_code, 200)
#         body = r.get_json()
#         self.assertTrue(body["ok"])
#         self.assertIn("provider", body)

#     def test_command_missing_command(self):
#         r = self.client.post("/command", json={"args": {}})
#         self.assertEqual(r.status_code, 400)
#         body = r.get_json()
#         self.assertFalse(body["success"])
#         self.assertEqual(body["error"], "missing_command")

#     def test_command_busy_rejected(self):
#         reject_executor = lms.CommandExecutor(
#             lms.ServiceConfig(provider="netease_windows", netease_client_path="", concurrency_policy="reject")
#         )
#         reject_executor.latest_request_id = "already-busy"

#         with patch.object(lms, "executor", reject_executor):
#             r = self.client.post(
#                 "/command",
#                 json={"command": "next_track", "args": {}, "request_id": "req-busy"},
#             )
#         self.assertEqual(r.status_code, 409)
#         body = r.get_json()
#         self.assertFalse(body["success"])
#         self.assertEqual(body["error"], "busy_rejected")


# if __name__ == "__main__":
#     unittest.main()

import unittest
from unittest.mock import patch

import light_music_server as lms


class TestCommandExecutor(unittest.TestCase):
    def setUp(self):
        self.cfg = lms.ServiceConfig(
            provider="netease_windows",
            netease_client_path=r"D:\CloudMusic\cloudmusic.exe",
            concurrency_policy="replace",
        )
        self.executor = lms.CommandExecutor(self.cfg)

    @patch("light_music_server._launch_client_if_needed", return_value=(True, "already_running"))
    @patch("light_music_server._send_media_next_track", return_value=True)
    def test_next_track_success(self, _mock_next, _mock_ready):
        res = self.executor.submit("next_track", {}, "req-1")
        self.assertTrue(res["success"])
        self.assertEqual(res["command"], "next_track")
        self.assertEqual(res["data"]["control"], "media_next_track")

    @patch("light_music_server._launch_client_if_needed", return_value=(True, "already_running"))
    def test_play_song_missing_identifier(self, _mock_ready):
        res = self.executor.submit("play_song", {}, "req-2")
        self.assertFalse(res["success"])
        self.assertEqual(res["error"], "missing_song_identifier")

    @patch("light_music_server._launch_client_if_needed", return_value=(True, "already_running"))
    @patch.object(lms.CommandExecutor, "_open_uri", return_value=(True, "ok"))
    def test_play_song_song_id_deep_link(self, _mock_open, _mock_ready):
        res = self.executor.submit("play_song", {"song_id": "12345"}, "req-3")
        self.assertTrue(res["success"])
        self.assertEqual(res["data"]["mode"], "deep_link")
        self.assertIn("orpheus://song/12345", res["data"]["uri"])

    @patch("light_music_server._launch_client_if_needed", return_value=(True, "already_running"))
    @patch.object(lms.CommandExecutor, "_open_uri", return_value=(False, "blocked"))
    def test_play_song_fallback_search(self, _mock_open, _mock_ready):
        res = self.executor.submit("play_song", {"song": "稻香", "artist": "周杰伦"}, "req-4")
        self.assertTrue(res["success"])
        self.assertTrue(res["fallback_used"])
        self.assertEqual(res["data"]["mode"], "fallback_search")
        self.assertIn("稻香", res["data"]["query"])

    @patch("light_music_server._launch_client_if_needed", return_value=(True, "already_running"))
    @patch.object(lms.CommandExecutor, "_open_uri", return_value=(True, "ok"))
    def test_switch_playlist_success(self, _mock_open, _mock_ready):
        res = self.executor.submit("switch_playlist", {"playlist_id": "987"}, "req-5")
        self.assertTrue(res["success"])
        self.assertEqual(res["data"]["mode"], "playlist_url")
        self.assertIn("playlist?id=987", res["data"]["url"])

    @patch("light_music_server._launch_client_if_needed", return_value=(False, "client_not_configured_or_not_found"))
    def test_client_not_ready(self, _mock_ready):
        res = self.executor.submit("next_track", {}, "req-6")
        self.assertFalse(res["success"])
        self.assertEqual(res["error"], "client_not_configured_or_not_found")


class TestFlaskRoutes(unittest.TestCase):
    def setUp(self):
        lms.app.config["TESTING"] = True
        self.client = lms.app.test_client()

    def test_health(self):
        r = self.client.get("/health")
        self.assertEqual(r.status_code, 200)
        body = r.get_json()
        self.assertTrue(body["ok"])
        self.assertIn("provider", body)

    def test_command_missing_command(self):
        r = self.client.post("/command", json={"args": {}})
        self.assertEqual(r.status_code, 400)
        body = r.get_json()
        self.assertFalse(body["success"])
        self.assertEqual(body["error"], "missing_command")

    def test_command_busy_rejected(self):
        reject_executor = lms.CommandExecutor(
            lms.ServiceConfig(provider="netease_windows", netease_client_path="", concurrency_policy="reject")
        )
        reject_executor.latest_request_id = "already-busy"

        with patch.object(lms, "executor", reject_executor):
            r = self.client.post(
                "/command",
                json={"command": "next_track", "args": {}, "request_id": "req-busy"},
            )
        self.assertEqual(r.status_code, 409)
        body = r.get_json()
        self.assertFalse(body["success"])
        self.assertEqual(body["error"], "busy_rejected")


if __name__ == "__main__":
    unittest.main()
