"""사용자가 실행하는 VMS 이벤트 관찰 도구. Pi/PTZ 명령은 보내지 않는다."""
import argparse
import json
import socket
import time
from websocket_test_client import WebSocket

parser = argparse.ArgumentParser()
parser.add_argument('--port', type=int, default=5000)
parser.add_argument('--camera', default='CAM01')
parser.add_argument('--seconds', type=int, default=30)
args = parser.parse_args()
client = WebSocket(args.port)
client.socket.settimeout(1)
try:
    status = client.request('GET_CAMERA_STATUS', args.camera)
    if not status['ok']:
        print(json.dumps(status, ensure_ascii=False))
    else:
        camera = status['data']['camera']
        print(json.dumps({'cameraId': args.camera, 'eventsStatus': camera.get('eventsStatus'),
                          'eventsEnabled': camera.get('capabilities', {}).get('events')}, ensure_ascii=False))
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        try:
            message = client.receive()
        except socket.timeout:
            # socket.makefile는 timeout 이후 재사용이 안전하지 않다. 접속만 다시 열고 명령은 보내지 않는다.
            client.close()
            client = WebSocket(args.port)
            client.socket.settimeout(1)
            continue
        if message.get('cameraId') != args.camera or message.get('event') not in (
                'EVENT_RECEIVER_STATUS', 'CAMERA_METADATA', 'CAMERA_EVENT'):
            continue
        data = dict(message.get('data', {}))
        raw = data.pop('rawItems', {})
        source = data.pop('sourceItems', {})
        # 실제 필드 이름만 출력한다. 원본의 임의 문자열/인증 정보는 console에 출력하지 않는다.
        if raw: data['receivedFieldNames'] = list(raw)
        if source: data['sourceFieldNames'] = list(source)
        print(json.dumps({'event': message['event'], 'cameraId': args.camera, 'data': data}, ensure_ascii=False))
finally:
    client.close()
