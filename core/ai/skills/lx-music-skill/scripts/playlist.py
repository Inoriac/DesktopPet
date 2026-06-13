import sqlite3, sys, json, os
from urllib.parse import parse_qs, urlparse

if sys.stdout.encoding != "utf-8":
    import io

    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")


def get_lx_data_path():
    if sys.platform == "win32":
        base = os.environ.get("APPDATA", os.path.expanduser("~/AppData/Roaming"))
    elif sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Application Support")
    else:
        base = os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config"))
        if not os.path.exists(os.path.join(base, "lx-music-desktop")):
            base = os.path.expanduser("~/.config")
    return os.path.join(base, "lx-music-desktop", "LxDatas", "lx.data.db")


LX_DATA = get_lx_data_path()


def normalize_scheme_id(source, source_id):
    if not source_id:
        return None
    sid = str(source_id).strip()
    parsed = urlparse(sid)
    if parsed.scheme in ("http", "https"):
        query = parse_qs(parsed.query)
        if source in ("wy", "tx") and query.get("id"):
            return query["id"][0]
        if query.get("playlistId"):
            return query["playlistId"][0]
    return sid


def build_playlist_url(source, source_id):
    sid = normalize_scheme_id(source, source_id)
    if not source or not sid:
        return None
    return f"lxmusic://songlist/play/{source}/{sid}"


def get_playlists():
    conn = sqlite3.connect(LX_DATA)
    conn.text_factory = str
    c = conn.cursor()
    c.execute("SELECT id, name, source, sourceListId FROM my_list ORDER BY position")
    result = []
    for row in c.fetchall():
        result.append({
            "id": row[0],
            "name": row[1],
            "source": row[2],
            "sourceId": row[3],
            "playUrl": build_playlist_url(row[2], row[3]),
        })
    conn.close()
    return result


def get_playlist_songs(list_id):
    conn = sqlite3.connect(LX_DATA)
    conn.text_factory = str
    c = conn.cursor()
    c.execute(
        f'SELECT m.name, m.singer FROM my_list_music_info_order o JOIN my_list_music_info m ON o.musicInfoId = m.id WHERE o.listId = ? ORDER BY o."order"',
        (list_id,),
    )
    result = [{"name": r[0], "singer": r[1]} for r in c.fetchall()]
    conn.close()
    return result


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(json.dumps(get_playlists(), ensure_ascii=False, indent=2))
    elif sys.argv[1] == "songs" and len(sys.argv) >= 3:
        print(json.dumps(get_playlist_songs(sys.argv[2]), ensure_ascii=False, indent=2))
    elif sys.argv[1] in ("url", "play") and len(sys.argv) >= 3:
        list_id = sys.argv[2]
        conn = sqlite3.connect(LX_DATA)
        conn.text_factory = str
        c = conn.cursor()
        c.execute("SELECT source, sourceListId FROM my_list WHERE id = ?", (list_id,))
        row = c.fetchone()
        conn.close()
        if row:
            source, sid = row
            url = build_playlist_url(source, sid)
            if url:
                if sys.argv[1] == "play" and sys.platform == "win32":
                    os.startfile(url)
                    print(json.dumps({"opened": True, "url": url}, ensure_ascii=False))
                elif sys.argv[1] == "play":
                    print(json.dumps({"opened": False, "url": url, "error": "play is only supported on Windows"}, ensure_ascii=False))
                else:
                    print(json.dumps({"url": url}, ensure_ascii=False))
            else:
                print("No sourceId for this playlist")
        else:
            print("Playlist not found:", list_id)
