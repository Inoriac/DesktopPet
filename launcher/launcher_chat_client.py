"""Authenticated Launcher client for the C++ chat bridge."""

from __future__ import annotations

from typing import Callable

from PySide6.QtNetwork import QLocalSocket

from owner_diary_client import (
    OwnerDiaryClient,
    OwnerDiaryError,
    OwnerDiaryOfflineError,
    OwnerDiaryProtocolError,
)


LauncherChatError = OwnerDiaryError
LauncherChatOfflineError = OwnerDiaryOfflineError
LauncherChatProtocolError = OwnerDiaryProtocolError


class LauncherChatClient(OwnerDiaryClient):
    """Small typed facade over the shared authenticated local transport."""

    def __init__(
        self,
        socket_factory: Callable[[], QLocalSocket] = QLocalSocket,
        timeout_ms: int = 250,
        max_frame_bytes: int = 1024 * 1024,
    ):
        super().__init__(socket_factory, timeout_ms, max_frame_bytes)

    def get_state(self, after_revision: int = -1) -> dict:
        return self._chat_exchange("get_chat_state", {
            "afterRevision": int(after_revision),
        })

    def send_message(self, text: str) -> dict:
        return self._chat_exchange("send_message", {"text": str(text)})

    def retry_message(self, message_id: str) -> dict:
        return self._chat_exchange(
            "retry_message", {"messageId": str(message_id)})

    def stop_response(self) -> dict:
        return self._chat_exchange("stop_response", {})

    def _chat_exchange(self, action: str, payload: dict) -> dict:
        try:
            return self._authenticated_exchange(action, payload)
        except OwnerDiaryProtocolError as error:
            if error.code == "CHAT_AUTH_FAILED":
                self.close()
            raise
