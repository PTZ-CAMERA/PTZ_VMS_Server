"""Exercise the real C++ server, RTSP fixture, concurrent WebSockets and errors."""
from pathlib import Path
import json
import socket
import sys
import tempfile
import time
from rtsp_integration import Fixture, Process
from websocket_test_client import WebSocket, free_port


def response(client, request_id):
    while True:
        result = client.receive()
        if result.get('type') == 'response' and result.get('requestId') == request_id:
            return result


def main():
    executable = str(Path(sys.argv[1]).resolve())
    fixture = Fixture(Path(sys.argv[2]).read_bytes())
    clients = []
    p = None
    half_open = None
    with tempfile.TemporaryDirectory() as temp:
        try:
            p = Process(executable, Path(temp), fixture.port)
            p.wait_for('Listening ws://')
            first = WebSocket(p.api_port)
            second = WebSocket(p.api_port)
            clients += [first, second]
            result = first.request('GET_CAMERA_LIST')
            assert result['ok'] and len(result['data']['cameras']) == 1
            camera = result['data']['cameras'][0]
            assert camera['id'] == 'CAM01' and not camera['recording']
            assert not camera['capabilities']['ptz']
            assert 'password' not in json.dumps(result) and 'rtsp://' not in json.dumps(result)
            assert second.request('GET_CAMERA_STATUS', 'missing')['error']['code'] == 'CAMERA_NOT_FOUND'
            assert second.request('GET_CAMERA_STATUS')['error']['code'] == 'INVALID_REQUEST'
            for command in ('TRACKING_ON', 'GET_EVENTS', 'START_LIVE', 'STOP_LIVE', 'unknown'):
                assert first.request(command, 'CAM01')['error']['code'] == 'NOT_SUPPORTED'
            assert first.request('PTZ_STOP', 'CAM01')['error']['code'] == 'PTZ_NOT_AVAILABLE'
            assert first.request('PTZ_MOVE', 'CAM01')['error']['code'] == 'INVALID_VELOCITY'
            first.send('not-json')
            assert response(first, None)['error']['code'] == 'INVALID_REQUEST'
            first.send('binary message', opcode=2)
            assert response(first, None)['error']['code'] == 'INVALID_REQUEST'
            first.send(json.dumps({'version': 2, 'requestId': 'v2', 'command': 'GET_CAMERA_LIST'}))
            assert response(first, 'v2')['error']['code'] == 'INVALID_REQUEST'
            fragment = json.dumps({'version': 1, 'requestId': 'fragment', 'command': 'GET_CAMERA_LIST'})
            first.send(fragment[:20], final=False)
            first.send(fragment[20:], opcode=0)
            assert response(first, 'fragment')['ok']
            for client in clients:
                end = time.monotonic() + 10
                while time.monotonic() < end:
                    notification = client.receive()
                    if notification.get('event') == 'CAMERA_STATUS':
                        cam = notification['data']['camera']
                        if cam['status'] == 'ONLINE' and cam['packets'] > 0:
                            assert cam['codec'] == 'H264' and cam['width'] == 1280 and cam['height'] == 720
                            break
                else:
                    raise AssertionError('No ONLINE packet notification')
            fixture.pause.set()
            end = time.monotonic() + 10
            while time.monotonic() < end:
                notification = second.receive()
                if notification.get('event') == 'CAMERA_STATUS':
                    cam = notification['data']['camera']
                    if cam['status'] != 'ONLINE':
                        assert cam['packets'] == 0 and cam['codec'] == ''
                        break
            else:
                raise AssertionError('No stalled camera status notification')
            assert first.request('GET_CAMERA_LIST')['ok'], 'Camera failure must not kill API'
            fixture.pause.clear()
            oversized = WebSocket(p.api_port)
            clients.append(oversized)
            oversized.send('x' * 65537)
            try:
                while True:
                    oversized.receive()
            except (EOFError, OSError):
                pass
            first.close(); clients.remove(first)
            reconnect = WebSocket(p.api_port)
            clients.append(reconnect)
            assert reconnect.request('GET_CAMERA_LIST')['ok']
            half_open = socket.create_connection(('127.0.0.1', p.api_port))
            assert second.request('GET_CAMERA_LIST')['ok'], 'Blocked handshake must not block clients'
            p.stop()
            print('PASS real server: two clients, query/errors, JSON validation, fragmentation, notifications, camera failure, reconnect, oversized frame, blocked handshake and shutdown')
        finally:
            for client in clients:
                client.close()
            if half_open:
                half_open.close()
            if p:
                p.cleanup()
            fixture.close()


if __name__ == '__main__':
    main()
