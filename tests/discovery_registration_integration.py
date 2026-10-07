"""Discovery previews a managed URI; ADD starts ingest using that same identity."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import threading
import time
from urllib.request import urlopen
from urllib.error import HTTPError
from rtsp_integration import Fixture
from websocket_test_client import WebSocket, free_port


def main():
    fixture = Fixture(Path(sys.argv[2]).read_bytes())

    class Onvif(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            body = self.rfile.read(int(self.headers['Content-Length'])).decode()
            if 'GetDeviceInformation' in body:
                result = '<GetDeviceInformationResponse><Model>Test camera</Model></GetDeviceInformationResponse>'
            elif 'GetCapabilities' in body:
                result = f'<GetCapabilitiesResponse><Capabilities><Media><XAddr>http://127.0.0.1:{self.server.server_port}/media</XAddr></Media></Capabilities></GetCapabilitiesResponse>'
            elif 'GetProfiles' in body:
                result = '<GetProfilesResponse><Profiles token="main"><Name>Main</Name></Profiles></GetProfilesResponse>'
            elif 'GetStreamUri' in body:
                result = f'<GetStreamUriResponse><MediaUri><Uri>rtsp://127.0.0.1:{fixture.port}/stream</Uri></MediaUri></GetStreamUriResponse>'
            else:
                self.send_error(400)
                return
            response = f'<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body>{result}</s:Body></s:Envelope>'.encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/soap+xml')
            self.send_header('Content-Length', str(len(response)))
            self.end_headers()
            self.wfile.write(response)

    http = ThreadingHTTPServer(('127.0.0.1', 0), Onvif)
    thread = threading.Thread(target=http.serve_forever, daemon=True)
    thread.start()
    process = None
    client = None
    try:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            api, relay = free_port(), free_port()
            service = f'http://127.0.0.1:{http.server_port}/device'
            config = root / 'vms.conf'
            config.write_text(f'camera_id=CAM01\nrtsp_url=\nonvif_url={service}\nclient_port={api}\nrtsp_relay_port={relay}\nstats_interval_ms=100\nrecording_root=recordings\nrecording_database=data/vms.db\n')
            process = subprocess.Popen([str(Path(sys.argv[1]).resolve()), '--config', str(config)],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            deadline = time.monotonic() + 5
            while client is None and time.monotonic() < deadline:
                assert process.poll() is None, 'VMS exited during startup'
                try:
                    client = WebSocket(api)
                except OSError:
                    time.sleep(0.05)
            assert client is not None

            def call(command, **fields):
                request = {'version': 1, 'requestId': command, 'command': command, **fields}
                client.send(json.dumps(request))
                while True:
                    response = client.receive()
                    if response.get('requestId') == command and response.get('type') == 'response':
                        assert response['ok'], response
                        return response['data']

            before = call('DISCOVER_CAMERAS')
            device = next(item for item in before['devices'] if item['deviceServiceUrl'] == service)
            assert device['cameraId'] == 'CAM01'
            expected = f'rtsp://127.0.0.1:{relay}/CAM01'
            assert device['rtspUri'] == expected and not device['registered'] and not device['ready']
            assert not fixture.connections, 'Discovery must not connect to upstream RTSP'
            assert 'password' not in json.dumps(before)
            added = call('REGISTER_CAMERA', deviceServiceUrl=service, profileToken='main')
            assert added['cameraId'] == device['cameraId'] and added['rtspUri'] == expected
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                try:
                    with urlopen(f'http://127.0.0.1:{api}/api/v1/cameras/CAM01/stream', timeout=2) as reply:
                        stream = json.load(reply)['data']
                    assert stream['ready'] and stream['uri'] == expected
                    break
                except HTTPError as error:
                    assert error.code == 503
                    time.sleep(0.1)
            else:
                raise AssertionError('Registered stream did not become ready')
            after = call('DISCOVER_CAMERAS')
            device = next(item for item in after['devices'] if item['deviceServiceUrl'] == service)
            assert device['registered'] and device['ready'] and device['rtspUri'] == expected
            print('PASS: discovery URI -> no ingest -> ADD -> same ready VMS URI')
    finally:
        if client:
            client.close()
        if process:
            process.terminate()
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        http.shutdown()
        http.server_close()
        thread.join(timeout=2)
        fixture.close()


if __name__ == '__main__':
    main()
