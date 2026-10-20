"""Durable, account-bound inbox shared by command invocations and the source."""
import sys
sys.dont_write_bytecode = True

import json
from pathlib import Path
import re
import sqlite3
import uuid

if __package__:
    from .common import AdapterError, authorize_chat, json_text, normalize_event, validate_settings
else:
    from common import AdapterError, authorize_chat, json_text, normalize_event, validate_settings


class Journal:
    def __init__(self, settings):
        self.settings = validate_settings(settings)
        state = Path(self.settings["stateDir"])
        state.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(str(state / "journal.sqlite3"), timeout=5)
        try:
            self.db.execute("PRAGMA journal_mode=WAL")
            with self.db:
                self.db.execute("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)")
                self.db.execute("CREATE TABLE IF NOT EXISTS messages (seq INTEGER PRIMARY KEY AUTOINCREMENT, chat_id TEXT NOT NULL, remote_id TEXT NOT NULL, payload TEXT NOT NULL, UNIQUE(chat_id,remote_id))")
                self.db.execute("CREATE INDEX IF NOT EXISTS idx_messages_chat_seq ON messages(chat_id,seq)")
                self.db.execute("CREATE TABLE IF NOT EXISTS source_ack (singleton INTEGER PRIMARY KEY CHECK(singleton=1), event_id TEXT NOT NULL, cursor TEXT NOT NULL)")
                self.db.execute("INSERT OR IGNORE INTO meta VALUES('identity',?)", (json_text([self.settings["connectionKey"], self.settings["selfId"]]),))
                self.db.execute("INSERT OR IGNORE INTO meta VALUES('journal_id',?)", (uuid.uuid4().hex,))
                identity = self.db.execute("SELECT value FROM meta WHERE key='identity'").fetchone()[0]
                if identity != json_text([self.settings["connectionKey"], self.settings["selfId"]]):
                    raise AdapterError("ACCOUNT_MISMATCH", "This journal belongs to a different QQ account or connection; use a new stateDir")
                self.identifier = self.db.execute("SELECT value FROM meta WHERE key='journal_id'").fetchone()[0]
        except Exception:
            self.db.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        self.db.close()

    def cursor(self, seq):
        return "qq:" + self.identifier + ":" + str(seq)

    def event_id(self, seq):
        return "qq-event:" + self.identifier + ":" + str(seq)

    def cursor_number(self, cursor, chat_id=None):
        if cursor == "":
            return 0
        prefix = "qq:" + self.identifier + ":"
        if not isinstance(cursor, str) or not cursor.startswith(prefix) or not re.fullmatch(r"[1-9][0-9]{0,18}", cursor[len(prefix):]):
            raise AdapterError("INVALID_CURSOR", "Cursor does not belong to this QQ journal")
        seq = int(cursor[len(prefix):])
        if seq > 9223372036854775807:
            raise AdapterError("INVALID_CURSOR", "Cursor is outside the journal range")
        row = self.db.execute("SELECT chat_id FROM messages WHERE seq=?", (seq,)).fetchone()
        if not row or chat_id and row[0] != chat_id:
            raise AdapterError("INVALID_CURSOR", "Cursor is not present in this conversation or journal")
        return seq

    def item(self, row):
        value = json.loads(row[1])
        value.update(event_id=self.event_id(row[0]), sequence=str(row[0]), cursor=self.cursor(row[0]))
        return value

    def ingest(self, event, historical=False, requested_chat=None):
        value = normalize_event(self.settings, event, historical, requested_chat)
        if value is None:
            return None
        with self.db:
            # Check under the write transaction so duplicate imports do not
            # consume new sequence numbers or replace the original payload.
            self.db.execute("BEGIN IMMEDIATE")
            row = self.db.execute("SELECT seq,payload FROM messages WHERE chat_id=? AND remote_id=?", (value["chat_id"], value["message_id"])).fetchone()
            if row is None:
                cursor = self.db.execute("INSERT INTO messages(chat_id,remote_id,payload) VALUES(?,?,?)", (value["chat_id"], value["message_id"], json_text(value)))
                row = (cursor.lastrowid, json_text(value))
        return self.item(row)

    def read(self, chat_id, cursor="", limit=10):
        authorize_chat(self.settings, chat_id)
        if type(limit) is not int or not 1 <= limit <= 20:
            raise AdapterError("INVALID_INPUT", "limit must be an integer from 1 to 20")
        before = self.cursor_number(cursor, chat_id) if cursor else 9223372036854775807
        rows = self.db.execute("SELECT seq,payload FROM messages WHERE chat_id=? AND seq<? ORDER BY seq DESC LIMIT ?", (chat_id, before, limit + 1)).fetchall()
        selected = rows[:limit]
        return {"items": [self.item(row) for row in reversed(selected)],
                "next_cursor": self.cursor(selected[-1][0]) if len(rows) > limit else ""}

    def after(self, cursor, limit=64):
        seq = self.cursor_number(cursor)
        rooms = self.settings["allowedConversations"]
        rows = self.db.execute("SELECT seq,payload FROM messages WHERE seq>? AND chat_id IN (" + ",".join("?" for _ in rooms) + ") ORDER BY seq LIMIT ?", (seq, *rooms, limit)).fetchall()
        return [self.item(row) for row in rows]

    def ack(self, event_id, cursor):
        seq = self.cursor_number(cursor)
        if not seq or event_id != self.event_id(seq):
            raise AdapterError("INVALID_ACK", "ACK event and cursor do not identify the same journal message")
        with self.db:
            previous = self.db.execute("SELECT cursor FROM source_ack WHERE singleton=1").fetchone()
            if previous and self.cursor_number(previous[0]) >= seq:
                return
            self.db.execute("INSERT OR REPLACE INTO source_ack VALUES(1,?,?)", (event_id, cursor))
