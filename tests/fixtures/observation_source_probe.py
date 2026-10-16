import json
import sys
import time

wire, log, mode = sys.argv[1:]
start = json.loads(sys.stdin.readline())
with open(log, 'a') as f:
    f.write(json.dumps(start) + '\n')
with open(wire) as f:
    events = json.load(f)
lines = [json.dumps(e, ensure_ascii=False) + '\n' for e in events]
if mode == 'reconnect':
    after = int(start.get('cursor', 'cursor:0').split(':')[-1] or 0)
    pending = [e for e in events if int(e['sequence']) > after]
    if pending:
        print(json.dumps(pending[0]), flush=True)
elif mode == 'invalid':
    sys.stdout.write('not-json\n' + ''.join(lines))
    sys.stdout.flush()
elif mode == 'overlong':
    sys.stdout.write('x' * 65537 + '\n' + ''.join(lines))
    sys.stdout.flush()
elif mode == 'split':
    first = lines[0].encode('utf-8')
    sys.stdout.buffer.write(first[:len(first)//2]); sys.stdout.flush()
    time.sleep(.03)
    sys.stdout.buffer.write(first[len(first)//2:] + ''.join(lines[1:]).encode('utf-8')); sys.stdout.flush()
else:
    sys.stdout.write(''.join(lines)); sys.stdout.flush()
for line in sys.stdin:
    with open(log, 'a') as f:
        f.write(line)
    if mode == 'reconnect' and json.loads(line).get('type') == 'ack':
        sys.exit(7)
    if json.loads(line).get('type') == 'stop':
        break
