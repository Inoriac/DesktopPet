#!/usr/bin/env python3
"""Optional QQ observation source using the agent-observation-1 protocol."""
import sys
sys.dont_write_bytecode = True

import argparse
import queue
import sqlite3
import threading

if __package__:
    from .common import AdapterError, MAX_JSON_BYTES, decode_json, json_text, load_settings, observation, validate_settings
    from .journal import Journal
    from .transport import OneBotClient
else:
    from common import AdapterError, MAX_JSON_BYTES, decode_json, json_text, load_settings, observation, validate_settings
    from journal import Journal
    from transport import OneBotClient


def run_source(settings, stdin, stdout):
    settings = validate_settings(settings)
    stopped = threading.Event()
    controls = queue.Queue(maxsize=256)
    incoming = queue.Queue(maxsize=256)

    def enqueue(destination, value):
        while not stopped.is_set():
            try:
                destination.put(value, timeout=.1)
                return
            except queue.Full:
                pass

    def read_controls():
        while not stopped.is_set():
            line = stdin.readline(MAX_JSON_BYTES + 1)
            enqueue(controls, line)
            if not line or len(line) > MAX_JSON_BYTES:
                return

    def listen():
        backoff = 1
        while not stopped.is_set():
            try:
                with OneBotClient(settings, capture_events=True) as client:
                    backoff = 1
                    while not stopped.is_set():
                        event = client.receive(.1)
                        if event is not None:
                            enqueue(incoming, event)
            except AdapterError as error:
                if stopped.is_set():
                    return
                if error.code not in ("NETWORK_ERROR", "TIMEOUT"):
                    enqueue(incoming, error)
                    return
                # No URL, token or user message is written to diagnostics.
                print("QQ source disconnected; reconnecting after backoff", file=sys.stderr, flush=True)
                if stopped.wait(backoff):
                    return
                backoff = min(backoff * 2, 30)

    threading.Thread(target=read_controls, daemon=True).start()
    started, last, pending = False, "", {}
    try:
        with Journal(settings) as journal:
            while True:
                try:
                    line = controls.get(timeout=.02)
                except queue.Empty:
                    line = None
                if line is not None:
                    if not line:
                        return
                    if len(line) > MAX_JSON_BYTES:
                        raise AdapterError("INVALID_CONTROL", "Source control exceeds 64 KiB")
                    try:
                        control = decode_json(line)
                    except (ValueError, UnicodeError):
                        raise AdapterError("INVALID_CONTROL", "Source control must be JSON") from None
                    if not isinstance(control, dict):
                        raise AdapterError("INVALID_CONTROL", "Source control must be an object")
                    kind = control.get("type")
                    if kind == "stop":
                        return
                    if not started:
                        if kind != "start":
                            raise AdapterError("INVALID_CONTROL", "Expected a source start control")
                        last = control.get("cursor", "")
                        journal.cursor_number(last)
                        started = True
                        threading.Thread(target=listen, daemon=True).start()
                    elif kind == "ack":
                        event_id, cursor = control.get("eventId"), control.get("cursor")
                        if not isinstance(event_id, str) or pending.get(event_id) != cursor:
                            raise AdapterError("INVALID_ACK", "ACK must identify an observation sent by this source run")
                        journal.ack(event_id, cursor)
                        del pending[event_id]
                    else:
                        raise AdapterError("INVALID_CONTROL", "Unexpected source control")
                if not started:
                    continue
                # Bounded batches leave time for stop/ACK even during a flood.
                for _ in range(32):
                    try:
                        event = incoming.get_nowait()
                    except queue.Empty:
                        break
                    if isinstance(event, AdapterError):
                        raise event
                    journal.ingest(event)
                if len(pending) < 64:
                    for item in journal.after(last, limit=64 - len(pending)):
                        line = json_text(observation(settings, item)) + "\n"
                        if len(line.encode("utf-8")) > MAX_JSON_BYTES:
                            raise AdapterError("MESSAGE_TOO_LARGE", "Normalized observation exceeds 64 KiB")
                        stdout.write(line)
                        stdout.flush()
                        pending[item["event_id"]] = item["cursor"]
                        last = item["cursor"]
    finally:
        stopped.set()


def main(argv=None):
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--settings", required=True)
    options = parser.parse_args(argv)
    try:
        # A daemon reader must not hold BufferedReader's lock at interpreter
        # shutdown after stop. FileIO reads are also portable to Windows pipes.
        run_source(load_settings(options.settings), sys.stdin.buffer.raw, sys.stdout)
    except AdapterError as error:
        print(error.code + ": " + error.message, file=sys.stderr)
        return 1
    except (sqlite3.Error, OSError):
        print("STORAGE_ERROR: QQ journal or source pipe unavailable; state was not reset", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
