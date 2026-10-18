"""Synthetic image-chat QA; no profile, real images, model, or pet process.

Run with launcher/.venv/bin/python. Default uses offscreen widgets; --native
opens the actual launcher shell with its process tracker replaced.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "launcher"))
NATIVE = "--native" in sys.argv
if not NATIVE:
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QMimeData, QObject, QPointF, QTimer, Signal, Qt
from PySide6.QtGui import QColor, QFont, QImage, QPainter, QPolygonF
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication
from qfluentwidgets import Theme, setTheme

from pages.chat_page import ChatPage


def synthetic_image(width=1000, height=650, transparent=False):
    image = QImage(width, height, QImage.Format_ARGB32)
    image.fill(Qt.transparent if transparent else QColor("#DDF0F4"))
    painter = QPainter(image)
    painter.setRenderHint(QPainter.Antialiasing)
    painter.setPen(Qt.NoPen)
    painter.setBrush(QColor("#FFD684"))
    painter.drawEllipse(QPointF(width * .76, height * .24), width * .09, width * .09)
    painter.setBrush(QColor("#759FA3"))
    painter.drawPolygon(QPolygonF([QPointF(0, height * .8), QPointF(width * .35, height * .3),
                                  QPointF(width * .72, height * .8)]))
    painter.setBrush(QColor("#A8C8BD"))
    painter.drawPolygon(QPolygonF([QPointF(width * .2, height * .86), QPointF(width * .67, height * .44),
                                  QPointF(width, height * .8), QPointF(width, height), QPointF(0, height)]))
    painter.setPen(QColor("#284D5B"))
    font = QFont()
    font.setPixelSize(max(18, width // 26))
    painter.setFont(font)
    painter.drawText(int(width * .06), int(height * .13), "WEEKEND / SYNTHETIC IMAGE")
    painter.end()
    return image


class ImageClient(QObject):
    disconnected = Signal()
    stateReceived = Signal(dict)
    operationFinished = Signal(str, dict)
    operationFailed = Signal(str, str)
    historyFailed = Signal(str, str, str)

    def __init__(self, directory):
        super().__init__()
        self.directory = Path(directory)
        self.is_connected = True
        self.requests = []
        self.sent = []
        self.revision = 1
        self.assets = []
        for i, (width, height, transparent) in enumerate(((1000, 650, False), (480, 760, False), (650, 480, True))):
            path = self.directory / f"synthetic-{i}.png"
            assert synthetic_image(width, height, transparent).save(str(path))
            self.assets.append({"id": f"fixture-{i}", "path": str(path), "name": f"周末插画-{i + 1}.png",
                                "mimeType": "image/png"})
        self.messages = [{"id": str(i), "role": "user" if i % 2 else "assistant", "status": "complete",
                          "content": f"周末安排 {i}：找个安静的地方走走，看看沿途的风景。",
                          "timestamp": f"2026-10-18T10:{i % 60:02d}:00+08:00", "images": []}
                         for i in range(90)]
        self.messages[15].update(content="早先发过的图片", images=[self.assets[1]])
        self.messages[-3].update(role="user", content="看看这张插画，适合周末出游的感觉吗？", images=[self.assets[0]])
        self.messages[-2].update(role="assistant", content="柔和的山色和阳光，看起来很适合慢慢散步。")
        self.messages[-1].update(role="user", content="", images=[self.assets[2]])

    def request_state(self, _revision=-1):
        self.stateReceived.emit({"revision": self.revision, "profileId": "synthetic-image-qa", "petName": "小晴",
                                "aiEnabled": True, "busy": False, "messages": self.messages[-40:],
                                "hasMoreBefore": True, "hasMoreAfter": False, "diagnosticMessages": []})

    def request_history(self, before_message_id="", around_message_id="", limit=40, request_token=""):
        if around_message_id:
            start = max(0, int(around_message_id) - limit // 2)
            end = min(len(self.messages), start + limit)
        else:
            end = int(before_message_id) if before_message_id else len(self.messages)
            start = max(0, end - limit)
        self.requests.append({"before": before_message_id, "around": around_message_id})
        result = {"messages": self.messages[start:end], "hasMoreBefore": start > 0,
                  "hasMoreAfter": end < len(self.messages), "requestToken": request_token,
                  "anchorMessageId": around_message_id}
        QTimer.singleShot(10, self, lambda: self.operationFinished.emit("get_chat_history", result))

    def search_history(self, query, before_message_id="", limit=30, request_token=""):
        end = int(before_message_id) if before_message_id else len(self.messages)
        found = [m for m in reversed(self.messages[:end]) if query.lower() in m["content"].lower()]
        result = {"messages": found[:limit], "query": query, "requestToken": request_token,
                  "hasMoreBefore": len(found) > limit,
                  "nextBeforeMessageId": found[min(limit, len(found)) - 1]["id"] if found else ""}
        QTimer.singleShot(10, self, lambda: self.operationFinished.emit("search_chat_history", result))

    def send_message(self, text, images=None):
        saved = []
        for i, image in enumerate(images or []):
            path = self.directory / f"sent-{len(self.sent)}-{i}{Path(image['path']).suffix}"
            shutil.copyfile(image["path"], path)
            saved.append({"id": path.stem, "path": str(path), "name": image["name"],
                          "mimeType": "image/png" if path.suffix == ".png" else "image/jpeg"})
        self.sent.append({"text": text, "images": saved})
        message_id = str(len(self.messages))
        self.messages.append({"id": message_id, "role": "user", "content": text, "images": saved,
                              "status": "complete", "timestamp": "2026-10-18T12:00:00+08:00"})
        self.revision += 1
        QTimer.singleShot(10, self, lambda: self.operationFinished.emit("send_message", {"messageId": message_id}))


def paste_image(page):
    mime = QMimeData()
    mime.setImageData(synthetic_image(650, 480))
    page.input_edit.insertFromMimeData(mime)


def main():
    app = QApplication.instance() or QApplication(["synthetic-image-chat-qa"])
    app.setOrganizationName("DesktopPetImageQA")
    app.setApplicationName("LauncherImageQA")
    output = Path(__file__).resolve().parent
    evidence = []
    with tempfile.TemporaryDirectory(prefix="desktop-pet-image-qa-") as directory:
        for theme in ((Theme.LIGHT,) if NATIVE else (Theme.LIGHT, Theme.DARK)):
            setTheme(theme)
            if NATIVE:
                from main import LauncherWindow
                with patch("main.PetProcessTracker") as tracker:
                    tracker.return_value.refresh.return_value = 0
                    shell = LauncherWindow()
                shell._pet_process_timer.stop()
                page = shell.chat_page
                shell.resize(1100, 850)
                shell.show()
                shell.switchTo(page)
            else:
                shell = page = ChatPage()
                page.resize(1100, 850)
                page.show()
            # Launcher construction restores its saved theme. Apply the QA
            # theme afterwards so native screenshots match their labels.
            setTheme(theme)
            page.refresh_theme()
            client = ImageClient(directory)
            page.set_client(client)
            page._poll_timer.stop()
            QTest.qWait(150)
            assert "89" in page._rows and not page._rows["89"].text_label.isVisible()
            assert not client.requests
            if not NATIVE:
                from pages.chat_images import ImagePreviewDialog
                page._rows["89"].image_grid.thumbnails[0].click()
                QTest.qWait(80)
                preview = next(widget for widget in app.topLevelWidgets()
                               if isinstance(widget, ImagePreviewDialog) and widget.isVisible())
                assert not preview.image_label.pixmap().isNull()
                assert preview.grab().save(str(output / f"{theme.value}-preview.png"))
                preview.close()
            for width in ((1100,) if NATIVE else (1100, 700, 460)):
                shell.resize(width, 850)
                QTest.qWait(100)
                assert page.message_area.horizontalScrollBar().maximum() == 0
                assert page.minimumSizeHint().width() <= page.width()
                if width != 460:
                    prefix = "Launcher-" if NATIVE else ""
                    assert shell.grab().save(str(output / f"{prefix}{theme.value}-{width}-images.png"))
            paste_image(page)
            paste_image(page)
            paste_image(page)
            paste_image(page)
            page.input_edit.setPlainText("也看看这张截图。")
            QTest.qWait(100)
            assert shell.grab().save(str(output / f"{'Launcher-' if NATIVE else ''}{theme.value}-draft.png"))
            QTest.keyClick(page.input_edit, Qt.Key_Return)
            QTest.qWait(150)
            assert len(client.sent) == 1 and len(client.sent[0]["images"]) == 4
            assert not page.input_edit.toPlainText()
            assert Path(client.sent[0]["images"][0]["path"]).is_file()
            page.message_area.verticalScrollBar().setValue(0)
            QTest.keyClick(page.message_area, Qt.Key_PageUp)
            QTest.qWait(150)
            assert client.requests and len(page._rows) >= 80
            page.open_history_search()
            page.search_input.setText("早先发过的图片")
            QTest.keyClick(page.search_input, Qt.Key_Return)
            QTest.qWait(100)
            assert page.search_results.count() == 1
            match = page.search_results.item(0)
            page.search_results.itemClicked.emit(match)
            QTest.qWait(150)
            assert "15" in page._rows and page._rows["15"].is_highlighted
            assert page._rows["15"].message["images"]
            evidence.append({"theme": theme.value, "native": NATIVE, "sent_images": 4,
                             "history_with_images": True, "search_located_image_message": "15"})
            page.set_client(None)
            shell.close()
            shell.deleteLater()
            app.processEvents()
    (output / ("native-evidence.json" if NATIVE else "evidence.json")).write_text(
        json.dumps(evidence, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("PASS: image paste/send, persistent fixture copy, history and search, responsive layout")


if __name__ == "__main__":
    main()
