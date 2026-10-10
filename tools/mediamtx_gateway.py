"""PC WebRTC gateway: 기존 VMS RTSP만 읽는다. Pi 직접 연결·녹화·재인코딩은 하지 않는다."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = '1.21.2'
PACKAGES = {
    'linux': ('tar.gz', '121be6e00397e473a95d83c75f4fcd3819a6af3b0e1a50889cba43ab4fab6a27'),
    'windows': ('zip', 'a84224f5a490855684e62ba6fb71e80ea2a5e16aed61883bb58380290e93e467'),
}

def install(target):
    extension, digest = PACKAGES[target]
    url = f'https://github.com/bluenviron/mediamtx/releases/download/v{VERSION}/mediamtx_v{VERSION}_{target}_amd64.{extension}'
    data = urllib.request.urlopen(url, timeout=60).read()
    if hashlib.sha256(data).hexdigest() != digest:
        raise RuntimeError('MediaMTX package checksum mismatch')
    name = 'mediamtx.exe' if target == 'windows' else 'mediamtx'
    # 검증한 실행 파일만 추출해 archive 경로 traversal을 피한다.
    if target == 'windows':
        with zipfile.ZipFile(io.BytesIO(data)) as archive: binary = archive.read(name)
    else:
        with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive: binary = archive.extractfile(name).read()
    folder = ROOT / 'build' / 'gateway' / target
    folder.mkdir(parents=True, exist_ok=True)
    executable = folder / name
    executable.write_bytes(binary)
    if target == 'linux': executable.chmod(0o755)
    print(f'MediaMTX {VERSION} installed: {executable}')
    return executable

def configuration(relay_host, relay_port, cameras, http_port=8889, ice_port=8189):
    if not re.fullmatch(r'[A-Za-z0-9.:-]+', relay_host) or not 1 <= relay_port <= 65535:
        raise ValueError('Invalid VMS relay endpoint')
    if not cameras or any(not re.fullmatch(r'[A-Za-z0-9_-]+', camera) for camera in cameras):
        raise ValueError('Invalid camera IDs')
    host = f'[{relay_host}]' if ':' in relay_host else relay_host
    return {
        'logLevel': 'warn', 'rtsp': False, 'rtmp': False, 'hls': False, 'srt': False, 'moq': False,
        'api': False, 'metrics': False, 'pprof': False, 'playback': False,
        'webrtc': True, 'webrtcAddress': f'127.0.0.1:{http_port}',
        'webrtcLocalUDPAddress': f'127.0.0.1:{ice_port}', 'webrtcLocalTCPAddress': f'127.0.0.1:{ice_port}',
        'webrtcAdditionalHosts': ['127.0.0.1'], 'webrtcIPsFromInterfaces': False,
        'webrtcAllowOrigins': ['http://localhost:5173', 'http://127.0.0.1:5173'],
        'authInternalUsers': [{'user': 'any', 'ips': ['127.0.0.1', '::1'], 'permissions': [{'action': 'read', 'path': ''}]}],
        'paths': {camera: {'source': f'rtsp://{host}:{relay_port}/{camera}', 'rtspTransport': 'tcp',
                          'sourceOnDemand': True, 'sourceOnDemandStartTimeout': '15s', 'record': False} for camera in cameras},
    }

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--install', action='store_true')
    parser.add_argument('--platform', choices=PACKAGES, default='windows' if os.name == 'nt' else 'linux')
    parser.add_argument('--config', type=Path, default=ROOT / 'config/cam01.local.conf')
    parser.add_argument('--camera', action='append')
    parser.add_argument('--relay-host')
    parser.add_argument('--relay-port', type=int)
    args = parser.parse_args()
    if platform.machine().lower() not in ('amd64', 'x86_64'):
        raise SystemExit('Pinned gateway packages target x64 PCs')
    executable = ROOT / 'build/gateway' / args.platform / ('mediamtx.exe' if args.platform == 'windows' else 'mediamtx')
    if args.install: install(args.platform); return
    if not executable.exists(): raise SystemExit('Run with --install first')
    values = {}
    for line in args.config.read_text(encoding='utf-8').splitlines():
        key, separator, value = line.partition('=')
        # 인증·Gemini 키는 읽어 저장하지 않는다. gateway에는 VMS relay 주소만 필요하다.
        if separator and key.strip() in ('rtsp_relay_bind', 'rtsp_relay_port', 'camera_id'): values[key.strip()] = value.strip()
    host = args.relay_host or values.get('rtsp_relay_bind', '127.0.0.1')
    if host in ('0.0.0.0', '::'): host = '127.0.0.1'
    settings = configuration(host, args.relay_port or int(values.get('rtsp_relay_port', 8555)), args.camera or [values.get('camera_id', 'CAM01')])
    generated = ROOT / 'build/gateway/mediamtx-pc.json'
    generated.parent.mkdir(parents=True, exist_ok=True)
    generated.write_text(json.dumps(settings, indent=2), encoding='utf-8')
    config_path = str(generated)
    if os.name != 'nt' and args.platform == 'windows':
        config_path = subprocess.check_output(['wslpath', '-w', config_path], text=True).strip()
    print('PC gateway: WHEP HTTP :8889 / ICE UDP+TCP :8189; source=VMS RTSP', flush=True)
    child = subprocess.Popen([str(executable), config_path])
    try: child.wait()
    except KeyboardInterrupt:
        child.terminate(); child.wait(timeout=10)
    raise SystemExit(child.returncode)

if __name__ == '__main__': main()
