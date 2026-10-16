"""Controlled command and JSONL MCP fixture; never touches a real account."""
import json
import sys
import time

def run(args):
    mode = args.get('mode', 'echo')
    if mode == 'slow':
        time.sleep(0.25)
    if mode == 'fail':
        sys.exit(4)
    if mode == 'scalar':
        return 42
    if mode == 'empty':
        return []
    if mode == 'accepted':
        return {'status': 'queued', 'receipt': 'job-123', 'tracking': {'operation': 'check', 'id': 'job-123'}}
    if mode == 'reported_failure':
        return {'status': 'bad', 'message': 'declined'}
    return {'echo': args, 'remote_message_id': 'remote-123', 'status': 'done', 'content': 'backend-confirmed', 'occurred_at': '2026-10-16T08:00:00.000Z'}

if '--mcp' not in sys.argv:
    if '--args' in sys.argv or '--slow-args' in sys.argv:
        if '--slow-args' in sys.argv:
            time.sleep(0.25)
        print(json.dumps(sys.argv[2:]))
    else:
        print(json.dumps(run(json.load(sys.stdin))))
else:
    initialized = False
    for line in sys.stdin:
        request = json.loads(line)
        method = request.get('method')
        if method == 'notifications/initialized':
            initialized = True
            continue
        result = None
        if method == 'initialize':
            result = {'protocolVersion': '2024-11-05', 'capabilities': {'tools': {}}, 'serverInfo': {'name': 'fixture', 'version': '1'}}
        elif method == 'tools/list':
            if not initialized:
                sys.exit(3)
            if request.get('params', {}).get('cursor') == 'page2':
                result = {'tools': [{'name': 'probe', 'description': 'Controlled fixture', 'inputSchema': {'type': 'object', 'properties': {'mode': {'type': 'string'}}, 'required': ['mode'], 'additionalProperties': False}}]}
            else:
                result = {'tools': [], 'nextCursor': 'page2'}
        elif method == 'tools/call':
            args = request['params']['arguments']
            mode = args.get('mode')
            if mode == 'disconnect':
                sys.exit(0)
            if mode == 'timeout':
                time.sleep(3)
            if mode == 'protocol_error':
                print(json.dumps({'jsonrpc': '2.0', 'id': request['id'], 'error': {'code': -32603, 'message': 'fixture failure'}}), flush=True)
                continue
            result = {'isError': mode == 'tool_error', 'structuredContent': {'matches': [], 'mode': mode}, 'content': [{'type': 'text', 'text': 'This is not JSON data'}, {'type': 'text', 'text': 'Second content block'}]}
        else:
            continue
        print(json.dumps({'jsonrpc': '2.0', 'id': request['id'], 'result': result}), flush=True)
