"""Independent loopback LX fixture for the C++ command gateway integration tests."""
import argparse
from collections import Counter
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
import socket
import sqlite3
import sys
from urllib.parse import parse_qs, urlsplit


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        request = urlsplit(self.path)
        service = self.server
        service.counts[request.path] += 1
        service.count_path.write_text(json.dumps(service.counts), encoding="utf-8")
        mode = service.mode_path.read_text(encoding="utf-8") if service.mode_path.exists() else "normal"
        write = request.path in {"/play", "/pause", "/volume", "/skip-next", "/skip-prev"}
        if request.path == "/play":
            service.status["status"] = "playing"
        elif request.path == "/pause":
            service.status["status"] = "paused"
        elif request.path == "/volume":
            service.status["volume"] = int(parse_qs(request.query)["volume"][0])
        if mode == "disconnect" and write:
            self.connection.shutdown(socket.SHUT_RDWR)
            self.connection.close()
            return
        if request.path == "/status":
            value = service.status
        elif request.path == "/lyric":
            value = {"lyric": "[00:01]窗外的麻雀\n[00:02]在电线杆上多嘴"}
        else:
            value = {"ok": True}
        body = json.dumps(value, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", required=True)
    directory = Path(parser.parse_args().directory)
    database = directory / "fixture 歌单.db"
    with sqlite3.connect(database) as connection:
        connection.executescript('''
            CREATE TABLE my_list(id TEXT PRIMARY KEY, name TEXT, source TEXT, sourceListId TEXT, position INTEGER);
            CREATE TABLE my_list_music_info(id TEXT PRIMARY KEY, name TEXT, singer TEXT);
            CREATE TABLE my_list_music_info_order(listId TEXT, musicInfoId TEXT, "order" INTEGER);
            INSERT INTO my_list VALUES('cloud', '收藏歌曲', 'wy', '12345', 2);
            INSERT INTO my_list VALUES('empty', '空歌单', '', '', 1);
            INSERT INTO my_list_music_info VALUES('a', '晴天', '周杰伦');
            INSERT INTO my_list_music_info VALUES('b', '稻香', '周杰伦');
            INSERT INTO my_list_music_info_order VALUES('cloud', 'a', 2);
            INSERT INTO my_list_music_info_order VALUES('cloud', 'b', 1);
        ''')
    service = HTTPServer(("127.0.0.1", 0), Handler)
    service.counts = Counter()
    service.count_path = directory / "counts.json"
    service.count_path.write_text("{}", encoding="utf-8")
    service.mode_path = directory / "mode.txt"
    service.status = {"status": "paused", "name": "晴天", "singer": "周杰伦", "volume": 50}
    print(json.dumps({"apiUrl": "http://127.0.0.1:" + str(service.server_port),
                      "database": str(database)}, ensure_ascii=False), flush=True)
    service.serve_forever()


if __name__ == "__main__":
    main()
