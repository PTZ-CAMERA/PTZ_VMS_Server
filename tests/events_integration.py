"""로컬 ONVIF/RTSP fixture만 사용한다. Pi 실물·장시간 시험은 자동 실행하지 않는다."""
from datetime import datetime, timezone, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.request import parse_http_list, parse_keqv_list
import hashlib
import json
import signal
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ET
from rtsp_integration import Fixture
from websocket_test_client import WebSocket, free_port


def main():
    runtime_mode = len(sys.argv) > 3 and sys.argv[3] == 'runtime'
    fixture = Fixture(Path(sys.argv[2]).read_bytes())
    operations, failures = [], []
    notifications = []
    pending = []
    lock = threading.Lock()
    fail_pull = [False]
    events_ns = 'http://www.onvif.org/ver10/events/wsdl'
    schema = 'http://www.onvif.org/ver10/schema'
    wsn = 'http://docs.oasis-open.org/wsn/b-2'
    addr = 'http://www.w3.org/2005/08/addressing'
    sequence = [0]

    def utc(offset=0):
        return (datetime.now(timezone.utc) + timedelta(seconds=offset)).isoformat(timespec='milliseconds').replace('+00:00', 'Z')

    def message(detected, operation='Changed', extra='', topic='Analytics/PersonDetection'):
        sequence[0] += 1
        # 시간 문자열을 원본 그대로 재전송하여 중복 시험을 한다.
        return f'<n:NotificationMessage><n:Topic>pc:{topic}</n:Topic><n:Message><t:Message UtcTime="{utc(sequence[0] / 1000)}" PropertyOperation="{operation}"><t:Source><t:SimpleItem Name="ProfileToken" Value="main"/></t:Source><t:Data><t:SimpleItem Name="{ "Enabled" if topic == "Tracking/State" else "Detected" }" Value="{str(detected).lower()}"/><t:SimpleItem Name="Available" Value="true"/>{extra}</t:Data></t:Message></n:Message></n:NotificationMessage>'

    def runtime_event(kind):
        sequence[0] += 1
        return f'<n:NotificationMessage><n:Topic>pc:Runtime/StateEvent</n:Topic><n:Message><t:Message UtcTime="{utc(sequence[0] / 1000)}"><t:Source><t:SimpleItem Name="ProfileToken" Value="main"/></t:Source><t:Data><t:SimpleItem Name="Type" Value="{kind}"/></t:Data></t:Message></n:Message></n:NotificationMessage>'

    class Onvif(BaseHTTPRequestHandler):
        def log_message(self, *args): pass

        def do_POST(self):
            body = self.rfile.read(int(self.headers['Content-Length']))
            authorization = self.headers.get('Authorization', '')
            if not authorization.startswith('Digest '):
                self.send_response(401)
                self.send_header('WWW-Authenticate', 'Digest realm="events", nonce="fixture", algorithm=SHA-256, qop="auth"')
                self.send_header('Content-Length', '0'); self.end_headers(); return
            values = parse_keqv_list(parse_http_list(authorization[7:]))
            sha = lambda text: hashlib.sha256(text.encode()).hexdigest()
            expected = sha(f'{sha("user:events:secret")}:{values["nonce"]}:{values["nc"]}:{values["cnonce"]}:auth:{sha("POST:" + values["uri"])}')
            if values.get('response') != expected: failures.append('Digest mismatch')
            root = ET.fromstring(body)
            operation = list(root.find('{http://www.w3.org/2003/05/soap-envelope}Body'))[0]
            op = operation.tag.rsplit('}', 1)[-1]
            operations.append(op)
            origin = f'http://127.0.0.1:{self.server.server_port}'
            contents, namespace = '', 'http://www.onvif.org/ver10/device/wsdl'
            if op == 'GetDeviceInformation': contents = '<Model>Events fixture</Model>'
            elif op == 'GetCapabilities': contents = f'<Capabilities><Media><XAddr>{origin}/media</XAddr></Media></Capabilities>'
            elif op == 'GetServices': contents = f'<Service><Namespace>http://www.onvif.org/ver10/media/wsdl</Namespace><XAddr>{origin}/media</XAddr></Service><Service><Namespace>{events_ns}</Namespace><XAddr>{origin}/events</XAddr></Service>'
            elif op == 'GetProfiles': contents = '<Profiles token="main"><Name>Main</Name></Profiles>'
            elif op == 'GetStreamUri': contents = f'<MediaUri><Uri>rtsp://127.0.0.1:{fixture.port}/stream</Uri></MediaUri>'
            elif op == 'GetSystemDateAndTime':
                now = datetime.now(timezone.utc)
                contents = f'<SystemDateAndTime><t:UTCDateTime><t:Date><t:Year>{now.year}</t:Year><t:Month>{now.month}</t:Month><t:Day>{now.day}</t:Day></t:Date><t:Time><t:Hour>{now.hour}</t:Hour><t:Minute>{now.minute}</t:Minute><t:Second>{now.second}</t:Second></t:Time></t:UTCDateTime></SystemDateAndTime>'
            elif op == 'GetEventProperties':
                namespace = events_ns
                # Pi는 그룹 QName만 있고 leaf에만 topic=true가 있다.
                contents = '<TopicSet><pc:Analytics><PersonDetection wstop:topic="true"><t:MessageDescription><t:Data><t:SimpleItemDescription Name="Detected" Type="xs:boolean"/><t:SimpleItemDescription Name="Confidence" Type="xs:float"/></t:Data></t:MessageDescription></PersonDetection></pc:Analytics>'
                if runtime_mode:
                    contents += '<pc:Runtime><StateEvent wstop:topic="true"><t:MessageDescription IsProperty="false"><t:Data><t:SimpleItemDescription Name="Type" Type="xs:string"/></t:Data></t:MessageDescription></StateEvent></pc:Runtime>'
                contents += '</TopicSet>'
            elif op == 'CreatePullPointSubscription':
                namespace = events_ns
                contents = f'<SubscriptionReference><a:Address>{origin}/subscription</a:Address><a:ReferenceParameters><pc:SubscriptionId>fixture-id</pc:SubscriptionId></a:ReferenceParameters></SubscriptionReference><CurrentTime>{utc()}</CurrentTime><TerminationTime>{utc(3)}</TerminationTime>'
                with lock: pending.extend([message(False, 'Initialized'), message(False, 'Initialized', topic='Tracking/State')])
            elif op in ('PullMessages', 'Renew', 'Unsubscribe'):
                if self.path != '/subscription': failures.append('Wrong subscription endpoint')
                ref = root.find('.//{urn:fixture}SubscriptionId')
                if ref is None or ref.text != 'fixture-id': failures.append('Missing EPR reference parameter')
                namespace = events_ns if op == 'PullMessages' else wsn
                if op == 'PullMessages':
                    if operation.find(f'{{{events_ns}}}Timeout').text != 'PT1S': failures.append('Wrong pull timeout')
                    if int(operation.find(f'{{{events_ns}}}MessageLimit').text) > 64: failures.append('Pi message limit exceeded')
                    with lock:
                        batch = pending[:]; pending.clear()
                        fault = fail_pull[0]; fail_pull[0] = False
                    if fault:
                        response = '<s:Fault><s:Code><s:Value>s:Receiver</s:Value></s:Code><s:Reason><s:Text>fixture fault</s:Text></s:Reason></s:Fault>'
                        return self.respond(response)
                    if not batch: time.sleep(.15)
                    contents = ''.join(batch)
                elif op == 'Renew': contents = f'<CurrentTime>{utc()}</CurrentTime><TerminationTime>{utc(3)}</TerminationTime>'
            else: failures.append('Unexpected operation ' + op)
            self.respond(f'<r:{op}Response xmlns:r="{namespace}">{contents}</r:{op}Response>')

        def respond(self, body):
            payload = f'<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:t="{schema}" xmlns:n="{wsn}" xmlns:a="{addr}" xmlns:pc="urn:fixture" xmlns:wstop="http://docs.oasis-open.org/wsn/t-1" xmlns:xs="http://www.w3.org/2001/XMLSchema"><s:Body>{body}</s:Body></s:Envelope>'.encode()
            self.send_response(200); self.send_header('Content-Length', str(len(payload))); self.end_headers()
            try: self.wfile.write(payload)
            except (BrokenPipeError, ConnectionResetError): pass

    http = ThreadingHTTPServer(('127.0.0.1', 0), Onvif)
    threading.Thread(target=http.serve_forever, daemon=True).start()
    process, client = None, None

    def wait(predicate, timeout=12):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if predicate(): return
            time.sleep(.05)
        raise AssertionError('Events fixture timeout; operations=' + str(operations))

    try:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); api, relay = free_port(), free_port()
            config = root / 'vms.conf'
            config.write_text(f'rtsp_url=\nonvif_url=http://127.0.0.1:{http.server_port}/device\nonvif_username=user\nonvif_password=secret\nclient_port={api}\nrtsp_relay_port={relay}\nevents_enabled=true\nevents_pull_seconds=1\nevents_retry_ms=100\nmetadata_sample_ms=100\nrecording_root=recordings\nrecording_database=data/vms.db\n')
            process = subprocess.Popen([sys.argv[1], '--config', str(config)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            logs = []
            def drain():
                for line in process.stdout: logs.append(line)
            threading.Thread(target=drain, daemon=True).start()
            def connect():
                nonlocal client
                try: client = WebSocket(api); return True
                except OSError: return False
            wait(connect)
            rid = [0]
            def call(command, **fields):
                rid[0] += 1; request_id = str(rid[0])
                client.send(json.dumps(dict(version=1, requestId=request_id, command=command, **fields)))
                while True:
                    result = client.receive()
                    if result.get('type') == 'notification': notifications.append(result)
                    if result.get('type') == 'response' and result.get('requestId') == request_id:
                        assert result['ok'], result
                        return result['data']
            call('REGISTER_CAMERA', deviceServiceUrl=f'http://127.0.0.1:{http.server_port}/device', profileToken='main')
            wait(lambda: 'PullMessages' in operations)
            time.sleep(.3)
            assert call('GET_EVENTS', cameraId='CAM01')['events'] == []
            wait(lambda: call('GET_CAMERA_STATUS', cameraId='CAM01')['camera']['status'] == 'ONLINE')
            call('START_RECORDING', cameraId='CAM01')
            wait(lambda: call('GET_CAMERA_STATUS', cameraId='CAM01')['camera']['recording'])
            time.sleep(.15)
            score = '<t:SimpleItem Name="Confidence" Value="0.92"/><t:SimpleItem Name="BoxX" Value="250"/><t:SimpleItem Name="FrameId" Value="9007199254740993"/><t:SimpleItem Name="StreamEpoch" Value="9007199254740995"/><t:SimpleItem Name="FramePtsNs" Value="9007199254740997"/>'
            first = message(True, extra=score)
            with lock:
                if runtime_mode:
                    r = runtime_event('PERSON_DETECTED')
                    pending.extend([r, r])
                pending.extend([first, first])
            wait(lambda: len(call('GET_EVENTS', cameraId='CAM01')['events']) == 1)
            with lock:
                if runtime_mode: pending.extend([runtime_event('PERSON_LOST'), runtime_event('TRACKING_STARTED')])
                pending.extend([message(False), message(True, topic='Tracking/State')])
            wait(lambda: len(call('GET_EVENTS', cameraId='CAM01')['events']) == 3)
            # 상태 변화 뒤 연결한 UI에도 camera status 응답 다음 마지막 추적 metadata를 제공한다.
            late=WebSocket(api)
            try:
                assert late.request('GET_CAMERA_STATUS','CAM01')['ok']
                deadline=time.monotonic()+5
                while time.monotonic()<deadline:
                    snapshot=late.receive()
                    if snapshot.get('event')=='CAMERA_METADATA' and snapshot.get('data',{}).get('tracking') is True:
                        assert snapshot['cameraId']=='CAM01'; break
                else:raise AssertionError('Late client did not receive confirmed tracking metadata')
            finally:late.close()
            found = call('GET_EVENTS', cameraId='CAM01', types=['PERSON_DETECTED'], minConfidence=.8)['events']
            if runtime_mode:
                assert found == [], 'Runtime Type-only에 confidence를 만들어 넣으면 안 된다'
                found = call('GET_EVENTS', cameraId='CAM01', types=['PERSON_DETECTED'])['events']
                assert len(found) == 1 and found[0]['confidence'] is None
            else:
                assert len(found) == 1 and found[0]['confidence'] == .92 and found[0]['bboxX'] == 250
                assert found[0]['bboxWidth'] is None and found[0]['panCommandAngle'] is None
            samples = call('GET_DETECTIONS', cameraId='CAM01', minConfidence=.8)['detections']
            assert len(samples) == 1 and samples[0]['frameId'] == '9007199254740993'
            assert samples[0]['streamEpoch'] == '9007199254740995' and samples[0]['framePtsNs'] == '9007199254740997'
            page = call('GET_EVENTS', cameraId='CAM01', limit=1)
            page2 = call('GET_EVENTS', cameraId='CAM01', limit=1, cursor=page['nextCursor'])
            assert page['events'][0]['id'] != page2['events'][0]['id']
            assert not call('GET_EVENT_PLAYBACK', eventId=found[0]['id'])['playable']
            assert call('GET_EVENT_PLAYBACK', eventId=found[0]['id'], allowEstimated=True)['reason'] == 'NO_RECORDING_AT_TIME'
            call('STOP_RECORDING', cameraId='CAM01')
            playback = call('GET_EVENT_PLAYBACK', eventId=found[0]['id'], allowEstimated=True)
            assert playback['playable'] and playback['offsetMs'] >= 0 and playback['timeMapping'] == 'receive_estimated'
            assert Path(playback['recording']['filePath']).is_file()
            assert playback['anchorKind'] == 'receive_pts'
            sample_playback = call('GET_EVENT_PLAYBACK', sampleId=samples[0]['sampleId'], allowEstimated=True)
            assert sample_playback['playable'] and sample_playback['sampleId'] == samples[0]['sampleId']
            properties = [n for n in notifications if n.get('event') == 'EVENT_RECEIVER_STATUS' and n['data'].get('state') == 'PROPERTIES']
            assert properties and 'Analytics/PersonDetection' in properties[0]['data']['properties']['topics']
            assert properties[0]['data']['transitionSource'] == ('runtime' if runtime_mode else 'property_fallback')
            with lock: fail_pull[0] = True
            wait(lambda: operations.count('CreatePullPointSubscription') >= 2)
            time.sleep(.3)
            assert len(call('GET_EVENTS', cameraId='CAM01')['events']) == 3
            assert 'Renew' in operations
            assert any(n.get('event') == 'CAMERA_METADATA' for n in notifications)
            process.send_signal(signal.SIGINT); process.wait(timeout=8)
            assert process.returncode == 0 and 'Unsubscribe' in operations
            with sqlite3.connect(root / 'data/vms.db') as db:
                assert db.execute('SELECT count(*) FROM metadata_samples').fetchone()[0] > 0
                assert db.execute('SELECT count(*) FROM events').fetchone()[0] == 3
            assert not failures, failures
            assert 'secret' not in ''.join(logs)
            print('PASS', 'runtime' if runtime_mode else 'legacy', 'Events Digest/Pi fields/64bit/refparameters/renew/fault-reconnect/dedup/search/recording-offset/shutdown')
    finally:
        if client: client.close()
        if process and process.poll() is None: process.terminate(); process.wait(timeout=10)
        http.shutdown(); http.server_close(); fixture.close()

if __name__ == '__main__': main()
