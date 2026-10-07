"""RTSP UDP negotiation, RTP/RTCP, mixed clients and real FFmpeg decoding."""
from pathlib import Path
import json
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
import urllib.error
from rtsp_integration import Fixture, Process


class Reader:
    def __init__(self, port, transport):
        self.socket = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.socket.settimeout(5)
        self.stream = self.socket.makefile('rb')
        self.uri = f'rtsp://127.0.0.1:{port}/CAM01?transport={transport}'
        self.cseq = 0
        self.session = None
        self.media = []

    def response(self):
        while True:
            first = self.stream.read(1)
            if not first: raise EOFError('RTSP connection ended')
            if first == b'$':
                header = self.stream.read(3)
                length = int.from_bytes(header[1:], 'big')
                self.media.append((header[0], self.stream.read(length)))
                continue
            status = (first + self.stream.readline()).decode().strip()
            headers = {}
            while True:
                line = self.stream.readline()
                if line == b'\r\n': break
                if not line: raise EOFError('RTSP header ended')
                key, value = line.decode().split(':', 1); headers[key.lower()] = value.strip()
            body = self.stream.read(int(headers.get('content-length', 0)))
            return int(status.split()[1]), headers, body

    def request(self, method, headers=None, track=False):
        self.cseq += 1
        uri = self.uri.replace('/CAM01?', '/CAM01/trackID=0?') if track else self.uri
        fields = {'CSeq': str(self.cseq)}
        if self.session: fields['Session'] = self.session
        fields.update(headers or {})
        self.socket.sendall((f'{method} {uri} RTSP/1.0\r\n' + ''.join(f'{k}: {v}\r\n' for k,v in fields.items()) + '\r\n').encode())
        return self.response()

    def next_tcp_rtp(self):
        end = time.monotonic() + 5
        while time.monotonic() < end:
            for channel, data in self.media:
                if channel == 0: return data
            self.response()
        raise AssertionError('No TCP RTP')

    def close(self):
        self.stream.close(); self.socket.close()


def bind_udp_pair():
    for _ in range(100):
        rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rtcp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            rtp.bind(('127.0.0.1', 0)); port = rtp.getsockname()[1]
            if port % 2 or port == 65535:
                rtp.close(); rtcp.close(); continue
            rtcp.bind(('127.0.0.1', port + 1))
            rtp.settimeout(5); rtcp.settimeout(5)
            rtp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
            return rtp, rtcp
        except OSError:
            rtp.close(); rtcp.close()
    raise RuntimeError('No client UDP pair')


def main():
    fixture = Fixture(Path(sys.argv[2]).read_bytes())
    p = None; clients=[]; rtp=None; rtcp=None
    with tempfile.TemporaryDirectory() as temp:
        try:
            p = Process(str(Path(sys.argv[1]).resolve()), Path(temp), fixture.port)
            p.wait_for('Listening ws://')
            api = f'http://127.0.0.1:{p.api_port}/api/v1/cameras/CAM01/stream'
            for _ in range(100):
                try:
                    result=json.load(urllib.request.urlopen(api+'?transport=udp', timeout=2))
                    assert result['data']['ready'] and result['data']['transport']=='udp'
                    assert result['data']['uri'].endswith('?transport=udp')
                    assert result['data']['transports']==['tcp','udp']
                    break
                except urllib.error.HTTPError: time.sleep(.1)
            else: raise AssertionError('Relay not ready')
            try: urllib.request.urlopen(api+'?transport=invalid')
            except urllib.error.HTTPError as error: assert error.code==400
            else: raise AssertionError('Invalid transport accepted')
            rtp,rtcp=bind_udp_pair()
            reader=Reader(p.relay_port,'udp'); clients.append(reader)
            assert reader.request('DESCRIBE')[0]==200
            for bad in ('RTP/AVP;unicast', 'RTP/AVP;unicast;client_port=0-1', 'RTP/AVP;unicast;client_port=70000-70001',
                        'RTP/AVP;multicast;client_port=12340-12341', 'RTP/AVP/TCP;unicast;interleaved=0-1',
                        'RTP/AVP;unicast;client_port=12340-12341;destination=127.0.0.2'):
                assert reader.request('SETUP',{'Transport':bad},track=True)[0]==461
            ports=f'{rtp.getsockname()[1]}-{rtcp.getsockname()[1]}'
            code,headers,_=reader.request('SETUP',{'Transport':'RTP/AVP/UDP;unicast;client_port='+ports},track=True)
            assert code==200 and 'client_port='+ports in headers['transport']
            server_rtp,server_rtcp=map(int,re.search(r'server_port=(\d+)-(\d+)',headers['transport']).groups())
            assert server_rtp%2==0 and server_rtcp==server_rtp+1
            reader.session=headers['session'].split(';')[0]
            assert reader.request('PLAY')[0]==200
            packet,source=rtp.recvfrom(65536)
            assert source[1]==server_rtp and packet[0]>>6==2 and packet[1]&127==96
            ssrc=int.from_bytes(packet[8:12],'big')
            assert ssrc==int(re.search(r'ssrc=([0-9a-f]+)',headers['transport']).group(1),16)
            got_idr=False
            for _ in range(50):
                nal=packet[12:]
                got_idr=got_idr or nal[0]&31==5 or (nal[0]&31==28 and nal[1]&31==5)
                if got_idr:break
                packet,source=rtp.recvfrom(65536)
            assert got_idr
            report,source=rtcp.recvfrom(65536)
            assert source[1]==server_rtcp and report[1]==200 and int.from_bytes(report[4:8],'big')==ssrc
            assert int.from_bytes(report[20:24],'big')>0
            rtcp.sendto(struct.pack('!BBHI',0x80,201,1,0x12345678),('127.0.0.1',server_rtcp))
            assert reader.request('GET_PARAMETER')[0]==200
            assert reader.request('PAUSE')[0]==200
            rtp.settimeout(.15)
            try:
                while True:rtp.recvfrom(65536)
            except socket.timeout:pass
            try:rtp.recvfrom(65536)
            except socket.timeout:pass
            else:raise AssertionError('Media continues while paused')
            rtp.settimeout(5);assert reader.request('PLAY')[0]==200
            assert rtp.recvfrom(65536)[0]
            tcp=Reader(p.relay_port,'tcp');clients.append(tcp)
            assert tcp.request('DESCRIBE')[0]==200
            assert tcp.request('SETUP',{'Transport':'RTP/AVP;unicast;client_port='+ports},track=True)[0]==461
            code,headers,_=tcp.request('SETUP',{'Transport':'RTP/AVP/TCP;unicast;interleaved=0-1'},track=True)
            assert code==200;tcp.session=headers['session'].split(';')[0]
            assert tcp.request('PLAY')[0]==200
            tcp.socket.sendall((f'GET_PARAMETER {tcp.uri} RTSP/1.0\r\nCSeq: 99\r\nSession: {tcp.session}\r\n\r\n').encode())
            assert tcp.response()[0]==200
            for _ in range(20):
                if any(ch==0 for ch,_ in tcp.media):break
                time.sleep(.05)
                tcp.request('GET_PARAMETER')
            assert any(ch==0 for ch,_ in tcp.media)
            if len(sys.argv)>3:
                for mode in ('udp','tcp'):
                    uri=f'rtsp://127.0.0.1:{p.relay_port}/CAM01?transport={mode}'
                    probe=subprocess.run([sys.argv[3],'-v','error','-rtsp_transport',mode,'-read_intervals','%+0.3','-count_frames','-select_streams','v:0',
                        '-show_entries','stream=codec_name,width,height,nb_read_frames','-of','json',uri],capture_output=True,text=True,timeout=15)
                    assert probe.returncode==0,probe.stderr
                    info=json.loads(probe.stdout)['streams'][0]
                    assert info['codec_name']=='h264' and info['width']==1280 and int(info['nb_read_frames'])>0
            assert reader.request('TEARDOWN')[0]==200
            p.stop()
            print('PASS UDP RTP/RTCP ports/SSRC/IDR/SR/RR, invalid transports, PAUSE/PLAY, mixed TCP, FFmpeg decoding and shutdown')
        finally:
            for client in clients:client.close()
            if rtp:rtp.close()
            if rtcp:rtcp.close()
            if p:p.cleanup()
            fixture.close()


if __name__=='__main__':main()
