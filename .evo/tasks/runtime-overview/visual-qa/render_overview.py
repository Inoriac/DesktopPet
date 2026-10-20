"""Synthetic launcher runtime dashboard QA. No real profile, core, or model.

Run: launcher/.venv/bin/python .evo/tasks/runtime-overview/visual-qa/render_overview.py
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import sys
import tempfile
from unittest.mock import patch

NATIVE = "--native" in sys.argv
if not NATIVE:
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = Path(__file__).resolve().parents[4]
sys.path[:0] = [str(ROOT / "launcher"), str(ROOT / "tests")]

from PySide6.QtTest import QTest
from PySide6.QtCore import QPoint
from PySide6.QtWidgets import QApplication
from qfluentwidgets import Theme, setTheme

from main import LauncherWindow
from pet_registry import PetEntry
from test_launcher_statistics import PROFILE, OverviewClient, sample_snapshot
from pages.statistics_page import StatisticsPage
if NATIVE:
    os.environ.pop("QT_QPA_PLATFORM", None)


def main():
    app = QApplication.instance() or QApplication(["synthetic-runtime-overview"])
    app.setOrganizationName("DesktopPetRuntimeQA")
    app.setApplicationName("RuntimeQA")
    output = Path(__file__).resolve().parent
    evidence = []
    with tempfile.TemporaryDirectory(prefix="desktop-pet-runtime-qa-") as directory, \
            patch("statistics_data._app_data_dir", return_value=directory), \
            patch("main.config_loader.load_effective_config", return_value={}), \
            patch("pages.pet_page.load_pets", return_value=[PetEntry("小晴（合成数据）", "", PROFILE)]), \
            patch("main.PetProcessTracker") as tracker, \
            patch.object(LauncherWindow, "_read_theme", return_value="light"):
        tracker.return_value.refresh.return_value = 0
        if NATIVE:
            shell = LauncherWindow()
            shell._pet_process_timer.stop()
            page = shell.statistics_page
            shell.show()
            shell.switchTo(page)
        else:
            page = shell = StatisticsPage()
            page.set_profile(PROFILE, "小晴（合成数据）")
            page.show()
        client = OverviewClient()
        snapshot = sample_snapshot()
        page.set_client(client, PROFILE)
        client.overviewReceived.emit(snapshot)
        scenarios = [
            ("light-overview", Theme.LIGHT, 1100, 920, "online", False),
            ("dark-overview", Theme.DARK, 1100, 920, "online", False),
            ("narrow-overview", Theme.LIGHT, 760 if NATIVE else 640, 880, "online", False),
            ("light-records", Theme.LIGHT, 1100, 920, "online", True),
            ("dark-records", Theme.DARK, 1100, 920, "online", True),
            ("narrow-records", Theme.LIGHT, 760 if NATIVE else 640, 880, "online", True),
            ("offline-overview", Theme.LIGHT, 1100, 920, "offline", False),
            ("empty-overview", Theme.LIGHT, 1100, 920, "empty", False),
        ]
        for name, theme, width, height, mode, records in scenarios:
            setTheme(theme)
            page.refresh_theme()
            shell.resize(width, height)
            if not NATIVE:
                page.viewport().setStyleSheet("background: #20242a;" if theme == Theme.DARK
                                             else "background: #f3f6f9;")
            if mode == "offline":
                page.set_client(None, "")
            elif mode == "empty":
                page.set_profile("22222222-2222-4222-8222-222222222222", "新角色（合成数据）")
            QTest.qWait(80)
            page._poll_timer.stop()
            page.verticalScrollBar().setValue(0)
            if records:
                page.verticalScrollBar().setValue(page.role_filter.mapTo(page.view, QPoint()).y() - 100)
            QTest.qWait(80)
            path = output / f"{'native-' if NATIVE else ''}{name}.png"
            assert shell.grab().save(str(path))
            evidence.append({"file": path.name, "theme": theme.value, "mode": mode,
                             "window": [shell.width(), shell.height()],
                             "pageWidth": page.width(), "horizontalOverflow":
                             page.view.width() > page.viewport().width(),
                             "live": page._live, "recentRows": page.recent_table.rowCount()})
        shell.close()
        app.processEvents()
    (output / ("native-evidence.json" if NATIVE else "evidence.json")).write_text(
        json.dumps(evidence, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(evidence, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
