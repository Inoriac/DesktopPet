"""Local image drafts and bounded previews for launcher chat."""
from __future__ import annotations

from dataclasses import dataclass
from html import escape
from pathlib import Path
import tempfile
import uuid

from PySide6.QtCore import QBuffer, QIODevice, QSize, Qt
from PySide6.QtGui import QIcon, QImage, QImageReader, QPixmap
from PySide6.QtWidgets import QDialog, QGridLayout, QLabel, QPushButton, QSizePolicy, QVBoxLayout, QWidget
from qfluentwidgets import isDarkTheme

MAX_IMAGES = 4
MAX_SOURCE_BYTES = 20 * 1024 * 1024
MAX_PIXELS = 40_000_000
MAX_SENT_BYTES = 2 * 1024 * 1024
MAX_EDGE = 2048


@dataclass
class DraftImage:
    path: str
    name: str
    thumbnail: QPixmap

    def source(self) -> dict:
        return {"path": self.path, "name": self.name}


def _read_image(path: str, bounds: QSize) -> QImage:
    reader = QImageReader(path)
    reader.setAutoTransform(True)
    size = reader.size()
    if not reader.canRead() or not size.isValid():
        raise ValueError("无法读取图片，请选择有效的图片文件。")
    if size.width() * size.height() > MAX_PIXELS:
        raise ValueError("图片像素过多，请选择不超过 4000 万像素的图片。")
    if size.width() > bounds.width() or size.height() > bounds.height():
        reader.setScaledSize(size.scaled(bounds, Qt.KeepAspectRatio))
    image = reader.read()
    if image.isNull():
        raise ValueError("无法读取图片，请选择有效的图片文件。")
    if image.width() > bounds.width() or image.height() > bounds.height():
        image = image.scaled(bounds, Qt.KeepAspectRatio, Qt.SmoothTransformation)
    return image


class ChatImageDraft:
    def __init__(self):
        self.items: list[DraftImage] = []
        self._directory = None

    def _check_count(self) -> None:
        if len(self.items) >= MAX_IMAGES:
            raise ValueError("每条消息最多发送 4 张图片。")

    def add_file(self, path: str) -> DraftImage:
        self._check_count()
        source = Path(path)
        if not source.is_file():
            raise ValueError("请选择本地图片文件。")
        if source.stat().st_size > MAX_SOURCE_BYTES:
            raise ValueError("原图片不能超过 20 MiB，请缩小后再试。")
        return self.add_image(_read_image(str(source), QSize(MAX_EDGE, MAX_EDGE)), source.name)

    def add_image(self, image: QImage, name: str = "粘贴的图片.png") -> DraftImage:
        self._check_count()
        if image.isNull():
            raise ValueError("剪贴板中没有可读取的图片。")
        if image.width() * image.height() > MAX_PIXELS:
            raise ValueError("图片像素过多，请选择不超过 4000 万像素的图片。")
        image = QImage(image)
        image.setDevicePixelRatio(1.0)
        if max(image.width(), image.height()) > MAX_EDGE:
            image = image.scaled(MAX_EDGE, MAX_EDGE, Qt.KeepAspectRatio, Qt.SmoothTransformation)
        # Keep transparency. Noisy PNGs may need a smaller sent version to fit
        # the same per-image transport/storage bound as JPEGs.
        format_name = "PNG" if image.hasAlphaChannel() else "JPEG"
        quality = 88
        while True:
            buffer = QBuffer()
            buffer.open(QIODevice.WriteOnly)
            if not image.save(buffer, format_name, quality):
                raise ValueError("图片转换失败，请换一张图片重试。")
            encoded = bytes(buffer.data())
            if len(encoded) <= MAX_SENT_BYTES:
                break
            if format_name == "JPEG" and quality > 64:
                quality -= 12
            else:
                image = image.scaled(max(1, int(image.width() * 0.8)), max(1, int(image.height() * 0.8)),
                                     Qt.KeepAspectRatio, Qt.SmoothTransformation)
        if self._directory is None:
            self._directory = tempfile.TemporaryDirectory(prefix="desktop-pet-chat-images-")
        path = Path(self._directory.name) / f"{uuid.uuid4().hex}.{'png' if format_name == 'PNG' else 'jpg'}"
        try:
            path.write_bytes(encoded)
        except OSError:
            path.unlink(missing_ok=True)
            raise
        thumbnail = QPixmap.fromImage(image.scaled(160, 110, Qt.KeepAspectRatio, Qt.SmoothTransformation))
        item = DraftImage(str(path), Path(name).name, thumbnail)
        self.items.append(item)
        return item

    def remove(self, path: str) -> None:
        for item in list(self.items):
            if item.path == path:
                Path(item.path).unlink(missing_ok=True)
                self.items.remove(item)

    def clear(self) -> None:
        self.items.clear()
        if self._directory is not None:
            self._directory.cleanup()
            self._directory = None


class ImagePreviewDialog(QDialog):
    def __init__(self, metadata: dict, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WA_DeleteOnClose)
        self.setWindowTitle(str(metadata.get("name") or "图片"))
        self.resize(900, 650)
        self.setMinimumSize(280, 220)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(16, 16, 16, 16)
        self.image_label = QLabel(self)
        self.image_label.setAlignment(Qt.AlignCenter)
        self.image_label.setTextFormat(Qt.PlainText)
        self.image_label.setMinimumSize(1, 1)
        self.image_label.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Ignored)
        layout.addWidget(self.image_label, 1)
        self._image = QPixmap()
        path = str(metadata.get("path") or "")
        try:
            self._image = QPixmap.fromImage(_read_image(path, QSize(MAX_EDGE, MAX_EDGE)))
        except (OSError, ValueError):
            self.image_label.setText("图片不可用，文件可能已被移除。")
        self.refresh_theme()
        self._fit_image()

    def refresh_theme(self) -> None:
        background, foreground = ("#22262E", "#F0F2F5") if isDarkTheme() else ("#F3F5F8", "#262B35")
        self.setStyleSheet(f"QDialog {{ background: {background}; }}"
                          f"QLabel {{ color: {foreground}; background: transparent; }}")

    def _fit_image(self) -> None:
        if not self._image.isNull():
            ratio = self.devicePixelRatioF()
            size = self.image_label.size() * ratio
            pixmap = self._image.scaled(size, Qt.KeepAspectRatio, Qt.SmoothTransformation)
            pixmap.setDevicePixelRatio(ratio)
            self.image_label.setPixmap(pixmap)

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self._fit_image()


class ImageThumbnail(QPushButton):
    def __init__(self, metadata: dict, parent=None):
        super().__init__(parent)
        self.metadata = dict(metadata)
        self.thumbnail = QPixmap()
        self._preview = None
        name = str(metadata.get("name") or "图片")
        self.setToolTip(f"<qt>{escape(name)}<br>点击查看大图</qt>")
        self.setAccessibleName(f"查看图片：{name}")
        self.setCursor(Qt.PointingHandCursor)
        self.refresh_theme()
        try:
            ratio = self.devicePixelRatioF()
            self.thumbnail = QPixmap.fromImage(_read_image(
                str(metadata.get("path") or ""), QSize(280, 200) * ratio))
            self.thumbnail.setDevicePixelRatio(ratio)
        except (ValueError, OSError):
            self.setText("图片不可用")
        if not self.thumbnail.isNull():
            self.setIcon(QIcon(self.thumbnail))
        self.clicked.connect(self._open_preview)

    def refresh_theme(self, foreground: str = "") -> None:
        foreground = foreground or ("#F0F2F5" if isDarkTheme() else "#262B35")
        self.setStyleSheet("QPushButton { border: none; border-radius: 6px;"
                          f"color: {foreground}; background: rgba(128,128,128,0.12); }}")
        if self._preview is not None:
            self._preview.refresh_theme()

    def fit(self, width: int, max_height: int) -> int:
        size = self.thumbnail.deviceIndependentSize().toSize() if not self.thumbnail.isNull() else QSize(180, 100)
        size = size.scaled(max(24, width), max_height, Qt.KeepAspectRatio)
        self.setIconSize(size)
        self.setFixedSize(size)
        return size.height()

    def _open_preview(self) -> None:
        if self._preview is None:
            self._preview = ImagePreviewDialog(self.metadata, self.window())
            self._preview.finished.connect(lambda _result: setattr(self, "_preview", None))
        self._preview.open()


class MessageImageGrid(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.images = []
        self.thumbnails: list[ImageThumbnail] = []
        self.grid = QGridLayout(self)
        self.grid.setContentsMargins(0, 0, 0, 0)
        self.grid.setSpacing(6)
        self.hide()

    def set_images(self, images: list[dict]) -> None:
        if images == self.images:
            return
        self.images = [dict(image) for image in images]
        for thumbnail in self.thumbnails:
            self.grid.removeWidget(thumbnail)
            thumbnail.hide()
            thumbnail.deleteLater()
        self.thumbnails = [ImageThumbnail(image, self) for image in self.images]
        columns = 1 if len(self.thumbnails) == 1 else 2
        for index, thumbnail in enumerate(self.thumbnails):
            self.grid.addWidget(thumbnail, index // columns, index % columns, Qt.AlignLeft | Qt.AlignTop)
        self.setVisible(bool(images))

    def fit(self, width: int) -> int:
        if not self.thumbnails:
            return 0
        columns = 1 if len(self.thumbnails) == 1 else 2
        cell_width = max(24, (width - (columns - 1) * 6) // columns)
        heights = []
        for index, thumbnail in enumerate(self.thumbnails):
            row = index // columns
            height = thumbnail.fit(cell_width, 200 if columns == 1 else 150)
            if len(heights) <= row:
                heights.append(0)
            heights[row] = max(heights[row], height)
        height = sum(heights) + (len(heights) - 1) * 6
        self.setFixedHeight(height)
        return height
