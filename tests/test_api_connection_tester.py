import json
import os
import sys
import types
import unittest


REPOSITORY_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPOSITORY_ROOT, "launcher"))


class FakeSignal:
    def __init__(self):
        self._callbacks = []

    def connect(self, callback):
        self._callbacks.append(callback)

    def emit(self, *args):
        for callback in list(self._callbacks):
            callback(*args)


class SignalDescriptor:
    def __set_name__(self, owner, name):
        self._name = name

    def __get__(self, instance, owner):
        if instance is None:
            return self
        return instance.__dict__.setdefault(self._name, FakeSignal())


class QObject:
    def __init__(self, parent=None):
        self.parent = parent


class QUrl:
    def __init__(self, value):
        self._value = value

    def toString(self):
        return self._value


class QNetworkRequest:
    ContentTypeHeader = 1
    HttpStatusCodeAttribute = 2

    def __init__(self, url):
        self._url = url
        self.headers = {}
        self.raw_headers = {}
        self.timeout = None

    def url(self):
        return self._url

    def setHeader(self, name, value):
        self.headers[name] = value

    def setRawHeader(self, name, value):
        self.raw_headers[bytes(name)] = bytes(value)

    def setTransferTimeout(self, timeout):
        self.timeout = timeout


class QNetworkReply:
    class NetworkError:
        NoError = 0


class FakeReply:
    def __init__(self, status=200, body=b"{}", error=0, error_text=""):
        self.finished = FakeSignal()
        self._status = status
        self._body = body
        self._error = error
        self._error_text = error_text
        self.deleted = False

    def attribute(self, name):
        return self._status if name == QNetworkRequest.HttpStatusCodeAttribute else None

    def readAll(self):
        return self._body

    def error(self):
        return self._error

    def errorString(self):
        return self._error_text

    def deleteLater(self):
        self.deleted = True


class FakeNetworkManager:
    def __init__(self):
        self.posts = []
        self.next_reply = FakeReply()

    def post(self, request, body):
        self.posts.append((request, bytes(body)))
        return self.next_reply


def _install_qt_stubs():
    try:
        import PySide6  # noqa: F401
        return
    except ModuleNotFoundError:
        pass

    pyside = types.ModuleType("PySide6")
    qtcore = types.ModuleType("PySide6.QtCore")
    qtcore.QObject = QObject
    qtcore.Signal = lambda *args: SignalDescriptor()
    qtcore.QUrl = QUrl
    qtcore.Qt = type("Qt", (), {"Vertical": 1})
    qtnetwork = types.ModuleType("PySide6.QtNetwork")
    qtnetwork.QNetworkAccessManager = object
    qtnetwork.QNetworkReply = QNetworkReply
    qtnetwork.QNetworkRequest = QNetworkRequest
    sys.modules.setdefault("PySide6", pyside)
    sys.modules.setdefault("PySide6.QtCore", qtcore)
    sys.modules.setdefault("PySide6.QtNetwork", qtnetwork)


def _install_ui_stubs():
    if "qfluentwidgets" in sys.modules:
        return

    class Dummy:
        DemiBold = 1

        def __init__(self, *args, **kwargs):
            pass

    qtgui = types.ModuleType("PySide6.QtGui")
    qtgui.QColor = Dummy
    qtgui.QFont = Dummy
    qtwidgets = types.ModuleType("PySide6.QtWidgets")
    for name in ("QWidget", "QHBoxLayout", "QVBoxLayout", "QSizePolicy"):
        setattr(qtwidgets, name, Dummy)
    fluent = types.ModuleType("qfluentwidgets")
    for name in (
        "SwitchButton", "SpinBox", "ComboBox", "LineEdit",
        "PasswordLineEdit", "PrimaryPushButton", "PushButton", "InfoBar",
        "SingleDirectionScrollArea", "HeaderCardWidget", "StrongBodyLabel",
        "CaptionLabel",
    ):
        setattr(fluent, name, Dummy)
    fluent.FluentIcon = type("FluentIcon", (), {})
    fluent.InfoBarPosition = type("InfoBarPosition", (), {"TOP": 1})
    fluent.setFont = lambda *args, **kwargs: None
    sys.modules.setdefault("PySide6.QtGui", qtgui)
    sys.modules.setdefault("PySide6.QtWidgets", qtwidgets)
    sys.modules.setdefault("qfluentwidgets", fluent)


_install_qt_stubs()

from api_connection_tester import ApiConnectionTester  # noqa: E402
from app_state import ModelEndpointState  # noqa: E402


class ApiConnectionTesterTests(unittest.TestCase):
    def _tester(self, reply=None):
        manager = FakeNetworkManager()
        if reply is not None:
            manager.next_reply = reply
        tester = ApiConnectionTester(network_manager=manager)
        results = []
        tester.finished.connect(lambda *args: results.append(args))
        return tester, manager, results

    def test_whenAnthropicEndpointAndRoleModelAreValid_shouldBuildMessagesProbeAndReportSuccess(self):
        tester, manager, results = self._tester(FakeReply(status=200))
        endpoint = ModelEndpointState(
            provider="anthropic-messages",
            base_url="https://text.example/v1",
            api_key="secret",
            anthropic_version="2024-01-01",
        )

        tester.test("anthropic-request", endpoint, "claude-text")

        request, raw_body = manager.posts[0]
        self.assertEqual(request.url().toString(), "https://text.example/v1/messages")
        self.assertEqual(request.raw_headers[b"x-api-key"], b"secret")
        self.assertEqual(request.raw_headers[b"anthropic-version"], b"2024-01-01")
        body = json.loads(raw_body)
        self.assertEqual(body["max_tokens"], 1)
        self.assertFalse(body["stream"])
        manager.next_reply.finished.emit()
        self.assertEqual(results, [("anthropic-request", True, "success", "连接成功")])

    def test_whenOpenAiCompatibleEndpointIsValid_shouldBuildChatCompletionsProbe(self):
        tester, manager, results = self._tester(FakeReply(status=204))
        endpoint = ModelEndpointState(
            provider="openai-compatible",
            base_url="https://vision.example/v1",
            api_key="vision-secret",
        )

        tester.test("openai-request", endpoint, "vision-model")

        request, raw_body = manager.posts[0]
        self.assertEqual(
            request.url().toString(),
            "https://vision.example/v1/chat/completions",
        )
        self.assertEqual(request.raw_headers[b"Authorization"], b"Bearer vision-secret")
        body = json.loads(raw_body)
        self.assertEqual(body["messages"], [{"role": "user", "content": "ping"}])
        manager.next_reply.finished.emit()
        self.assertEqual(results[0][1:3], (True, "success"))

    def test_whenProviderIsUnsupported_shouldFailBeforeNetwork(self):
        tester, manager, results = self._tester()
        endpoint = ModelEndpointState(
            provider="unsupported-provider",
            base_url="https://api.example/v1",
            api_key="secret",
        )

        tester.test("invalid-request", endpoint, "model")

        self.assertEqual(manager.posts, [])
        self.assertEqual(results[0][0:3], ("invalid-request", False, "invalid"))

    def test_whenProviderErrorContainsApiKey_shouldRedactAndTruncateMessage(self):
        key = "super-secret-api-key"
        provider_message = f"Authorization: Bearer {key} " + ("x" * 900)
        reply = FakeReply(
            status=401,
            body=json.dumps({"error": {"message": provider_message}}).encode(),
        )
        tester, manager, results = self._tester(reply)
        endpoint = ModelEndpointState(
            provider="anthropic-messages",
            base_url="https://text.example",
            api_key=key,
        )

        tester.test("error-request", endpoint, "claude-text")
        manager.next_reply.finished.emit()

        request_id, success, category, message = results[0]
        self.assertEqual(request_id, "error-request")
        self.assertFalse(success)
        self.assertEqual(category, "authentication")
        self.assertNotIn(key, message)
        self.assertLessEqual(len(message), 400)

    def test_whenApiKeyChangesWhilePending_shouldRedactTheKeyUsedForRequest(self):
        original_key = "original-secret-api-key"
        reply = FakeReply(
            status=401,
            body=json.dumps({
                "error": {"message": f"rejected key {original_key}"}
            }).encode(),
        )
        tester, manager, results = self._tester(reply)
        endpoint = ModelEndpointState(
            provider="anthropic-messages",
            base_url="https://text.example",
            api_key=original_key,
        )

        tester.test("changed-key-request", endpoint, "claude-text")
        endpoint.api_key = "new-secret-api-key"
        manager.next_reply.finished.emit()

        self.assertNotIn(original_key, results[0][3])


class FakeButton:
    def __init__(self):
        self.enabled = True

    def setEnabled(self, enabled):
        self.enabled = enabled


class FakeTester:
    def __init__(self):
        self.finished = FakeSignal()
        self.requests = []

    def test(self, request_id, endpoint, model):
        self.requests.append((request_id, endpoint, model))


class AiPageConnectionTests(unittest.TestCase):
    @staticmethod
    def _page():
        _install_ui_stubs()
        from pages.ai_page import AiPage

        page = object.__new__(AiPage)
        page.connection_tester = FakeTester()
        page._connection_test_buttons = {
            "DEFAULT": FakeButton(),
            "VISION_VENDOR": FakeButton(),
        }
        page._active_connection_test_ids = {
            "DEFAULT": None,
            "VISION_VENDOR": None,
        }
        page._shown_connection_results = []
        page._show_connection_test_result = (
            lambda endpoint_id, success, category, message:
            page._shown_connection_results.append(
                (endpoint_id, success, category, message)))
        page.connection_tester.finished.connect(page._on_connection_test_finished)
        return page

    def test_connectionTest_whenTwoEndpointsRunTogether_shouldMaintainIndependentPendingState(self):
        page = self._page()
        default = ModelEndpointState()
        vision = ModelEndpointState()

        text_id = page._start_connection_test("DEFAULT", default, "text")
        vision_id = page._start_connection_test(
            "VISION_VENDOR", vision, "vision")

        self.assertNotEqual(text_id, vision_id)
        self.assertFalse(page._connection_test_buttons["DEFAULT"].enabled)
        self.assertFalse(page._connection_test_buttons["VISION_VENDOR"].enabled)
        page.connection_tester.finished.emit(text_id, True, "success", "ok")
        self.assertTrue(page._connection_test_buttons["DEFAULT"].enabled)
        self.assertFalse(page._connection_test_buttons["VISION_VENDOR"].enabled)

    def test_connectionTest_whenOldRequestFinishesLate_shouldIgnoreStaleResult(self):
        page = self._page()
        endpoint = ModelEndpointState()

        old_id = page._start_connection_test("DEFAULT", endpoint, "text")
        new_id = page._start_connection_test("DEFAULT", endpoint, "text")
        page.connection_tester.finished.emit(old_id, False, "network", "old")

        self.assertFalse(page._connection_test_buttons["DEFAULT"].enabled)
        self.assertEqual(page._shown_connection_results, [])
        page.connection_tester.finished.emit(new_id, True, "success", "new")
        self.assertTrue(page._connection_test_buttons["DEFAULT"].enabled)
        self.assertEqual(page._shown_connection_results[0][-1], "new")


if __name__ == "__main__":
    unittest.main()
