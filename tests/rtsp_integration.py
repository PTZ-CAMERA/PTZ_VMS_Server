"""Dependency-free local RTSP/TCP fixture; no FFmpeg CLI is used during tests.
The checked-in Annex B sample is synthetic H.264, 1280x720 at 25 fps.
This is a test fixture, not a production RTSP server.
"""
import base64
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time


class Fixture:
    def __init__(self, sample):
        nals = [n for n in re.split(b'\x00\x00\x00?\x01', sample) if n]
        self.sps = next(n for n in nals if n[0] & 31 == 7)
        self.pps = next(n for n in nals if n[0] & 31 == 8)
        self.frames = []
        for nal in nals:
            if nal[0] & 31 == 9:
                self.frames.append([])
            if self.frames:
                self.frames[-1].append(nal)
        self.sock = socket.socket()
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen()
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.closed = threading.Event()
        self.pause = threading.Event()
        self.silent = False
        self.connections = []
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self.accept, daemon=True)
        self.thread.start()

    def accept(self):
        while not self.closed.is_set():
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with self.lock:
                self.connections.append(conn)
            threading.Thread(target=self.serve, args=(conn,), daemon=True).start()

    def serve(self, conn):
        conn.settimeout(0.2)
        sending = threading.Event()
        send_lock = threading.Lock()
        sender = None
        buffer = b''
        try:
            while not self.closed.is_set():
                try:
                    data = conn.recv(8192)
                except socket.timeout:
                    continue
                if not data:
                    break
                buffer += data
                while b'\r\n\r\n' in buffer:
                    request, buffer = buffer.split(b'\r\n\r\n', 1)
                    lines = request.decode().split('\r\n')
                    method = lines[0].split()[0]
                    headers = dict(line.split(': ', 1) for line in lines[1:] if ': ' in line)
                    if self.silent:
                        continue
                    extra = ''
                    body = ''
                    if method == 'OPTIONS':
                        extra = 'Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n'
                    elif method == 'DESCRIBE':
                        sprop = ','.join(base64.b64encode(x).decode() for x in (self.sps, self.pps))
                        profile = self.sps[1:4].hex()
                        body = (f'v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=VMS test\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n'
                                f'a=control:*\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=framerate:25\r\n'
                                f'a=fmtp:96 packetization-mode=1;profile-level-id={profile};sprop-parameter-sets={sprop}\r\na=control:trackID=0\r\n')
                        extra = f'Content-Type: application/sdp\r\nContent-Base: rtsp://127.0.0.1:{self.port}/stream/\r\n'
                    elif method == 'SETUP':
                        extra = 'Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nSession: 123456\r\n'
                    else:
                        extra = 'Session: 123456\r\n'
                    response = (f'RTSP/1.0 200 OK\r\nCSeq: {headers.get("CSeq", "1")}\r\n{extra}Content-Length: {len(body)}\r\n\r\n{body}').encode()
                    with send_lock:
                        conn.sendall(response)
                    if method == 'PLAY' and sender is None:
                        sending.set()
                        sender = threading.Thread(target=self.stream, args=(conn, sending, send_lock), daemon=True)
                        sender.start()
                    if method == 'TEARDOWN':
                        return
        except (OSError, ValueError, UnicodeError):
            pass
        finally:
            sending.clear()
            conn.close()
            if sender:
                sender.join(timeout=1)

    def stream(self, conn, sending, send_lock):
        seq = 0
        timestamp = 0
        frame_index = 0
        try:
            while sending.is_set() and not self.closed.is_set():
                if self.pause.is_set():
                    time.sleep(0.02)
                    continue
                frame = self.frames[frame_index % len(self.frames)]
                for i, nal in enumerate(frame):
                    if len(nal) <= 1200:
                        payloads = [nal]
                    else:
                        chunks = [nal[j:j+1198] for j in range(1, len(nal), 1198)]
                        payloads = [bytes([(nal[0] & 0xe0) | 28, (nal[0] & 31) | (0x80 if j == 0 else 0) | (0x40 if j == len(chunks)-1 else 0)]) + chunk for j, chunk in enumerate(chunks)]
                    for j, payload in enumerate(payloads):
                        marker = 128 if i == len(frame)-1 and j == len(payloads)-1 else 0
                        rtp = struct.pack('!BBHII', 0x80, marker | 96, seq & 65535, timestamp & 0xffffffff, 0x12345678) + payload
                        with send_lock:
                            conn.sendall(b'$\x00' + struct.pack('!H', len(rtp)) + rtp)
                        seq += 1
                frame_index += 1
                timestamp += 3600
                time.sleep(0.04)
        except OSError:
            pass

    def disconnect(self):
        with self.lock:
            for conn in self.connections:
                try:
                    conn.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                conn.close()
            self.connections.clear()

    def close(self):
        self.closed.set()
        self.disconnect()
        self.sock.close()
        self.thread.join(timeout=1)


class Process:
    def __init__(self, executable, root, port, delay=200):
        with socket.socket() as api_listener, socket.socket() as relay_listener:
            api_listener.bind(('127.0.0.1', 0))
            relay_listener.bind(('127.0.0.1', 0))
            self.api_port = api_listener.getsockname()[1]
            self.relay_port = relay_listener.getsockname()[1]
        config = root / f'vms-{port}.conf'
        config.write_text(f'camera_id=CAM01\nrtsp_url=rtsp://127.0.0.1:{port}/stream\nconnect_timeout_ms=3000\nread_timeout_ms=800\nreconnect_delay_ms={delay}\nstats_interval_ms=200\nclient_port={self.api_port}\nrtsp_relay_port={self.relay_port}\nrecording_root={root / "recordings"}\nrecording_database={root / "vms.db"}\n')
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == 'nt' else 0
        self.proc = subprocess.Popen([executable, '--config', str(config)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, creationflags=flags)
        self.lines = []
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        for line in self.proc.stdout:
            self.lines.append(line.rstrip())

    def wait_for(self, text, count=1, timeout=10):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if sum(text in line for line in self.lines) >= count:
                return
            if self.proc.poll() is not None:
                break
            time.sleep(0.03)
        raise AssertionError(f'Missing {text!r} count={count}\n' + '\n'.join(self.lines))

    def stop(self):
        started = time.monotonic()
        self.proc.send_signal(signal.CTRL_BREAK_EVENT if os.name == 'nt' else signal.SIGINT)
        self.proc.wait(timeout=4)
        self.thread.join(timeout=1)
        assert self.proc.returncode == 0, '\n'.join(self.lines)
        assert time.monotonic() - started < 4
        assert any('Shutdown complete' in line for line in self.lines)

    def cleanup(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        self.thread.join(timeout=1)
        self.proc.stdout.close()


def main():
    executable = str(Path(sys.argv[1]).resolve())
    fixture = Fixture(Path(sys.argv[2]).read_bytes())
    processes = []
    try:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            p = Process(executable, root, fixture.port)
            processes.append(p)
            p.wait_for('Packets received=')
            for expected in ('Codec: H264', 'Resolution: 1280x720', 'FPS: 25', 'Time Base: 1/90000'):
                p.wait_for(expected)
            assert any(int(line.split('Packets received=')[1].split()[0]) > 0 for line in p.lines if 'Packets received=' in line)
            fixture.disconnect()
            p.wait_for('Connection lost')
            p.wait_for('Codec: H264', count=2)
            p.wait_for('Packets received=', count=3)
            fixture.pause.set()
            p.wait_for('Reconnecting', count=2)
            fixture.pause.clear()
            p.wait_for('Codec: H264', count=3)
            p.stop()
            print('PASS H264 info, AVPacket receive, disconnect/reconnect, stalled I/O, active shutdown')

            fixture.silent = True
            p = Process(executable, root, fixture.port)
            processes.append(p)
            p.wait_for('Connecting')
            time.sleep(0.2)
            p.stop()
            print('PASS Ctrl+C while RTSP handshake is blocked')

            fixture.close()
            p = Process(executable, root, fixture.port, delay=300000)
            processes.append(p)
            p.wait_for('RTSP open failed')
            p.wait_for('Reconnecting')
            p.stop()
            print('PASS unreachable camera and immediate shutdown during 5-minute reconnect wait')
    finally:
        for process in processes:
            process.cleanup()
        fixture.close()


if __name__ == '__main__':
    main()
