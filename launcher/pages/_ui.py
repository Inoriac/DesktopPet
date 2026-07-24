"""demo.py 风格的页面/卡片布局助手。

对齐官方参考示例（launcher/demo.py）：
- 页面用 SingleDirectionScrollArea 承载卡片（DetailInterface 风格）；
- 分区用 HeaderCardWidget（带标题卡片），内部以行形式罗列设置项；
- 行样式参考 AppInfoCard 的左标题/右控件排布。

各设置页只负责构造控件并绑定 AppState，布局细节收口于此。
"""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor, QFont
from PySide6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QSizePolicy
from qfluentwidgets import (
    SingleDirectionScrollArea, HeaderCardWidget, StrongBodyLabel,
    CaptionLabel, setFont,
)

# CaptionLabel 副标题灰度色（亮/暗主题各一，对齐 demo StatisticsWidget 配色）
_CAPTION_LIGHT = QColor(96, 96, 96)
_CAPTION_DARK = QColor(206, 206, 206)


class ScrollPage(SingleDirectionScrollArea):
    """demo.DetailInterface 风格：单向滚动 + 透明背景 + 卡片顶部对齐。

    即便内容一屏放得下，也保证可滚动 + 平滑惯性：底部预留一段「弹性空白」，
    其高度随视口自适应（= 视口高的 60%），使内容总是高于视口 → 滚动范围恒为正；
    配合库自带 SmoothScroll 动画，滚轮得「能拉下去、松手平滑回位」的动量手感。
    """

    # 弹性空白相对视口高的比例 + 保底下限，保证短页也能拉
    _TAIL_RATIO = 0.6
    _TAIL_MIN = 200

    def __init__(self, object_name: str, parent=None):
        super().__init__(parent=None)
        self.view = QWidget(self)
        self.vBoxLayout = QVBoxLayout(self.view)

        self.setWidget(self.view)
        self.setWidgetResizable(True)
        self.setObjectName(object_name)

        self.vBoxLayout.setSpacing(10)
        self.vBoxLayout.setContentsMargins(36, 20, 36, 20)

        self.setStyleSheet("QScrollArea {border: none; background:transparent}")
        self.view.setStyleSheet("QWidget {background:transparent}")

        # 平滑动量滚动（库自带 SmoothScroll 动画，越界由其 clamp 处理）
        try:
            self.setScrollAnimation(Qt.Vertical, 400)  # type: ignore[attr-defined]
        except Exception:
            pass

        # 末尾占位：一个固定 spacing 项，其尺寸随视口在 resizeEvent 里调整，
        # 使内容恒高于视口（widgetResizable=True 下 stretch 只会吸收多余空间、
        # 不增大尺寸，故必须用 addSpacing 的固定像素顶出滚动范围）。
        self._tailIndex = self.vBoxLayout.count()
        self.vBoxLayout.addSpacing(self._TAIL_MIN)

    def addStretch(self):  # noqa: D401  保留各页既有调用（已由末尾占位替代）
        pass

    def addCard(self, card: QWidget, stretch: int = 0):
        # 插在末尾占位之前，保持「卡片们 + 尾部弹性空白」的顺序
        self.vBoxLayout.insertWidget(self._tailIndex, card, stretch, Qt.AlignTop)
        self._tailIndex = self.vBoxLayout.count() - 1
        return card

    def resizeEvent(self, e):
        super().resizeEvent(e)
        self._adjust_tail()

    def showEvent(self, e):
        super().showEvent(e)
        self._adjust_tail()

    def _adjust_tail(self):
        """按视口高度刷新尾部弹性空白，保证内容 > 视口 → 可滚动。"""
        vh = self.viewport().height()
        if vh <= 0:
            return
        size = max(self._TAIL_MIN, int(vh * self._TAIL_RATIO))
        item = self.vBoxLayout.itemAt(self._tailIndex)
        if item is not None:
            item.changeSize(0, size, QSizePolicy.Fixed, QSizePolicy.Fixed)
        self.vBoxLayout.invalidate()


class Section(HeaderCardWidget):
    """带标题的分区卡片；viewLayout 内嵌纵向 bodyLayout 放置若干设置行。

    HeaderCardWidget.viewLayout 为 QHBoxLayout（单一主体），故内部再用
    QVBoxLayout 容器堆叠多行，渲染上等同于 demo 中卡片内的纵向布局。
    """

    def __init__(self, title: str, parent=None):
        super().__init__(parent)
        self.setTitle(title)

        self._body = QWidget(self)
        self.bodyLayout = QVBoxLayout(self._body)
        self.bodyLayout.setContentsMargins(0, 0, 0, 0)
        self.bodyLayout.setSpacing(14)
        self.viewLayout.addWidget(self._body)
        self.viewLayout.setContentsMargins(24, 0, 24, 20)

    def addRow(self, title: str, content: str, control: QWidget) -> QWidget:
        row = make_row(title, content, control)
        self.bodyLayout.addWidget(row)
        return row

    def addWidget(self, widget: QWidget):
        """直接挂一个自定义控件（如整段说明）到分区主体。"""
        self.bodyLayout.addWidget(widget)


def make_row(title: str, content: str, control: QWidget) -> QWidget:
    """左：标题 + 副标题；右：控件。参考 demo AppInfoCard 的左文右控件排布。"""
    row = QWidget()
    h = QHBoxLayout(row)
    h.setContentsMargins(0, 0, 0, 0)
    h.setSpacing(16)

    left = QWidget()
    vl = QVBoxLayout(left)
    vl.setContentsMargins(0, 0, 0, 0)
    vl.setSpacing(2)

    titleLabel = StrongBodyLabel(title, left)
    vl.addWidget(titleLabel)

    if content:
        caption = CaptionLabel(content, left)
        caption.setTextColor(_CAPTION_LIGHT, _CAPTION_DARK)
        vl.addWidget(caption)

    h.addWidget(left)
    h.addStretch(1)
    h.addWidget(control, 0, Qt.AlignRight | Qt.AlignVCenter)
    return row


def titled_label(text: str, size: int = 20, weight=QFont.DemiBold) -> "CaptionLabel":
    """构造一个加粗标题标签（用于页首大标题等）。"""
    from qfluentwidgets import BodyLabel
    lbl = BodyLabel(text)
    setFont(lbl, size, weight)
    return lbl