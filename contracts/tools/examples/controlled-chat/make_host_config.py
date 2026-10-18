#!/usr/bin/env python3
"""Print reviewable host bindings for the local controlled chat package."""
import argparse
import json
from pathlib import Path
import sys
import uuid


def configuration(profile, database, python, with_source=True):
    root = Path(__file__).resolve().parent
    database = Path(database).expanduser().resolve()
    if root == database or root in database.parents:
        raise ValueError('Keep the mutable database outside the plugin package')
    profile = str(uuid.UUID(profile))
    python = str(Path(python).expanduser().resolve())
    plugin, entry, source = 'example.controlled-chat', 'example.controlled-chat-tool', 'example.controlled-events'
    manifest = json.loads((root / 'plugin.json').read_text())
    plugin = manifest['id']
    entry = manifest['entries'][0]['id']
    source = manifest.get('sources', [{'id': source}])[0]['id']
    channel, connection = 'controlled-chat', 'account-one'
    identity = dict(provider=channel, namespace=connection, remoteActorId='pet-account')
    rooms = ['group-a', 'group-b']

    def ref(operation):
        return dict(pluginId=plugin, entryId=entry, entryVersion='1.0.0', operationId=operation)

    def conversation(room):
        material = profile + json.dumps([channel, connection, room], ensure_ascii=False, separators=(',', ':'))
        return str(uuid.uuid5(uuid.UUID('312f2a64-cdc2-4fb7-a004-1f398bf10dca'), material))

    observation = dict(id='account', pluginId=plugin, channel=channel, connectionKey=connection,
                       provider=channel, namespace=connection, selfIdentity=identity,
                       conversations=[dict(remoteConversationId=r, kind='group') for r in rooms])
    settings = dict(program=python, script=str(root / 'adapter.py'), database=str(database))
    if with_source:
        observation['source'] = dict(sourceId=source, configurationKey=source, executor=python, enabled=False)
    grants, replies = [], []
    for room in [None] + rooms:
        for op in ['list', 'read', 'send', 'receipt']:
            if room is None and op == 'send':
                continue
            scope = conversation(room) if room else ''
            grant = dict(id=scope + ':' + op, profileId=profile, pluginId=plugin, entryId=entry,
                         operationId=op, effects=['write' if op == 'send' else 'read'],
                         executor=python, autonomous=True)
            grant.update(dict(conversationId=scope) if room else dict(scope='internal_activity'))
            grants.append(grant)
    for room in rooms:
        replies.append(dict(channel=channel, connectionKey=connection, remoteConversationId=room,
                            kind='group', selfIdentity=identity, ref=ref('send'),
                            grantRef=conversation(room) + ':send',
                            argumentBindings={'/chat_id': 'remoteConversationId', '/text': 'content',
                                              '/reply_to': 'replyToRemoteId'}))
    return dict(plugins=[dict(rootPath=str(root), enabled=True)], configuration={entry: settings, source: settings},
                grants=grants, replyBindings=replies, observationBindings=[observation],
                observationAdapters=[dict(ref=ref('read'), observationBindingId='account', itemsPointer='/items',
                                          fields=dict(eventId='/event_id', sequence='/sequence', cursor='/cursor',
                                                      remoteConversationId='/chat_id', remoteMessageId='/message_id',
                                                      text='/text', remoteActorId='/speaker_id', occurredAt='/occurred_at',
                                                      replyToRemoteId='/reply_to'))])


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', required=True, help='Existing host profile UUID')
    parser.add_argument('--database', required=True, help='Local adapter journal outside the package')
    parser.add_argument('--python', default=sys.executable, help='Trusted absolute Python executable')
    parser.add_argument('--without-source', action='store_true', help='Omit optional source binding')
    args = parser.parse_args()
    print(json.dumps(configuration(args.profile, args.database, args.python, not args.without_source), ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
