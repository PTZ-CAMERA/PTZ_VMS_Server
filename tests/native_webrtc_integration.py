"""합성 RTSP → 실제 VMS 내장 WebRTC → Chromium. Pi/모터 요청 없음."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from rtsp_integration import Fixture, Process
from websocket_test_client import free_port

def main():
    executable, sample, web = [Path(p).resolve() for p in sys.argv[1:4]]
    fixture = Fixture(sample.read_bytes()); process = None; static = None
    with tempfile.TemporaryDirectory() as temp:
        try:
            root=Path(temp); port=free_port()
            process=Process(str(executable),root,fixture.port,extra_config='webrtc_enabled=true\n')
            process.wait_for('Connected')
            static=subprocess.Popen(['node',str(web/'server.mjs')],env={**os.environ,'PORT':str(port)},stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            result=subprocess.run(['node',str(web/'tests/native-webrtc-read.mjs'),str(port),str(process.api_port)],capture_output=True,text=True,timeout=60)
            if result.returncode: raise AssertionError(result.stdout+'\n'+result.stderr)
            print(result.stdout.strip())
        finally:
            if static and static.poll() is None: static.terminate();static.wait(timeout=5)
            if process:process.cleanup()
            fixture.close()
if __name__=='__main__':main()
