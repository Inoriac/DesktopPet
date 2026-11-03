"""Atomic file writes that fail fast instead of spinning on denied paths.

``tempfile.mkstemp(dir=...)`` retries up to 10,000 times on Windows whenever
``os.open`` raises ``PermissionError`` but ``os.access(dir, W_OK)`` still
reports the directory as writable (CPython's "name collision" heuristic). A
sandboxed or ACL-restricted AppData directory therefore blocks the launcher's
UI thread for minutes. This helper keeps the atomic write but propagates
permission errors immediately.
"""

from __future__ import annotations

import os
import secrets

_MAX_ATTEMPTS = 100
_FILE_MODE = 0o600


def atomic_write_text(
    path: str,
    text: str,
    *,
    prefix: str = ".tmp-",
    encoding: str = "utf-8",
) -> None:
    """Write ``text`` to ``path`` atomically, raising on the first OS error."""
    directory = os.path.dirname(os.path.abspath(path))
    payload = text.encode(encoding)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0)

    for _ in range(_MAX_ATTEMPTS):
        temporary_path = os.path.join(
            directory, f"{prefix}{secrets.token_hex(8)}.tmp")
        try:
            descriptor = os.open(temporary_path, flags, _FILE_MODE)
        except FileExistsError:
            continue
        try:
            with os.fdopen(descriptor, "wb") as handle:
                handle.write(payload)
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(temporary_path, path)
        except BaseException:
            try:
                os.unlink(temporary_path)
            except OSError:
                pass
            raise
        return

    raise FileExistsError(
        f"could not create a temporary file in {directory!r}")
