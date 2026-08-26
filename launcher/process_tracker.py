"""Track desktop-pet processes started by the current launcher session."""

from __future__ import annotations

from typing import Protocol


class PollableProcess(Protocol):
    def poll(self) -> int | None:
        """Return None while the process is running, otherwise its exit code."""


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
