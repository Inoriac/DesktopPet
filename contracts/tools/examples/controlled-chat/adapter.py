#!/usr/bin/env python3
"""Controlled, local-only chat adapter. Its SQLite journal is always outside the package."""
import argparse
import datetime
import json
import os
import select
import sqlite3
import sys
import time


def connect(path):
    db = sqlite3.connect(path, timeout=10)
    db.execute('PRAGMA journal_mode=WAL')
    db.execute('CREATE TABLE IF NOT EXISTS journal (seq INTEGER PRIMARY KEY AUTOINCREMENT, chat TEXT NOT NULL, remote_id TEXT UNIQUE, actor TEXT NOT NULL, text TEXT NOT NULL, at TEXT NOT NULL, reply TEXT NOT NULL)')
    db.execute('CREATE TABLE IF NOT EXISTS acknowledgements (event_id TEXT PRIMARY KEY, cursor TEXT NOT NULL)')
    db.commit()
    return db


def stamp():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='milliseconds')


def item(row):
    seq, chat, remote, actor, text, at, reply = row
    return dict(event_id='event-' + str(seq), sequence=str(seq), cursor='cursor:' + str(seq),
                chat_id=chat, message_id=remote, speaker_id=actor, text=text, occurred_at=at,
                reply_to=reply)


def append(db, chat, actor, text, reply=''):
    with db:
        cur = db.execute('INSERT INTO journal(chat,actor,text,at,reply) VALUES(?,?,?,?,?)', (chat, actor, text, stamp(), reply))
        seq = cur.lastrowid
        db.execute('UPDATE journal SET remote_id=? WHERE seq=?', ('message-' + str(seq), seq))
    return item(db.execute('SELECT * FROM journal WHERE seq=?', (seq,)).fetchone())


def cursor_number(cursor):
    if not cursor:
        return 0
    if not isinstance(cursor, str) or not cursor.startswith('cursor:') or not cursor[7:].isdigit():
        raise ValueError('Cursor is not from this adapter journal')
    return int(cursor[7:])


def stream(db, provider, namespace):
    control_input = sys.stdin.buffer.raw
    first = json.loads(control_input.readline(65537))
    if first.get('type') != 'start':
        raise ValueError('Expected host start')
    last = cursor_number(first.get('cursor', ''))
    while True:
        for row in db.execute('SELECT * FROM journal WHERE seq>? ORDER BY seq LIMIT 64', (last,)).fetchall():
            v = item(row)
            envelope = dict(type='observation', kind='message', eventId=v['event_id'], sequence=v['sequence'], cursor=v['cursor'],
                            occurredAt=v['occurred_at'], conversation=dict(remoteConversationId=v['chat_id'], kind='group'),
                            speaker=dict(provider=provider, namespace=namespace, remoteActorId=v['speaker_id']),
                            message=dict(remoteMessageId=v['message_id'], text=v['text'], replyToRemoteId=v['reply_to']))
            print(json.dumps(envelope, ensure_ascii=False), flush=True)
            last = row[0]
        ready, _, _ = select.select([control_input], [], [], 0.05)
        if ready:
            line = control_input.readline(65537)
            if not line:
                return
            control = json.loads(line)
            if control.get('type') == 'stop':
                return
            if control.get('type') == 'ack':
                with db:
                    db.execute('INSERT OR REPLACE INTO acknowledgements VALUES(?,?)', (control['eventId'], control['cursor']))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--database', required=True)
    parser.add_argument('--operation', choices=['list', 'read', 'send', 'receipt', 'source', 'inject'], required=True)
    parser.add_argument('--provider', default='controlled-chat')
    parser.add_argument('--namespace', default='account-one')
    args = parser.parse_args()
    root = os.path.dirname(os.path.realpath(__file__))
    if os.path.commonpath([root, os.path.realpath(args.database)]) == root:
        raise ValueError('Keep the mutable journal outside the immutable package')
    db = connect(args.database)
    if args.operation == 'source':
        stream(db, args.provider, args.namespace)
        return
    request = json.load(sys.stdin)
    if args.operation == 'list':
        result = dict(conversations=[dict(chat_id=row[0], kind='group') for row in db.execute('SELECT DISTINCT chat FROM journal ORDER BY chat')])
    elif args.operation == 'read':
        limit = request.get('limit', 64)
        if type(limit) is not int or not 1 <= limit <= 256:
            raise ValueError('Use a page size between 1 and 256')
        rows = db.execute('SELECT * FROM journal WHERE chat=? AND seq>? ORDER BY seq LIMIT ?',
                          (request['chat_id'], cursor_number(request.get('cursor', '')), limit)).fetchall()
        result = dict(items=[item(row) for row in rows])
    elif args.operation in ('send', 'inject'):
        value = append(db, request['chat_id'], request['speaker_id'] if args.operation == 'inject' else 'pet-account', request['text'], request.get('reply_to', ''))
        result = dict(remote_message_id=value['message_id'], content=value['text'], occurred_at=value['occurred_at'])
    else:
        row = db.execute('SELECT * FROM journal WHERE remote_id=?', (request['message_id'],)).fetchone()
        result = dict(found=bool(row), message=item(row) if row else None)
    print(json.dumps(result, ensure_ascii=False))

if __name__ == '__main__':
    main()
