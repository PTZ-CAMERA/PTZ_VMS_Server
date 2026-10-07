# VMS-managed RTSP 및 stream REST API

2026-10-07. 사용자 요청의 **2번 stream 조회와 3번 RTSP 재배포**를 구현했다.
기존 WebSocket camera list/status query 및 상태 알림은 유지한다.
Camera list/status REST endpoint는 이번 구현 범위에 포함되지 않는다.

## 주소 / 실행

| 종류 | 기본 주소 |
|---|---|
| stream REST | `http://127.0.0.1:5000/api/v1/cameras/CAM01/stream` |
| WebSocket | `ws://127.0.0.1:5000/ws` |
| VMS RTSP | `rtsp://127.0.0.1:8555/CAM01` |

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
./build/vms-server --config config/cam01.local.conf
```

인증된 기존 local 설정을 사용한다. 새 설정 key가 없어도 아래 기본값을 적용한다.

```ini
client_bind=127.0.0.1
client_port=5000
rtsp_relay_bind=127.0.0.1
rtsp_relay_port=8555
rtsp_public_host=127.0.0.1
```

HTTP/WS는 같은 port를 공유한다. Relay는 별도의 RTSP/TCP listener를 사용한다.
`rtsp_public_host`는 **VMS PC의 client 접근 주소**다. 원본 Pi 주소/계정에서 유도하지 않는다.
같은 PC에서 실행하는 Qt는 기본 loopback 주소를 사용할 수 있다. 다른 PC의 client를
지원할 때는 listener bind와 public host를 VMS PC의 접근 가능한 IP로 변경해야 한다.
예: VMS PC가 192.168.0.10이면 public host는 192.168.0.10이며 Pi의 192.168.0.92가 아니다.
기본은 localhost 개발용이며 외부 공개용 인증/TLS는 아직 없다.

## REST response

`GET /api/v1/cameras/{cameraId}/stream`. Camera ID는 ASCII 영숫자/underscore/hyphen.
JSON content type, cache-control no-store, HTTP connection close 방식으로 응답한다.
아래 값은 **응답 형식 예시이며 실제 재생 검사 결과가 아니다**.

HTTP 200:

```json
{
  "version": 1,
  "ok": true,
  "data": {
    "cameraId": "CAM01",
    "ready": true,
    "protocol": "rtsp",
    "transport": "tcp",
    "uri": "rtsp://127.0.0.1:8555/CAM01",
    "codec": "H264",
    "width": 1280,
    "height": 720,
    "fps": 30.0
  }
}
```

HTTP 503 (camera offline/connecting 또는 SPS/PPS/IDR 대기):

```json
{
  "version": 1,
  "ok": false,
  "error": {
    "code": "STREAM_NOT_READY",
    "message": "VMS stream is waiting for camera media and an IDR frame"
  },
  "data": {
    "cameraId": "CAM01",
    "ready": false,
    "protocol": "rtsp",
    "transport": "tcp",
    "uri": "rtsp://127.0.0.1:8555/CAM01",
    "codec": "",
    "width": 0,
    "height": 0,
    "fps": 0.0
  }
}
```

503 응답의 `Retry-After: 1`을 참고해 다시 조회할 수 있다. URI는 VMS 주소만 포함한다.
`ready=true`는 ingest ONLINE + relay의 SPS/PPS/IDR 준비 snapshot이다. 개별 client의
접속/디코딩 성공을 보장하는 playback 확인 값은 아니다. 주기적 snapshot 갱신으로
준비 상태 반영에 약 1~2초 지연이 생길 수 있다.

| HTTP / code | 의미 |
|---|---|
| 200 | stream ready, VMS URI 반환 |
| 503 / STREAM_NOT_READY | 등록 camera지만 relay 준비 중 |
| 404 / CAMERA_NOT_FOUND | 등록되지 않은 cameraId |
| 400 / INVALID_CAMERA_ID | 잘못된 cameraId |
| 404 / NOT_FOUND | 다른 REST 경로 |
| 405 / METHOD_NOT_ALLOWED | GET 외 method, Allow: GET |

Unknown camera 예시:

```json
{"version":1,"ok":false,"error":{"code":"CAMERA_NOT_FOUND","message":"Camera is not registered"}}
```

Pi의 RTSP URL과 credentials는 REST/WS/SDP에 포함하지 않는다. SDP의 Content-Base도
VMS public host / relay port / camera ID로 만든다.

## Data flow / ownership

```text
Pi RTSP H264
    ↓
RtspSession / FFmpeg AVPacket
    ├─ 기존 snapshot → HTTP + WebSocket
    └─ owned compressed access unit copy
             ↓ bounded queue
        RtspRelayServer
             ↓ NAL parsing + RTP packetization
        VMS RTSP/TCP → Qt / compatible RTSP player
```

`RtspRelayServer`는 별도 module이다. Qt dependency나 외부 service process를 추가하지 않았다.
기존 Boost.Asio와 FFmpeg utility API를 재사용한다. decode/encode/transcoding과 FFmpeg CLI
호출은 없다. Input AVFormatContext/AVPacket은 callback 이후 보관하지 않고 codec extradata,
time base와 압축 payload를 복사한다.

- Main: listener start/stop, configuration, stream URI 구성.
- RTSP ingest worker: configure / packet / offline task를 동일 queue에 순서대로 전달.
- Relay worker 하나: accept, RTSP parse/response, RTP/RTCP, media/session map 단일 소유.
- Queue mutex: configure/offline/resync/packet 전달과 bounded payload 관리.
- Ready mutex: 외부 snapshot query와 relay readiness 공유.
- 새로운 camera generation은 대기 중인 이전 packet/control을 대체한다.

동작 범위: H.264 video 한 track, RTSP 1.0, RTP/RTCP TCP interleaving.
Methods: OPTIONS / DESCRIBE / SETUP / PLAY / PAUSE / GET_PARAMETER / TEARDOWN.
UDP transport는 461 Unsupported Transport로 거부한다. Live stream이므로 과거 시각 seek는
지원하지 않는다. Browser native video playback은 RTSP로 해결되지 않으며 WebRTC는 별도 단계다.

RTP payload type 96, clock 90kHz, single NAL/FU-A fragmentation, access-unit marker,
SDP SPS/PPS 및 profile-level-id, RTCP Sender Report/SDES를 구현했다.
Annex B와 codec extradata가 avcC인 length-prefixed packet을 처리한다. 현재 Pi의
단일 SPS/PPS와 AVPacket당 한 access unit 경로를 대상으로 한다. 여러 parameter-set을
동시에 사용하는 일반 장치, audio/multi-track, RTSP 2.0, HTTP tunneling, authentication은
지원 범위가 아니다.

PLAY한 reader는 다음 IDR을 기다린다. IDR 전에 현재 SPS/PPS를 함께 전송한다.
따라서 첫 화면 대기 시간은 camera의 GOP/IDR 주기에 영향을 받는다. Keyframe을 Pi에
강제로 요청하는 기능은 아직 없다. 정상 PTS는 90kHz로 rescale하고, 없으면 steady clock을
사용한다. packet cadence는 ingest 수신 흐름을 따르며 별도 transcoding/pacing pipeline은 없다.

## 장애 / 자원 제한

- camera disconnect/reconfigure: 해당 reader를 닫고 relay readiness 해제.
  복구 후 client는 REST 조회와 RTSP 접속을 다시 수행해야 한다.
- SPS/PPS 변경: 기존 reader를 닫아 새 SDP/codec 정보로 재접속하게 한다.
- malformed/oversized access unit 또는 queue overflow: reader reset, 다음 IDR에서 재개.
- max 32 readers, cross-thread media queue 약 8MiB / packet limit 256.
- access unit 최대 4MiB, reader별 outgoing queue 최대 8MiB / 128 writes.
  Slow reader는 닫고 다른 reader/ingest는 유지한다.
- RTSP parser header/body 각 16KiB 제한, 수신 buffer 최대 128KiB.
- 미완료 연결은 약 5초 이후 timer가 정리한다. SETUP 후 keepalive timeout은 약 65초.
  RTSP request/receiver RTCP 수신이 activity를 갱신한다.
- Ctrl+C: HTTP/WS 종료 → ingest stop/join → relay socket/worker 종료.

## 이번 확인 결과

Linux Debug compile/link 성공. 기존 configuration/interface unit test 통과.
사용자가 이번 실제 재생 검증을 보류하여 **새 REST endpoint/relay의 통합 테스트와
VMS RTSP URI의 실제 디코딩·재생은 수행하지 않았다.** 기존 Pi ingest 성공 결과를
새 relay 재생 검증 결과로 대신 보고하지 않는다. Windows native도 미검증이다.

향후 재생 확인 시 VMS URI를 TCP transport로 열어야 한다. VLC의 RTSP-over-TCP 설정 또는
FFmpeg player의 TCP 옵션을 사용한다. Qt에는 REST response의 `data.uri`를 연결하면 된다.
Qt VmsClient의 REST 조회/영상 widget 연결은 이번 서버 2·3번 구현에 포함되지 않는다.
기존 Qt WebSocket 연결과 상태 UI는 유지한다.

## References

- [RTSP 1.0 / TCP interleaving, RFC 2326](https://www.rfc-editor.org/info/rfc2326/)
- [H.264 RTP payload / FU-A, RFC 6184](https://www.rfc-editor.org/info/rfc6184/)
- [구현 방식 결정](tech-decisions/0001-native-rtsp-relay.md)
