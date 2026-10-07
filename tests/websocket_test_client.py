"""Minimal RFC6455 test client using only Python's standard library."""
import base64
import hashlib
import json
import os
import socket
import struct


def free_port():
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        return listener.getsockname()[1]


class WebSocket:
    def __init__(self, port):
        self.socket = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.socket.settimeout(8)
        self.stream = self.socket.makefile('rb')
        key = base64.b64encode(os.urandom(16)).decode()
        self.socket.sendall((f'GET /ws HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
        assert b'101' in self.stream.readline()
        headers = {}
        while True:
            line = self.stream.readline()
            if line == b'\r\n':
                break
            if not line:
                raise EOFError('Handshake ended early')
            k, v = line.decode().split(':', 1)
            headers[k.lower()] = v.strip()
        expected = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        assert headers['sec-websocket-accept'] == expected

    def send(self, payload, opcode=1, final=True):
        if isinstance(payload, str):
            payload = payload.encode()
        mask = os.urandom(4)
        length = len(payload)
        header = bytes([(128 if final else 0) | opcode])
        if length < 126:
            header += bytes([128 | length])
        elif length < 65536:
            header += bytes([128 | 126]) + struct.pack('!H', length)
        else:
            header += bytes([128 | 127]) + struct.pack('!Q', length)
        self.socket.sendall(header + mask + bytes(value ^ mask[i % 4] for i, value in enumerate(payload)))

    def exact(self, length):
        payload = self.stream.read(length)
        if len(payload) != length:
            raise EOFError('WebSocket ended')
        return payload

    def receive(self):
        while True:
            first, second = self.exact(2)
            opcode = first & 15
            length = second & 127
            assert not second & 128, 'Server frames must not be masked'
            if length == 126:
                length = struct.unpack('!H', self.exact(2))[0]
            elif length == 127:
                length = struct.unpack('!Q', self.exact(8))[0]
            payload = self.exact(length)
            if opcode == 9:
                self.send(payload, opcode=10)
                continue
            if opcode == 8:
                raise EOFError('WebSocket close received')
            if opcode == 10:
                continue
            assert opcode == 1 and first & 128
            return json.loads(payload)

    def request(self, command, camera_id=None, request_id='test'):
        obj = {'version': 1, 'requestId': request_id, 'command': command}
        if camera_id is not None:
            obj['cameraId'] = camera_id
        self.send(json.dumps(obj))
        while True:
            result = self.receive()
            if result.get('type') == 'response' and result.get('requestId') == request_id:
                return result

    def close(self):
        self.stream.close()
        self.socket.close()
