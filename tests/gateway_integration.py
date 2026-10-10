"""선택 실행: 실제 게이트웨이와 브라우저를 연결하되 입력은 합성 RTSP fixture이다."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from rtsp_integration import Fixture, Process
from websocket_test_client import free_port

def main():
    executable, sample, gateway, web = map(lambda x: Path(x).resolve(), sys.argv[1:5])
    spec = importlib.util.spec_from_file_location('gateway_tools', Path(__file__).parents[1] / 'tools/mediamtx_gateway.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    fixture = Fixture(sample.read_bytes()); process = None; children = []
    with tempfile.TemporaryDirectory() as temp:
        try:
            root = Path(temp); http_port, ice_port, web_port = free_port(), free_port(), free_port()
            process = Process(str(executable), root, fixture.port, extra_config=f'webrtc_gateway_url=http://127.0.0.1:{http_port}\n')
            process.wait_for('Connected')
            settings = module.configuration('127.0.0.1', process.relay_port, ['CAM01'], http_port, ice_port)
            settings['webrtcAllowOrigins'] = [f'http://127.0.0.1:{web_port}']
            config = root / 'gateway.json'; config.write_text(json.dumps(settings))
            with (root/'gateway.log').open('w+') as log:
                children.append(subprocess.Popen([str(gateway), str(config)], stdout=log, stderr=log))
                children.append(subprocess.Popen(['node', str(web/'server.mjs')], env={**os.environ, 'PORT': str(web_port)}, stdout=log, stderr=log))
                time.sleep(.7)
                result = subprocess.run(['node', str(web/'tests/gateway-read.mjs'), str(web_port), str(process.api_port)], capture_output=True, text=True, timeout=45)
                if result.returncode:
                    log.seek(0); raise AssertionError(result.stderr + '\n' + log.read())
                print(result.stdout.strip())
        finally:
            for child in children:
                if child.poll() is None: child.terminate(); child.wait(timeout=5)
            if process: process.cleanup()
            fixture.close()

if __name__ == '__main__': main()
