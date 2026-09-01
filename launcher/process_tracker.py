"""Track desktop-pet processes started by the current launcher session."""

from __future__ import annotations

import subprocess
import time
from typing import Protocol


class PollableProcess(Protocol):
    def poll(self) -> int | None:
        """Return None while the process is running, otherwise its exit code."""

    def terminate(self) -> None:
        """Ask the process to exit."""

    def wait(self, timeout: float | None = None) -> int:
        """Wait for process completion."""

    def kill(self) -> None:
        """Force process completion."""


class PetProcessTracker:
    def __init__(self) -> None:
        self._processes: list[PollableProcess] = []

    def add(self, process: PollableProcess) -> int:
        self._processes.append(process)
        return self.refresh()

    def refresh(self) -> int:
        running: list[PollableProcess] = []
        for process in self._processes:
            try:
                if process.poll() is None:
                    running.append(process)
            except (OSError, ValueError):
                continue
        self._processes = running
        return len(running)

    def terminate_all(self, timeout_seconds: float = 1.5) -> None:
        processes = list(self._processes)
        self._processes.clear()
        for process in processes:
            try:
                if process.poll() is None:
                    process.terminate()
            except (OSError, ValueError):
                continue
        deadline = time.monotonic() + max(0.0, float(timeout_seconds))
        for process in processes:
            try:
                if process.poll() is not None:
                    continue
                process.wait(timeout=max(0.0, deadline - time.monotonic()))
            except (TimeoutError, subprocess.TimeoutExpired):
                try:
                    process.kill()
                    process.wait(timeout=0.5)
                except (OSError, TimeoutError, subprocess.TimeoutExpired, ValueError):
                    pass
            except (OSError, ValueError):
                pass
