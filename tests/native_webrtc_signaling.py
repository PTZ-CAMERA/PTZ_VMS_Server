"""실제 libdatachannel SDP, 세션 격리와 연결 상한. 합성 RTSP만 사용한다."""
import json
from pathlib import Path
import sys
import tempfile
from rtsp_integration import Fixture, Process
from websocket_test_client import WebSocket

def main():
    fixture=Fixture(Path(sys.argv[2]).read_bytes()); process=None; clients=[]
    with tempfile.TemporaryDirectory() as temp:
        try:
            process=Process(sys.argv[1],Path(temp),fixture.port,extra_config='webrtc_enabled=true\n')
            process.wait_for('Connected')
            count=0
            def call(c,command,**fields):
                nonlocal count
                count+=1;rid=str(count);c.send(json.dumps(dict(version=1,requestId=rid,command=command,cameraId='CAM01',**fields)))
                while True:
                    result=c.receive()
                    if result.get('type')=='response' and result.get('requestId')==rid:return result
            for i in range(9):
                c=WebSocket(process.api_port);c.socket.settimeout(20);clients.append(c)
                r=call(c,'WEBRTC_START')
                if i<8:
                    assert r['ok'],r
                    assert r['data']['type']=='offer' and 'a=fingerprint:sha-256' in r['data']['sdp']
                    if i==0:owner=r['data']['clientId']
                else:assert r['error']['code']=='WEBRTC_BUSY'
            cross=call(clients[1],'WEBRTC_ANSWER',clientId=owner,sdp='invalid')
            assert cross['error']['code']=='WEBRTC_SESSION_NOT_FOUND'
            bad=call(clients[0],'WEBRTC_ANSWER',clientId=owner,sdp='invalid')
            assert not bad['ok']
            assert clients[0].request('GET_CAMERA_LIST')['ok']
            assert call(clients[0],'WEBRTC_STOP')['ok']
            assert call(clients[8],'WEBRTC_START')['ok'], 'Stop must release peer capacity'
            clients[8].send(json.dumps(dict(version=1,requestId='malformed',command='WEBRTC_START',cameraId=False)))
            while True:
                r=clients[8].receive()
                if r.get('requestId')=='malformed':assert r['error']['code']=='INVALID_REQUEST';break
            assert clients[8].request('GET_CAMERA_LIST')['ok']
            print('PASS native WebRTC SDP/session isolation/peer cap/stop/invalid request/WS responsiveness')
        finally:
            for c in clients:c.close()
            if process:process.cleanup()
            fixture.close()
if __name__=='__main__':main()
