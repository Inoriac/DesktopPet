"""Render synthetic failure/retry states; no pet process or model requests."""
import os
from pathlib import Path
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT / "launcher"), str(ROOT / "tests")]
from PySide6.QtWidgets import QApplication
from PySide6.QtTest import QTest
from qfluentwidgets import Theme, setTheme
from test_launcher_chat import StubChatClient
from pages.chat_page import ChatPage

app = QApplication.instance() or QApplication([])
output = Path(__file__).parent / "visual-qa"
output.mkdir(exist_ok=True)
for theme in (Theme.LIGHT, Theme.DARK):
    setTheme(theme)
    page = ChatPage()
    client = StubChatClient()
    page.set_client(client)
    page._poll_timer.stop()
    page._known_statuses = None
    page._apply_state({"revision": 2, "petName": "Milltina", "aiEnabled": True, "busy": False,
        "messages": [
            {"id": "u1", "role": "user", "status": "complete", "content": "帮我安排一下周末吧"},
            {"id": "a1", "role": "assistant", "replyToId": "u1", "status": "failed", "content": "",
             "errorMessage": "LLM network error (code 2): Connection closed"},
            {"id": "u2", "role": "user", "status": "complete", "content": "想找个安静的地方休息"},
            {"id": "a2", "role": "assistant", "replyToId": "u2", "status": "interrupted",
             "content": "可以先找一家安静的书店，带上你喜欢的茶，再沿着附近的街道走走。"},
        ]})
    page.resize(460, 720)
    page.show()
    QTest.qWait(250)
    page._poll_timer.stop()
    page.scroll_to_bottom()
    QTest.qWait(100)
    assert set(page._rows) == {"u1", "a1", "u2", "a2"}
    assert page.message_area.horizontalScrollBar().maximum() == 0
    for row in page._rows.values():
        if row.status_label.text():
            assert row.status_bar.geometry().bottom() <= row.bubble.height()
            assert row.retry_button.isVisible()
    page.grab().save(str(output / f"{theme.value}-failure-retry.png"))
    page.close()
