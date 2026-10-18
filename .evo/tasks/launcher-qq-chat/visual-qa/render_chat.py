"""Reproducible launcher chat QA with a synthetic 180-message archive.

Run from the repository root:
QT_QPA_PLATFORM=offscreen launcher/.venv/bin/python \
    .evo/tasks/launcher-qq-chat/visual-qa/render_chat.py

No profile, database, model or external application is accessed.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "launcher"))

from PySide6.QtCore import QObject, QTimer, Signal, Qt
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication
from qfluentwidgets import Theme, setTheme

from pages.chat_page import ChatPage


def archive_messages():
    phrases = [
        "旅行清单：轻便背包、充电器和一双舒服的鞋。周末就慢慢走，不赶行程。",
        "好呀，上午沿着河边散步，下午找个有阳光的地方坐一会儿。",
        "听起来不错。顺路买点水果，再拍几张路上的照片。",
        "旅行清单里再加一本书吧，休息的时候可以读几页。",
        "路线已经整理好了。地铁出来步行十分钟就能到公园入口。",
        "那就这样安排，天气变化的话我们再调整。",
        "旅行清单已更新，也记得带水。沿途有几处适合休息的小店。",
        "收到，晚上回来以后再挑照片，今天就安心享受散步。",
        "我们还可以记下喜欢的小店，下次经过时再去看看。",
        "旅行清单：相机已经充好电了。想记录一些阳光和树影。",
        "午后可能会有阵雨，把小伞也放进包里就好。",
        "准备妥当啦。明天出发前我们再确认一次路线。",
    ]
    return [{"id": str(i), "role": "user" if i % 2 else "assistant",
             "content": phrases[i % len(phrases)], "status": "complete",
             "timestamp": f"2026-10-18T{9 + i // 60:02d}:{i % 60:02d}:00+08:00"}
            for i in range(180)]


class ArchiveClient(QObject):
    disconnected = Signal()
    stateReceived = Signal(dict)
    operationFinished = Signal(str, dict)
    operationFailed = Signal(str, str)
    historyFailed = Signal(str, str, str)

    def __init__(self):
        super().__init__()
        self.is_connected = True
        self.messages = archive_messages()
        self.requests = []
        self.state = {"revision": 1, "profileId": "synthetic-qa", "petName": "小晴",
                      "aiEnabled": True, "busy": False, "messages": self.messages[-40:],
                      "hasMoreBefore": True, "hasMoreAfter": False, "diagnosticMessages": []}

    def request_state(self, _after_revision=-1):
        self.stateReceived.emit(self.state)

    def request_history(self, before_message_id="", around_message_id="", limit=40, request_token=""):
        if around_message_id:
            start = max(0, int(around_message_id) - limit // 2)
            end = min(len(self.messages), start + limit)
        else:
            end = int(before_message_id) if before_message_id else len(self.messages)
            start = max(0, end - limit)
        result = {"messages": self.messages[start:end], "hasMoreBefore": start > 0,
                  "hasMoreAfter": end < len(self.messages), "requestToken": request_token,
                  "anchorMessageId": around_message_id}
        self.requests.append({"action": "history", "before": before_message_id,
                              "around": around_message_id, "returned": end - start})
        QTimer.singleShot(10, self, lambda: self.operationFinished.emit("get_chat_history", result))

    def search_history(self, query, before_message_id="", limit=30, request_token=""):
        end = int(before_message_id) if before_message_id else len(self.messages)
        matches = [m for m in reversed(self.messages[:end]) if query.lower() in m["content"].lower()]
        result = {"messages": matches[:limit], "hasMoreBefore": len(matches) > limit,
                  "query": query, "requestToken": request_token,
                  "nextBeforeMessageId": matches[min(limit, len(matches)) - 1]["id"] if matches else ""}
        self.requests.append({"action": "search", "query": query, "before": before_message_id,
                              "returned": len(result["messages"])})
        QTimer.singleShot(10, self, lambda: self.operationFinished.emit("search_chat_history", result))


def settle():
    QTest.qWait(120)


def main():
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    app = QApplication.instance() or QApplication(["synthetic-chat-qa"])
    output = Path(__file__).resolve().parent
    evidence = []
    for theme in (Theme.LIGHT, Theme.DARK):
        setTheme(theme)
        page = ChatPage()
        page.resize(1100, 760)
        page.show()
        client = ArchiveClient()
        page.set_client(client)
        page._poll_timer.stop()
        settle()
        assert len(page._rows) == 40 and not client.requests
        page.grab().save(str(output / f"{theme.value}-1100-latest.png"))

        page.message_area.verticalScrollBar().setValue(0)
        QTest.keyClick(page.message_area, Qt.Key_PageUp)
        settle()
        assert len(page._rows) == 80 and client.requests[-1]["before"] == "140"
        page.open_history_search()
        page.search_input.setText("旅行清单")
        QTest.keyClick(page.search_input, Qt.Key_Return)
        settle()
        assert page.search_results.count() == 30
        page.search_more_button.click()
        settle()
        assert page.search_results.count() == 60 and not page.search_more_button.isVisible()
        match = next(page.search_results.item(i) for i in range(page.search_results.count())
                     if page.search_results.item(i).data(Qt.UserRole) == "24")
        page.search_results.setCurrentItem(match)
        page.search_results.scrollToItem(match)
        page.search_results.itemClicked.emit(match)
        settle()
        assert set(page._rows) == {str(i) for i in range(4, 44)}
        target = page._rows["24"]
        offset = target.y() - page.message_area.verticalScrollBar().value()
        assert target.is_highlighted and 0 <= offset < page.message_area.viewport().height()
        page.input_edit.setPlainText("查完记录了，继续聊聊周末的安排吧。")
        page.grab().save(str(output / f"{theme.value}-1100-search.png"))

        client.state["revision"] = 2
        client.state["task"] = {"id": "qa-task", "status": "Paused", "goal": "整理周末出行安排",
                                 "reason": "行程已保存，可以补充想去的地方。", "tool_calls": 12,
                                 "budget": {"max_tool_calls": 12}}
        page._apply_state(client.state)
        for width in (700, 460):
            page.resize(width, 760)
            settle()
            assert page.minimumSizeHint().width() <= width
            assert page.message_area.horizontalScrollBar().maximum() == 0
            assert page.message_area.viewport().height() >= 60
            assert page.task_extend.isVisible() and page.action_button.isVisible()
            if width == 700:
                page.search_results.scrollToItem(match)
                page.search_results.itemClicked.emit(match)
                settle()
                page.grab().save(str(output / f"{theme.value}-700-search-task.png"))

        page.scroll_to_bottom()
        settle()
        assert len(page._rows) == 40 and page._follow_bottom
        evidence.append({"theme": theme.value, "archive_count": 180, "search_count": 60,
                         "located_message_id": "24", "latest_restored": True,
                         "requests": client.requests})
        page.set_client(None)
        page.close()
        page.deleteLater()
        app.processEvents()
    (output / "evidence.json").write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n",
                                         encoding="utf-8")
    print("PASS: synthetic archive pagination, search, locate, latest; light/dark and 1100/700/460 widths")


if __name__ == "__main__":
    main()
