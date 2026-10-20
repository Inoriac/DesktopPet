"""Local OneBot test server; never connects to QQ or a third-party service."""
import sys
sys.dont_write_bytecode = True

import argparse
import json
from pathlib import Path
import threading
import time

from websockets.sync.server import serve
from websockets.exceptions import ConnectionClosed


def message(message_id="m1", chat_id="group:123", actor="456", text="你好🙂", timestamp=1770000000):
    kind, target = chat_id.split(":")
    value = {"post_type": "message", "message_type": kind, "message_id": message_id,
             "self_id": 999, "user_id": int(actor), "sender": {"user_id": int(actor), "nickname": "朋友"},
             "message": [{"type": "text", "data": {"text": text}}]}
    value["group_id" if kind == "group" else "target_id"] = int(target)
    if timestamp is not None:
        value["time"] = timestamp
    return value


class FakeOneBot:
    def __init__(self, self_id="999", history=None, events=None, unsupported=None, action_log=None):
        self.self_id = self_id
        self.history = history or {}
        self.events = events or []
        self.unsupported = set(unsupported or [])
        self.calls = []
        self.headers = []
        self.clients = set()
        self.lock = threading.Lock()
        self.hook = None
        self.action_log = action_log

    def __enter__(self):
        self.server = serve(self.handle, "127.0.0.1", 0)
        self.port = self.server.socket.getsockname()[1]
        self.url = "ws://127.0.0.1:%d" % self.port
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.server.shutdown()
        with self.lock:
            clients = list(self.clients)
        for client in clients:
            client.close()
        self.thread.join(timeout=3)

    def handle(self, ws):
        with self.lock:
            self.clients.add(ws)
            self.headers.append(dict(ws.request.headers))
        try:
            for raw in ws:
                request = json.loads(raw)
                with self.lock:
                    self.calls.append(request)
                    if self.action_log:
                        with open(self.action_log, "a", encoding="utf-8") as output:
                            output.write(json.dumps(request, ensure_ascii=False) + "\n")
                if self.hook and self.hook(ws, request):
                    continue
                action, params = request["action"], request.get("params", {})
                if action in self.unsupported:
                    ws.send(json.dumps({"echo": request["echo"], "status": "failed", "retcode": 1404}))
                    continue
                if action == "get_login_info":
                    data = {"user_id": int(self.self_id), "nickname": "测试账号"}
                elif action in ("get_group_msg_history", "get_friend_msg_history"):
                    chat = ("group:" + str(params["group_id"])) if "group_id" in params else ("private:" + str(params["user_id"]))
                    data = {"messages": self.history.get(chat, [])}
                else:
                    data = {}
                ws.send(json.dumps({"echo": request["echo"], "status": "ok", "retcode": 0, "data": data}))
                if action == "get_login_info":
                    for event in self.events:
                        ws.send(json.dumps(event, ensure_ascii=False))
        except ConnectionClosed:
            pass
        finally:
            with self.lock:
                self.clients.discard(ws)

    def emit(self, event):
        with self.lock:
            clients = list(self.clients)
        for ws in clients:
            try:
                ws.send(json.dumps(event, ensure_ascii=False))
            except ConnectionClosed:
                pass

    def disconnect(self):
        with self.lock:
            clients = list(self.clients)
        for ws in clients:
            ws.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ready-file", required=True)
    parser.add_argument("--scenario")
    parser.add_argument("--action-log")
    options = parser.parse_args()
    scenario = json.loads(Path(options.scenario).read_text(encoding="utf-8")) if options.scenario else {}
    with FakeOneBot(action_log=options.action_log, **scenario) as backend:
        Path(options.ready_file).write_text(json.dumps({"wsUrl": backend.url}), encoding="utf-8")
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
