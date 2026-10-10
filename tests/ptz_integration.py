"""Real VMS + Digest SHA-256 ONVIF fixture: PTZ lifecycle and failure isolation."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import hashlib
import json
import signal
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ET
from urllib.request import parse_http_list, parse_keqv_list
from rtsp_integration import Fixture
from websocket_test_client import WebSocket, free_port


def main():
    fixture = Fixture(Path(sys.argv[2]).read_bytes())
    actions, operations, problems = [], [], []
    delay = [0.0]
    fault = [False]
    tracking_capability = [True]
    base = 'http://www.onvif.org/ver10/tptz/PanTiltSpaces/'
    ns = {'p': 'http://www.onvif.org/ver20/ptz/wsdl', 'tt': 'http://www.onvif.org/ver10/schema'}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args): pass
        def do_POST(self):
            raw = self.rfile.read(int(self.headers['Content-Length']))
            auth = self.headers.get('Authorization', '')
            if not auth.startswith('Digest '):
                self.send_response(401)
                self.send_header('WWW-Authenticate', 'Digest realm="test", nonce="abcdef", algorithm=SHA-256, qop="auth"')
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            d = parse_keqv_list(parse_http_list(auth[7:]))
            h = lambda text: hashlib.sha256(text.encode()).hexdigest()
            expected = h(f'{h("user:test:secret")}:{d["nonce"]}:{d["nc"]}:{d["cnonce"]}:auth:{h("POST:" + d["uri"])}')
            if d.get('response') != expected: problems.append('Digest mismatch')
            root = ET.fromstring(raw)
            operation = list(root.find('{http://www.w3.org/2003/05/soap-envelope}Body'))[0]
            op = operation.tag.split('}')[-1]
            operations.append(op)
            url = f'http://127.0.0.1:{self.server.server_port}'
            content = ''
            if op == 'GetDeviceInformation': content = '<Model>PTZ fixture</Model>'
            elif op == 'GetCapabilities': content = f'<Capabilities><Media><XAddr>{url}/media</XAddr></Media></Capabilities>'
            elif op == 'GetServices':
                content = ''.join(f'<Service><Namespace>http://www.onvif.org/{ver}/{name}/wsdl</Namespace><XAddr>{url}/{name}</XAddr></Service>' for ver, name in [('ver10', 'media'), ('ver20', 'ptz')])
            elif op == 'GetProfiles': content = '<Profiles token="main"><Name>Main</Name><PTZConfiguration token="ptz-config"/></Profiles>'
            elif op == 'GetServiceCapabilities': content = '<Capabilities MoveAndTrack="PTZVector"/>' if tracking_capability[0] else '<Capabilities/>'
            elif op == 'GetConfigurationOptions':
                assert operation.find('p:ConfigurationToken', ns).text == 'ptz-config'
                content = '<PTZConfigurationOptions><Spaces>'
                for kind, uri in [('ContinuousPanTiltVelocitySpace', 'VelocityGenericSpace'), ('AbsolutePanTiltPositionSpace', 'PositionGenericSpace')]:
                    content += f'<{kind}><URI>{base}{uri}</URI><XRange><Min>-1</Min><Max>1</Max></XRange><YRange><Min>-1</Min><Max>1</Max></YRange></{kind}>'
                content += '</Spaces><PTZTimeout><Min>PT0.1S</Min><Max>PT5S</Max></PTZTimeout></PTZConfigurationOptions>'
            elif op == 'GetStreamUri': content = f'<MediaUri><Uri>rtsp://127.0.0.1:{fixture.port}/stream</Uri></MediaUri>'
            elif op in ('ContinuousMove', 'Stop', 'AbsoluteMove', 'MoveAndStartTracking'):
                if operation.find('p:ProfileToken', ns).text != 'main': problems.append('Wrong profile')
                if self.path != '/ptz': problems.append('Wrong endpoint')
                xy = operation.find('.//tt:PanTilt', ns)
                if op == 'ContinuousMove':
                    if operation.find('p:Timeout', ns).text != 'PT1S': problems.append('Missing timeout')
                    if xy.get('space') != base + 'VelocityGenericSpace': problems.append('Wrong velocity space')
                if op == 'AbsoluteMove':
                    if float(xy.get('x')) != 0 or float(xy.get('y')) != 0: problems.append('Wrong center')
                if op == 'Stop' and operation.find('p:PanTilt', ns).text != 'true': problems.append('Wrong stop')
                if op == 'MoveAndStartTracking' and operation.find('p:TargetPosition', ns) is not None: problems.append('Tracking unexpectedly moved to a position')
                actions.append((op, float(xy.get('x')) if xy is not None else None, time.monotonic()))
                if op == 'ContinuousMove': time.sleep(delay[0])
            else: problems.append('Unexpected operation ' + op)
            response = f'<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body><{op}Response>{content}</{op}Response></s:Body></s:Envelope>'
            if fault[0] and op in ('ContinuousMove', 'MoveAndStartTracking'): response = '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body><s:Fault><s:Reason>rejected</s:Reason></s:Fault></s:Body></s:Envelope>'
            payload = response.encode()
            self.send_response(200)
            self.send_header('Content-Length', str(len(payload))); self.end_headers()
            try: self.wfile.write(payload)
            except (BrokenPipeError, ConnectionResetError): pass

    http = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=http.serve_forever, daemon=True).start()
    process = None
    clients = []
    def wait_for(predicate, timeout=5):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if predicate(): return
            time.sleep(.02)
        raise AssertionError('Timed out; actions=' + str(actions))
    def send(c, rid, command, **kwargs):
        c.send(json.dumps(dict(version=1, requestId=rid, command=command, cameraId='CAM01', **kwargs)))
    def read(c, rid, kind='response'):
        while True:
            r = c.receive()
            if r.get('requestId') == rid and r.get('type') == kind: return r
    def move(c, rid, x=.3): send(c, rid, 'PTZ_MOVE', panVelocity=x, tiltVelocity=0)
    def stop(c, rid):
        send(c, rid, 'PTZ_STOP'); assert read(c, rid)['ok']
        assert read(c, rid, 'notification')['data']['phase'] == 'PI_ACKNOWLEDGED'
    try:
        with tempfile.TemporaryDirectory() as tmp:
            api, relay = free_port(), free_port()
            config = Path(tmp) / 'vms.conf'
            service = f'http://127.0.0.1:{http.server_port}/device'
            config.write_text(f'camera_id=CAM01\nrtsp_url=\nonvif_url={service}\nonvif_username=user\nonvif_password=secret\nclient_port={api}\nrtsp_relay_port={relay}\nrecording_root=recordings\nrecording_database=data/vms.db\n')
            process = subprocess.Popen([sys.argv[1], '--config', str(config)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            def connect():
                try: clients.append(WebSocket(api)); return True
                except OSError: return False
            wait_for(connect)
            c = clients[0]
            send(c, 'reg', 'REGISTER_CAMERA', deviceServiceUrl=service, profileToken='main')
            assert read(c, 'reg')['ok']
            assert {'GetServices', 'GetProfiles', 'GetConfigurationOptions'} <= set(operations)
            assert c.request('GET_CAMERA_STATUS', 'CAM01')['data']['camera']['capabilities']['ptz']
            move(c, 'bad', 2); assert not read(c, 'bad')['ok']; assert not actions
            move(c, 'm1'); assert read(c, 'm1')['data']['phase'] == 'ACCEPTED'
            r = read(c, 'm1', 'notification'); assert r['data']['phase'] == 'PI_ACKNOWLEDGED' and not r['data']['motorArrivalConfirmed']
            other = WebSocket(api); clients.append(other)
            send(other, 'busy', 'PTZ_MOVE', panVelocity=.3, tiltVelocity=0, _sessionId=1)
            assert read(other, 'busy')['error']['code'] == 'PTZ_BUSY'
            # 200ms refresh keeps moving; releasing yields an immediate Stop.
            for i in range(5):
                time.sleep(.2); move(c, f'hold{i}')
                assert read(c, f'hold{i}')['ok']; assert read(c, f'hold{i}', 'notification')['ok']
            assert all(a[0] == 'ContinuousMove' for a in actions)
            stop(c, 'release'); assert actions[-1][0] == 'Stop'
            send(c, 'center', 'PTZ_CENTER'); assert read(c, 'center')['ok']; assert read(c, 'center', 'notification')['ok']
            stop(c, 'centerstop')
            assert c.request('GET_CAMERA_STATUS','CAM01')['data']['camera']['capabilities']['tracking']
            start_count=len(actions)
            send(c,'track-on','TRACKING_ON'); assert read(c,'track-on')['data']['phase']=='ACCEPTED'
            ack=read(c,'track-on','notification'); assert ack['ok'] and ack['data']['trackingStateConfirmed'] is False
            time.sleep(.8); assert [a[0] for a in actions[start_count:]]==['MoveAndStartTracking'], 'Manual lease stopped auto tracking'
            send(other,'track-busy','TRACKING_OFF'); assert read(other,'track-busy')['error']['code']=='PTZ_BUSY'
            move(c,'manual-override'); assert read(c,'manual-override')['ok']; assert read(c,'manual-override','notification')['ok']
            stop(c,'manual-release')
            send(c,'track-off','TRACKING_OFF'); assert read(c,'track-off')['ok']; assert read(c,'track-off','notification')['ok']; assert actions[-1][0]=='Stop'
            tracking_capability[0]=False
            send(c,'no-track-reg','REGISTER_CAMERA',deviceServiceUrl=service,profileToken='main'); assert read(c,'no-track-reg')['ok']
            assert not c.request('GET_CAMERA_STATUS','CAM01')['data']['camera']['capabilities']['tracking']
            send(c,'no-track','TRACKING_ON'); assert read(c,'no-track')['error']['code']=='TRACKING_NOT_SUPPORTED'
            tracking_capability[0]=True
            send(c,'track-reg','REGISTER_CAMERA',deviceServiceUrl=service,profileToken='main'); assert read(c,'track-reg')['ok']
            fault[0]=True
            send(c,'track-fault','TRACKING_ON'); assert read(c,'track-fault')['ok']; assert read(c,'track-fault','notification')['error']['code']=='ONVIF_FAULT'
            wait_for(lambda:actions[-1][0]=='Stop'); fault[0]=False; time.sleep(.05)
            n=len(actions)
            send(other,'track-disconnect','TRACKING_ON'); assert read(other,'track-disconnect')['ok']; assert read(other,'track-disconnect','notification')['ok']
            other.close(); clients.remove(other); wait_for(lambda:len(actions)>n+1 and actions[-1][0]=='Stop')
            time.sleep(.05); other=WebSocket(api); clients.append(other)
            # A blocked Pi cannot stall camera/status API; queued motion is replaced by Stop.
            delay[0] = .35
            n = len(actions); move(c, 'slow'); assert read(c, 'slow')['ok']
            wait_for(lambda: len(actions) > n)
            for i in range(10):
                move(c, f'queued{i}', -.3)
            start = time.monotonic(); assert other.request('GET_CAMERA_LIST')['ok']; assert time.monotonic() - start < .3
            stop(c, 'priority'); assert [a[0] for a in actions[n:]] == ['ContinuousMove', 'Stop'], actions[n:]
            delay[0] = 0
            n = len(actions); move(c, 'lease'); assert read(c, 'lease')['ok']; assert read(c, 'lease', 'notification')['ok']
            wait_for(lambda: len(actions) >= n + 2); assert actions[-1][0] == 'Stop'
            time.sleep(.05)
            n = len(actions); move(c, 'disconnect'); assert read(c, 'disconnect')['ok']; assert read(c, 'disconnect', 'notification')['ok']
            c.close(); clients.remove(c)
            wait_for(lambda: len(actions) >= n + 2); assert actions[-1][0] == 'Stop'
            time.sleep(.05)
            fault[0] = True
            move(other, 'fault'); assert read(other, 'fault')['ok']
            r = read(other, 'fault', 'notification'); assert r['error']['code'] == 'ONVIF_FAULT'
            wait_for(lambda: actions[-1][0] == 'Stop'); fault[0] = False
            time.sleep(.05)
            delay[0] = 3.5
            move(other, 'timeout'); assert read(other, 'timeout')['ok']
            r = read(other, 'timeout', 'notification'); assert r['error']['code'] == 'ONVIF_COMMUNICATION_ERROR'
            wait_for(lambda: actions[-1][0] == 'Stop'); delay[0] = 0
            time.sleep(.05)
            move(other, 'shutdown'); assert read(other, 'shutdown')['ok']; assert read(other, 'shutdown', 'notification')['ok']
            n = len(actions); process.send_signal(signal.SIGINT); process.wait(timeout=8)
            assert process.returncode == 0 and any(a[0] == 'Stop' for a in actions[n:])
            assert not problems, problems
            print('PASS Digest SHA-256, discovery/spaces, velocity validation, acceptance/ack, ownership, 200ms refresh, Stop coalescing, lease, disconnect, faults/timeouts, shutdown')
    finally:
        for c in clients: c.close()
        if process and process.poll() is None: process.terminate(); process.wait(timeout=10)
        http.shutdown(); http.server_close(); fixture.close()

if __name__ == '__main__': main()
