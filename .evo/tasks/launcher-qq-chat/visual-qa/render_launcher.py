"""Show the real launcher shell with synthetic chat and capture its chat page.

Run with the native Qt platform (cocoa on macOS), not offscreen: the existing
frameless window library needs a native window handle. Launcher configuration
is read to construct the settings pages; no configuration is saved. Process
tracking is replaced and no pet, model, or real chat service is started.
"""

from pathlib import Path
import sys
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "launcher"))

from PySide6.QtCore import Qt
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication
from qfluentwidgets import Theme, setTheme

from main import LauncherWindow
from render_chat import ArchiveClient


def main():
    app = QApplication.instance() or QApplication(["launcher-shell-chat-qa"])
    app.setOrganizationName("DesktopPetChatQA")
    app.setApplicationName("LauncherChatQA")
    with patch("main.PetProcessTracker") as tracker:
        tracker.return_value.refresh.return_value = 0
        window = LauncherWindow()
        window._pet_process_timer.stop()
        try:
            setTheme(Theme.LIGHT)
            page = window.chat_page
            page.refresh_theme()
            window.resize(1100, 800)
            window.show()
            window.switchTo(page)
            client = ArchiveClient()
            page.set_client(client)
            page._poll_timer.stop()
            QTest.qWait(300)
            page.open_history_search()
            page.search_input.setText("旅行清单")
            page._search_history()
            QTest.qWait(200)
            page.search_more_button.click()
            QTest.qWait(200)
            match = next(page.search_results.item(i) for i in range(page.search_results.count())
                         if page.search_results.item(i).data(Qt.UserRole) == "24")
            page.search_results.setCurrentItem(match)
            page.search_results.scrollToItem(match)
            page.search_results.itemClicked.emit(match)
            QTest.qWait(200)
            target = page._rows["24"]
            offset = target.y() - page.message_area.verticalScrollBar().value()
            assert 0 <= offset < page.message_area.viewport().height()
            assert page.message_area.horizontalScrollBar().maximum() == 0
            output = Path(__file__).resolve().parent / "Launcher-Light-1100.png"
            assert window.grab().save(str(output))
            print("PASS: native launcher shell, search pagination, visible selected match;", output)
        finally:
            window.close()


if __name__ == "__main__":
    main()
