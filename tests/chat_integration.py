"""Fake Gemini HTTP + 실제 VMS/SQLite. 외부 Gemini·Pi를 자동 호출하지 않는다."""
from datetime import datetime, timezone, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json
import os
import signal
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
from websocket_test_client import WebSocket, free_port


def main():
    now = datetime.now(timezone.utc)
    received = int(now.timestamp() * 1000) - 1000
    requests, problems = [], []
    def plan(**changes):
        result = dict(action='search', recordKind='detections', cameraId='CAM01',
                      fromIso=(now-timedelta(hours=1)).isoformat().replace('+00:00', 'Z'),
                      toIso=now.isoformat().replace('+00:00', 'Z'), minConfidence=.8,
                      types=[], limit=1, resultIndex=None, message='')
        result.update(changes)
        return result

    class Gemini(BaseHTTPRequestHandler):
        def log_message(self, *args): pass
        def do_POST(self):
            payload = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            context = json.loads(payload['contents'][0]['parts'][0]['text'])
            requests.append(context)
            if self.headers.get('x-goog-api-key') != 'fixture-secret': problems.append('Key header mismatch')
            if 'recordings' in json.dumps(context) or 'filePath' in json.dumps(context): problems.append('Recording data leaked to provider')
            message = context['message']
            if message == 'quota': return self.reply(429, {'error': {'message': 'fixture-secret raw provider error'}})
            if message == 'timeout': time.sleep(1.3)
            value = plan()
            if message == 'next': value = plan(action='next_page')
            elif message == 'pick': value = plan(action='select_result', resultIndex=1)
            elif message == 'ambiguous': value = plan(action='clarify', message='검색할 시간을 알려주세요.')
            elif message == 'color': value = plan(action='unsupported')
            elif message == 'malicious': value['sql'] = 'DELETE FROM recordings'
            return self.reply(200, {'candidates': [{'finishReason': 'STOP', 'content': {'parts': [{'text': json.dumps(value)}]}}]})
        def reply(self, status, value):
            body = json.dumps(value).encode(); self.send_response(status)
            self.send_header('Content-Length', str(len(body))); self.end_headers()
            try: self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError): pass

    http = ThreadingHTTPServer(('127.0.0.1', 0), Gemini)
    threading.Thread(target=http.serve_forever, daemon=True).start()
    process, clients = None, []
    try:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); api, relay = free_port(), free_port()
            key = root/'key.local.conf'; key.write_text('gemini_api_key=fixture-secret\n')
            config = root/'vms.conf'
            config.write_text(f'rtsp_url=\nonvif_url=\nclient_port={api}\nrtsp_relay_port={relay}\nchat_enabled=true\nchat_api_base=http://127.0.0.1:{http.server_port}/v1beta\nchat_api_key_file={key}\nchat_timeout_ms=1000\nrecording_database=data/vms.db\nrecording_root=recordings\n')
            environment = dict(os.environ); environment.pop('GEMINI_API_KEY', None); environment.pop('GOOGLE_API_KEY', None)
            process = subprocess.Popen([sys.argv[1], '--config', str(config)], env=environment,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for _ in range(100):
                try: clients.append(WebSocket(api)); break
                except OSError: time.sleep(.05)
            assert clients
            client = clients[0]; client.socket.settimeout(10)
            assert client.request('GET_EVENTS', 'CAM01')['ok']
            file = root/'recordings'/'segment.mkv'; file.parent.mkdir(parents=True); file.write_bytes(b'fixture')
            with sqlite3.connect(root/'data/vms.db') as db:
                db.execute('INSERT INTO recordings(camera_id,start_time,end_time,file_path,duration,codec,width,height) VALUES(?,?,?,?,?,?,?,?)',
                           ('CAM01', received-1000, received+2000, str(file), 3, 'H264', 1280, 720))
                for i in range(2):
                    metadata = dict(cameraId='CAM01', receivedTimeMs=received+i, sourceTimeMs=received+i,
                                    confidence=.92, captureTimeMs=None, analysisTimeMs=None)
                    cursor = db.execute('INSERT INTO metadata_samples(camera_id,source_time_ms,received_time_ms,payload_json) VALUES(?,?,?,?)',
                                        ('CAM01', received+i, received+i, json.dumps(metadata)))
                    db.execute('INSERT INTO detection_index VALUES(?,?,?,?)', (cursor.lastrowid, 'CAM01', received+i, .92))
            sequence = [0]
            def call(message, c=client):
                sequence[0] += 1; rid = str(sequence[0])
                c.send(json.dumps(dict(version=1, requestId=rid, command='CHAT_SEARCH', cameraId='CAM01', message=message)))
                while True:
                    value = c.receive()
                    if value.get('requestId') == rid: return value
            first = call('search'); assert first['ok'], first
            data = first['data']; assert len(data['results']) == 1 and data['nextCursor']
            assert data['results'][0]['playback']['playable'] and data['results'][0]['playback']['offsetMs'] >= 0
            assert call('next')['data']['results'][0]['id'] != data['results'][0]['id']
            assert call('pick')['data']['playback']['playable']
            assert call('ambiguous')['data']['action'] == 'clarify'
            assert call('color')['data']['action'] == 'unsupported'
            assert call('malicious')['error']['code'] == 'LLM_INVALID_PLAN'
            quota = call('quota'); assert quota['error']['code'] == 'LLM_RATE_LIMITED' and 'fixture-secret' not in json.dumps(quota)
            clients.append(WebSocket(api)); other = clients[-1]; other.socket.settimeout(5)
            assert call('pick', other)['error']['code'] == 'CHAT_RESULT_INDEX_INVALID', 'Different session must not access previous results'
            client.send(json.dumps(dict(version=1, requestId='slow', command='CHAT_SEARCH', cameraId='CAM01', message='timeout')))
            start = time.monotonic(); assert other.request('GET_CAMERA_LIST')['ok']; assert time.monotonic()-start < .4
            while True:
                slow = client.receive()
                if slow.get('requestId') == 'slow': break
            assert slow['error']['code'] == 'LLM_TIMEOUT'
            file.unlink()
            assert not call('pick')['data']['playback']['playable'], 'Recheck deleted recordings on selection'
            with sqlite3.connect(root/'data/vms.db') as db: assert db.execute('SELECT count(*) FROM recordings').fetchone()[0] == 1
            assert not problems, problems
            process.send_signal(signal.SIGINT); process.wait(timeout=8); assert process.returncode == 0
            print('PASS Chat Gemini schema/key, real DB search/playback, conversation/pagination, session isolation, SQL rejection, quota/timeout, responsive WS, shutdown')
    finally:
        for c in clients: c.close()
        if process and process.poll() is None: process.terminate(); process.wait(timeout=8)
        http.shutdown(); http.server_close()

if __name__ == '__main__': main()
