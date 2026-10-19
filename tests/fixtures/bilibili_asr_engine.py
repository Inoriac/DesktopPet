"""Install import-level ASR doubles for real, fixed production worker processes."""
import json
from pathlib import Path


ENGINE = r'''
import json
from pathlib import Path
import time
from types import SimpleNamespace
def config():
    return json.loads((Path(__file__).parent.parent / 'behavior.json').read_text(encoding='utf-8'))
class WhisperModel:
    def __init__(self, model, **kwargs):
        assert kwargs == {'device':'cpu', 'compute_type':'int8', 'local_files_only':True}
        if config().get('engineError'):
            raise RuntimeError('secret-cookie https://signed.example/audio?key=secret')
    def transcribe(self, audio, **kwargs):
        assert Path(audio).is_file()
        value = config()
        time.sleep(value.get('sleep', 0))
        segments = [SimpleNamespace(start=s['start'],end=s['end'],text=s['text'])
                    for s in value.get('segments', [{'start':1.25,'end':4.5,'text':'开始🙂'}])]
        return iter(segments), SimpleNamespace(language='zh')
'''

DOWNLOADER = r'''
import http.cookiejar
import json
from pathlib import Path
from urllib.parse import urlsplit, parse_qs
class YoutubeDL:
    def __init__(self, options):
        assert options['cachedir'] is False
        self.options=options
        self.cookiejar=http.cookiejar.CookieJar()
    def __enter__(self): return self
    def __exit__(self, *args): pass
    def extract_info(self, url, download=False):
        assert download is False
        cfg=json.loads((Path(__file__).parent.parent / 'behavior.json').read_text(encoding='utf-8'))
        if cfg.get('downloadError'):
            raise RuntimeError('secret-cookie https://signed.example/audio?key=secret')
        query=parse_qs(urlsplit(url).query)
        part=int(query.get('p',['1'])[0])
        self.info={'id':urlsplit(url).path.rstrip('/').split('/')[-1]+'_p'+str(part),
                   'ext':'m4a','duration':cfg.get('downloadDuration',60)}
        if cfg.get('wrongPart'): self.info['id']='wrong-video'
        return self.info
    def prepare_filename(self, info):
        return self.options['outtmpl'].replace('%(ext)s','m4a')
    def process_info(self, info):
        target=Path(self.prepare_filename(info))
        target.parent.mkdir(parents=True,exist_ok=True)
        target.write_bytes(b'controlled audio')
        (target.parent / 'download.json').write_text(json.dumps({'id':info['id'],
             'cookieCount':len(self.cookiejar), 'retries':self.options['retries']}))
'''


def install(directory, **behavior):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "behavior.json").write_text(json.dumps(behavior, ensure_ascii=False), encoding="utf-8")
    for name, source in (("faster_whisper", ENGINE), ("yt_dlp", DOWNLOADER)):
        module = directory / name
        module.mkdir(exist_ok=True)
        (module / "__init__.py").write_text(source, encoding="utf-8")
    return directory
