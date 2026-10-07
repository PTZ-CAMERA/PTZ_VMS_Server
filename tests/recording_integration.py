"""Exercise segment remux, metadata, reconnect, errors, and active-recording shutdown."""
from pathlib import Path
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from rtsp_integration import Fixture, Process
from websocket_test_client import WebSocket


def await_status(client, predicate, timeout=12):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        result = client.request('GET_CAMERA_STATUS', 'CAM01')['data']['camera']
        if predicate(result):
            return result
        time.sleep(.1)
    raise AssertionError('Recording state timeout: ' + json.dumps(result))


def run_case(executable, sample, root, container='mkv', broken=False):
    fixture = Fixture(sample)
    p = None
    client = None
    try:
        p = Process(executable, root, fixture.port)
        # Restart before any camera/recording command to set recorder configuration.
        p.wait_for('Listening ws://'); p.stop(); p.cleanup()
        config = root / f'vms-{fixture.port}.conf'
        text = config.read_text().replace('recording_root=' + str(root / 'recordings'), 'recording_root=' + str(root / ('blocked' if broken else 'recordings')))
        config.write_text(text + f'recording_container={container}\nrecording_segment_seconds=1\n')
        if broken:
            (root / 'blocked').write_text('not a directory')
        p.proc = subprocess.Popen([executable, '--config', str(config)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == 'nt' else 0)
        p.lines = []
        import threading
        p.thread = threading.Thread(target=p.read, daemon=True); p.thread.start()
        p.wait_for('Listening ws://')
        client = WebSocket(p.api_port)
        await_status(client, lambda c: c['status'] == 'ONLINE')
        assert client.request('START_RECORDING', 'CAM01')['ok']
        if broken:
            status = await_status(client, lambda c: c['recordingState'] == 'ERROR')
            assert not status['recording'] and status['status'] == 'ONLINE'
            assert client.request('GET_CAMERA_LIST')['ok']
            print('PASS recording disk/path error isolated from live ingest and API')
            p.stop()
            return
        await_status(client, lambda c: c['recording'])
        time.sleep(2.5)
        assert client.request('STOP_RECORDING', 'CAM01')['ok']
        await_status(client, lambda c: not c['recording'] and not c['recordingRequested'])
        rows = client.request('GET_RECORDINGS', 'CAM01')['data']['recordings']
        assert len(rows) >= 2, rows
        assert client.request('GET_RECORDINGS', 'CAM99')['data']['recordings'] == []
        client.send(json.dumps({'version':1, 'requestId':'outside', 'command':'GET_RECORDINGS', 'cameraId':'CAM01', 'fromMs':0, 'toMs':1}))
        while True:
            result = client.receive()
            if result.get('requestId') == 'outside': break
        assert result['ok'] and result['data']['recordings'] == []
        point = rows[0]['startTimeMs'] + 1
        client.send(json.dumps({'version':1, 'requestId':'point', 'command':'GET_RECORDINGS', 'cameraId':'CAM01', 'fromMs':point, 'toMs':point+1}))
        while True:
            result = client.receive()
            if result.get('requestId') == 'point': break
        assert any(row['id'] == rows[0]['id'] for row in result['data']['recordings'])
        for row in rows:
            path = Path(row['filePath'])
            assert path.exists() and path.suffix == '.' + container and row['duration'] > 0
            assert row['codec'] == 'H264' and row['width'] == 1280 and row['height'] == 720
            if len(sys.argv) > 3:
                probe = json.loads(subprocess.check_output([sys.argv[3], '-v', 'error', '-select_streams', 'v:0', '-count_frames', '-show_entries', 'stream=codec_name,width,height,nb_read_frames', '-of', 'json', str(path)]))['streams'][0]
                assert probe['codec_name'] == 'h264' and int(probe['nb_read_frames']) > 0
        if container == 'mkv':
            assert client.request('START_RECORDING', 'CAM01')['ok']
            await_status(client, lambda c: c['recording'])
            fixture.disconnect()
            await_status(client, lambda c: c['recordingRequested'] and not c['recording'])
            await_status(client, lambda c: c['recording'], timeout=15)
            before = len(client.request('GET_RECORDINGS', 'CAM01')['data']['recordings'])
            p.stop() # Active segment must be finalized by shutdown.
            import sqlite3
            with sqlite3.connect(root / 'vms.db') as db:
                count = db.execute('SELECT count(*) FROM recordings').fetchone()[0]
            assert count > before
            assert not list((root / 'recordings').rglob('*.part'))
        else:
            p.stop()
        print('PASS', container, 'segments, independent frames, SQLite, start/stop' + (', reconnect and active shutdown' if container == 'mkv' else ''))
    finally:
        if client: client.close()
        if p: p.cleanup()
        fixture.close()


def main():
    executable = str(Path(sys.argv[1]).resolve())
    sample = Path(sys.argv[2]).read_bytes()
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        for kind in ('mkv', 'mp4', 'error'):
            folder = root / kind; folder.mkdir()
            run_case(executable, sample, folder, container='mp4' if kind == 'mp4' else 'mkv', broken=kind == 'error')


if __name__ == '__main__':
    main()
