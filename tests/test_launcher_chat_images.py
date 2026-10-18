"""Synthetic image input/history tests; no personal files or model calls."""
import os
from pathlib import Path
import sys
import tempfile
import random
import struct
import unittest
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "launcher"))

from PySide6.QtCore import QMimeData, QPointF, Qt, QUrl
from PySide6.QtGui import QColor, QDragEnterEvent, QDropEvent, QImage, QImageReader
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication

from pages.chat_images import ChatImageDraft, ImagePreviewDialog, ImageThumbnail
from pages.chat_page import ChatPage
from pages.chat_widgets import MessageRow
from test_launcher_chat import StubChatClient


class ImageClient(StubChatClient):
    def send_message(self, text, images=None):
        self.sent.append({"text": text, "images": images or []})

    def get_state(self, after_revision=-1):
        return {**super().get_state(after_revision), "profileId": "image-profile"}


class LauncherChatImageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication(["chat-image-tests"])

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.errors = patch("pages.chat_page.InfoBar.error").start()
        self.addCleanup(patch.stopall)

    def image(self, width=640, height=320, alpha=False):
        result = QImage(width, height, QImage.Format_ARGB32 if alpha else QImage.Format_RGB32)
        result.fill(QColor(72, 150, 221, 100 if alpha else 255))
        return result

    def source(self, name="synthetic.png", width=640, height=320, alpha=False):
        path = Path(self.temp.name) / name
        self.assertTrue(self.image(width, height, alpha).save(str(path)))
        return str(path)

    def draft(self):
        result = ChatImageDraft()
        self.addCleanup(result.clear)
        return result

    def page(self):
        page = ChatPage()
        page.resize(840, 720)
        page.show()
        client = ImageClient()
        page.set_client(client)
        page._poll_timer.stop()
        self.addCleanup(page.deleteLater)
        self.addCleanup(page.close)
        self.addCleanup(lambda: page.set_client(None))
        self.addCleanup(page._image_draft.clear)
        self.settle()
        return page, client

    def settle(self):
        for _ in range(4):
            self.app.processEvents()
        QTest.qWait(10)

    def test_normalized_copy_survives_source_deletion_and_respects_limits(self):
        draft = self.draft()
        source = self.source(width=3200, height=1600)
        item = draft.add_file(source)
        Path(source).unlink()
        sent = QImage(item.path)
        self.assertEqual((sent.width(), sent.height()), (2048, 1024))
        self.assertLessEqual(Path(item.path).stat().st_size, 2 * 1024 * 1024)
        self.assertEqual(item.name, "synthetic.png")
        self.assertLessEqual(max(item.thumbnail.width(), item.thumbnail.height()), 160)
        draft.remove(item.path)
        self.assertFalse(Path(item.path).exists())

    def test_alpha_kept_and_invalid_or_excess_inputs_do_not_replace_draft(self):
        draft = self.draft()
        image = draft.add_image(self.image(alpha=True), "clipboard.png")
        self.assertTrue(QImage(image.path).hasAlphaChannel())
        bad = Path(self.temp.name) / "invalid.png"
        bad.write_text("not an image")
        with self.assertRaises(ValueError):
            draft.add_file(str(bad))
        huge = Path(self.temp.name) / "oversize.png"
        with huge.open("wb") as stream:
            stream.truncate(20 * 1024 * 1024 + 1)
        with self.assertRaisesRegex(ValueError, "20"):
            draft.add_file(str(huge))
        with patch("pages.chat_images.MAX_PIXELS", 100):
            with self.assertRaisesRegex(ValueError, "像素"):
                draft.add_image(self.image(20, 20))
        for _ in range(3):
            draft.add_image(self.image(32, 32))
        with self.assertRaisesRegex(ValueError, "4"):
            draft.add_image(self.image())
        self.assertEqual(len(draft.items), 4)
        self.assertTrue(Path(image.path).exists())

    def test_jpeg_orientation_and_high_dpi_clipboard_are_normalized(self):
        draft = self.draft()
        source = Path(self.source(name="rotated.jpg", width=80, height=40))
        # One standard EXIF Orientation=6 entry: rotate clockwise on decode.
        tiff = b"II" + struct.pack("<HIH", 42, 8, 1)
        tiff += struct.pack("<HHIHHI", 0x0112, 3, 1, 6, 0, 0)
        exif = b"Exif\0\0" + tiff
        original = source.read_bytes()
        source.write_bytes(original[:2] + b"\xff\xe1" + struct.pack(">H", len(exif) + 2) + exif + original[2:])
        rotated = QImage(draft.add_file(str(source)).path)
        self.assertEqual((rotated.width(), rotated.height()), (40, 80))
        clipboard = self.image()
        clipboard.setDevicePixelRatio(2.0)
        item = draft.add_image(clipboard)
        self.assertEqual(item.thumbnail.devicePixelRatio(), 1.0)
        self.assertEqual(clipboard.devicePixelRatio(), 2.0)

    def test_noisy_transparency_fits_storage_bound_and_write_failure_leaves_no_item(self):
        draft = self.draft()
        pixels = random.Random(7).randbytes(1024 * 1024 * 4)
        noisy = QImage(pixels, 1024, 1024, QImage.Format_RGBA8888)
        item = draft.add_image(noisy)
        self.assertLessEqual(Path(item.path).stat().st_size, 2 * 1024 * 1024)
        normalized = QImage(item.path)
        self.assertTrue(normalized.hasAlphaChannel())
        self.assertLess(normalized.width(), 1024)
        with patch.object(Path, "write_bytes", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                draft.add_image(self.image())
        self.assertEqual(draft.items, [item])
        self.assertEqual(list(Path(item.path).parent.iterdir()), [Path(item.path)])

    def test_multi_image_layout_keeps_every_thumbnail_inside_bubble(self):
        source = self.source(width=320, height=600)
        row = MessageRow({"id": "multi", "role": "user", "content": "查看这四张图片", "status": "complete",
                          "images": [{"id": str(i), "path": source, "name": "test.png"} for i in range(4)]})
        self.addCleanup(row.deleteLater)
        row.show()
        self.addCleanup(row.close)
        row.set_available_width(280)
        self.settle()
        self.assertEqual(len(row.image_grid.thumbnails), 4)
        for thumbnail in row.image_grid.thumbnails:
            self.assertLessEqual(thumbnail.geometry().right(), row.image_grid.width())
            self.assertLessEqual(thumbnail.geometry().bottom(), row.image_grid.height())
        self.assertLessEqual(row.image_grid.geometry().bottom(), row.bubble.height())

    def test_selected_files_preview_can_remove_without_sending(self):
        page, client = self.page()
        source = self.source()
        with patch("pages.chat_page.QFileDialog.getOpenFileNames", return_value=([source], "")):
            page.attach_button.click()
        self.assertEqual(len(page._image_draft.items), 1)
        self.assertFalse(page.image_tray.isHidden())
        self.assertEqual(client.sent, [])
        item = page._image_draft.items[0]
        page._remove_image(item.path)
        self.assertFalse(Path(item.path).exists())
        self.assertTrue(page.image_tray.isHidden())
        self.assertTrue(Path(source).exists())

    def test_file_selection_returns_focus_to_composer_for_enter_send(self):
        page, client = self.page()
        page.attach_button.setFocus()
        with patch("pages.chat_page.QFileDialog.getOpenFileNames", return_value=([self.source()], "")):
            page.attach_button.click()
        self.assertIs(QApplication.focusWidget(), page.input_edit)
        QTest.keyClick(QApplication.focusWidget(), Qt.Key_Return)
        self.assertEqual(len(client.sent), 1)
        self.assertEqual(client.sent[0]["text"], "")
        self.assertEqual(len(client.sent[0]["images"]), 1)

    def test_clipboard_pure_image_enter_and_inflight_edits(self):
        page, client = self.page()
        mime = QMimeData()
        mime.setImageData(self.image())
        page.input_edit.insertFromMimeData(mime)
        staged = page._image_draft.items[0].path
        QTest.keyClick(page.input_edit, Qt.Key_Return)
        self.assertEqual(client.sent, [{"text": "", "images": [{"path": staged, "name": "粘贴的图片.png"}]}])
        self.assertEqual(page.action_button.text(), "发送中…")
        self.assertFalse(page.attach_button.isEnabled())
        page._remove_image(staged)
        page.input_edit.insertFromMimeData(mime)
        self.assertEqual(len(page._image_draft.items), 1)
        page.input_edit.setPlainText("下一条消息")
        client.operationFinished.emit("send_message", {"messageId": "saved"})
        self.assertEqual(page.input_edit.toPlainText(), "下一条消息")
        self.assertFalse(Path(staged).exists())
        self.assertEqual(page._image_draft.items, [])
        self.assertFalse(page._send_inflight)

    def test_image_text_failure_and_disconnect_keep_draft_then_retry_cleans(self):
        page, client = self.page()
        page._add_image_files([self.source()])
        staged = page._image_draft.items[0].path
        page.input_edit.setPlainText("解释这张图")
        page._submit()
        client.operationFailed.emit("send_message", "磁盘空间不足")
        self.assertTrue(Path(staged).exists())
        self.assertEqual(page.input_edit.toPlainText(), "解释这张图")
        client.is_connected = False
        client.disconnected.emit()
        page.set_client(None)
        self.assertTrue(Path(staged).exists())
        replacement = ImageClient()
        page.set_client(replacement)
        page._poll_timer.stop()
        self.assertTrue(Path(staged).exists())
        page._submit()
        self.assertEqual(replacement.sent[-1]["text"], "解释这张图")
        replacement.operationFinished.emit("send_message", {"messageId": "saved"})
        self.assertFalse(Path(staged).exists())
        self.assertEqual(page.input_edit.toPlainText(), "")

    def test_profile_switch_clears_old_images_and_ignores_late_ack(self):
        page, client = self.page()
        page._add_image_files([self.source()])
        staged = page._image_draft.items[0].path
        page._submit()
        page._apply_state({**client.get_state(), "profileId": "other-profile", "revision": 3})
        self.assertFalse(Path(staged).exists())
        self.assertEqual(page._image_draft.items, [])
        page.input_edit.setPlainText("新角色的草稿")
        client.operationFinished.emit("send_message", {"messageId": "late"})
        self.assertEqual(page.input_edit.toPlainText(), "新角色的草稿")

    def test_image_drop_is_a_draft_and_normal_text_paste_unchanged(self):
        page, client = self.page()
        mime = QMimeData()
        mime.setUrls([QUrl.fromLocalFile(self.source())])
        drag = QDragEnterEvent(page.input_edit.rect().center(), Qt.CopyAction, mime, Qt.LeftButton, Qt.NoModifier)
        page.input_edit.dragEnterEvent(drag)
        self.assertTrue(drag.isAccepted())
        drop = QDropEvent(QPointF(10, 10), Qt.CopyAction, mime, Qt.LeftButton, Qt.NoModifier)
        page.input_edit.dropEvent(drop)
        self.assertTrue(drop.isAccepted())
        self.assertEqual(len(page._image_draft.items), 1)
        self.assertEqual(client.sent, [])
        plain = QMimeData()
        plain.setText("正常粘贴文字")
        page.input_edit.insertFromMimeData(plain)
        self.assertEqual(page.input_edit.toPlainText(), "正常粘贴文字")

    def test_image_history_is_visible_cached_and_handles_missing_file(self):
        page, client = self.page()
        source = self.source(width=1400, height=700)
        message = {"id": "image-history", "role": "user", "content": "", "status": "complete",
                   "images": [{"id": "saved-image", "path": source, "name": "合成.png", "mimeType": "image/png"}]}
        with patch("pages.chat_images.QImageReader", wraps=QImageReader) as reader:
            page._apply_state({**client.get_state(), "revision": 3, "messages": [message]})
            self.settle()
            row = page._rows["image-history"]
            self.assertTrue(row.text_label.isHidden())
            image = row.image_grid.thumbnails[0]
            self.assertLessEqual(max(image.thumbnail.width(), image.thumbnail.height()), 280)
            reads = reader.call_count
            row.update_message({**message, "timestamp": "2026-10-18T10:30:00Z"})
            row.set_available_width(300)
            self.assertEqual(reader.call_count, reads)
            self.assertLessEqual(row.bubble.width(), 246)
            self.assertGreater(row.height(), 80)
        with patch("pages.chat_images.ImagePreviewDialog.open") as opened:
            image.click()
            self.assertTrue(opened.called)
        Path(source).unlink()
        missing = MessageRow({**message, "id": "missing"})
        self.addCleanup(missing.deleteLater)
        self.assertIn("图片不可用", missing.image_grid.thumbnails[0].text())
        preview = ImagePreviewDialog(message["images"][0])
        self.addCleanup(preview.deleteLater)
        self.assertIn("图片不可用", preview.image_label.text())

    def test_retina_history_thumbnail_uses_bounded_physical_pixels_and_logical_layout(self):
        with patch.object(ImageThumbnail, "devicePixelRatioF", return_value=2.0):
            thumbnail = ImageThumbnail({"path": self.source(width=1400, height=700), "name": "retina.png"})
        self.addCleanup(thumbnail.deleteLater)
        self.assertEqual(thumbnail.thumbnail.devicePixelRatio(), 2.0)
        self.assertEqual(thumbnail.thumbnail.width(), 560)
        self.assertLessEqual(thumbnail.thumbnail.height(), 400)
        thumbnail.fit(280, 200)
        self.assertEqual((thumbnail.width(), thumbnail.height()), (280, 140))

    def test_large_preview_resizes_down_without_pixmap_minimum_lock(self):
        dialog = ImagePreviewDialog({"path": self.source(width=1200, height=800), "name": "合成图片"})
        self.addCleanup(dialog.close)
        dialog.show()
        self.settle()
        large_width = dialog.image_label.pixmap().width()
        dialog.resize(400, 300)
        self.settle()
        self.assertEqual(dialog.width(), 400)
        self.assertLess(dialog.image_label.pixmap().width(), large_width)
        self.assertLessEqual(dialog.image_label.pixmap().width(), dialog.image_label.width())
        self.assertLessEqual(dialog.image_label.pixmap().height(), dialog.image_label.height())

    def test_upward_image_history_preserves_anchor_and_search_location(self):
        page, client = self.page()
        source = self.source()
        image = {"id": "old-image", "role": "user", "content": "图片说明", "status": "complete",
                 "images": [{"id": "saved", "path": source, "name": "test.png", "mimeType": "image/png"}]}
        recent = [{"id": str(i), "role": "assistant", "content": "文字消息" * 30, "status": "complete"}
                  for i in range(40)]
        page._apply_state({**client.get_state(), "revision": 3, "messages": recent, "hasMoreBefore": True})
        self.settle()
        page.message_area.verticalScrollBar().setValue(0)
        anchor = page._capture_anchor()
        page._load_older()
        client.operationFinished.emit("get_chat_history", {
            "requestToken": client.history_calls[-1]["requestToken"], "messages": [image], "hasMoreBefore": False})
        self.settle()
        self.assertEqual(page._rows[anchor[0]].y() - page.message_area.verticalScrollBar().value(), anchor[1])
        self.assertIn("old-image", page._rows)
        page._locate_history_message("old-image")
        client.operationFinished.emit("get_chat_history", {
            "requestToken": client.history_calls[-1]["requestToken"], "messages": [image, recent[0]],
            "hasMoreBefore": False, "hasMoreAfter": True})
        self.settle()
        self.assertTrue(page._rows["old-image"].is_highlighted)
        self.assertEqual(len(page._rows["old-image"].image_grid.thumbnails), 1)


if __name__ == "__main__":
    unittest.main()
