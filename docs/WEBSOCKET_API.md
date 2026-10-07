> 2026-10-08: DISCOVER_CAMERAS에 VMS RTSP 주소 미리보기를 추가했다. 검색은 ingest를 시작하지 않고 REGISTER_CAMERA 후 등록/수신한다. [결정 기록](tech-decisions/discovery-stream-preview.md)을 참조한다.
>
> 후속: VMS RTSP relay와 stream REST endpoint를 추가했다.
> HTTP와 WebSocket은 같은 5000 port를 사용한다. 기존 /ws 메시지 형식은 유지하며
> capabilities.live는 VMS RTSP relay 준비 시 true다. START_LIVE/STOP_LIVE command는 여전히
> 미지원이고 URI는 REST로 조회한다. [RTSP_RELAY_API.md](RTSP_RELAY_API.md)를 참조한다.
> 아래 false capability와 REST 미구현 설명은 최초 WebSocket 단계의 기록이다.

## Discovery address preview (v1 additive fields)

DISCOVER_CAMERAS의 data.devices는 기존 deviceServiceUrl/name/address/source에 다음 필드를 추가한다.

```json
{
  "deviceServiceUrl": "http://192.168.0.92:8080/onvif/device_service",
  "name": "Raspberry Pi PTZ",
  "address": "192.168.0.92",
  "source": "ws-discovery",
  "cameraId": "CAM01",
  "rtspUri": "rtsp://127.0.0.1:8555/CAM01",
  "registered": false,
  "ready": false
}
```

rtspUri는 VMS public host/relay port를 사용한 할당 주소이며 카메라 원본 URL이 아니다. registered=false는 미등록, ready=false는 재생 미준비 상태다. 검색에 의한 ID 예약은 스트림이나 녹화를 시작하지 않는다.

REGISTER_CAMERA에 선택 항목의 deviceServiceUrl 및 계정/프로파일을 보내면 예약된 cameraId로 등록한다. 응답 data에 기존 cameraId/profileToken/onvifStatus와 rtspUri를 제공한다. 이후 기존 REST stream endpoint의 ready 확인을 거쳐 Qt 영상을 시작한다. 예약은 서버 프로세스 동안 유지되므로 서버 재시작 시 재검색해야 한다. REST transport=tcp/udp 계약은 그대로 유지한다.

# VMS WebSocket API v1

2026-10-07. C++ VMS와 Qt Client 양쪽에 구현한 control/status API.
Endpoint: `ws://127.0.0.1:5000/ws`. Standard WebSocket text frame에 JSON object를 넣는다.
Socket.IO protocol은 사용하지 않는다. Browser의 native WebSocket에서도 같은 형식을 쓴다.

## 실행 / 화면

VMS root에서:

```sh
./build/vms-server --config config/cam01.local.conf
```

Qt 프로젝트를 다시 빌드하여 MiniVmsClient를 실행한다. DEVICE 탭에서 Host `127.0.0.1`,
Port `5000`, Connect를 누른다. Connect가 Dummy Mode를 끄고 WebSocket을 연결한다.
연결 후 camera list를 자동 조회하고 CAM01을 선택하면 상세 status를 조회한다.
CAMERA LIST 버튼은 VMS에 등록된 목록을 다시 조회한다. ONVIF WS-Discovery를 실행하는
버튼이 아니다. REFRESH는 선택 camera status query다.

수신한 ONLINE/CONNECTING/ERROR/OFFLINE, codec, resolution, FPS, time base, packet/byte
counter를 표시한다. Stream의 `Receiving (VMS)`는 서버 ingest 상태이며 Qt 화면의 LIVE는
아직 활성화하지 않는다. 실제 frame은 이번 WebSocket API로 보내지 않는다.

Disconnect/server disconnect 시 목록, 선택 camera, frame/detection/tracking, event/recording
표시를 정리한다. Dummy Mode를 다시 켜면 실소켓을 닫고 모의 데이터만 표시한다.
PTZ/tracking controls는 실연결에서 비활성화한다. 녹화/events/live 요청은 미지원 응답으로
SYSTEM LOG에 표시한다. SYSTEM LOG는 현재 Qt 연결/요청 결과 로그이며 Pi 또는 VMS console
전체 로그를 forwarding하지 않는다.

## Request / response

```json
{"version":1,"requestId":"1","command":"GET_CAMERA_LIST"}
```

```json
{"version":1,"requestId":"2","command":"GET_CAMERA_STATUS","cameraId":"CAM01"}
```

`requestId`는 client가 지정하는 비어 있지 않은 string(최대128자)이다. 정상 응답:

```json
{
  "version": 1,
  "type": "response",
  "requestId": "2",
  "ok": true,
  "data": {
    "camera": {
      "id": "CAM01",
      "name": "Raspberry Pi PTZ",
      "status": "ONLINE",
      "recording": false,
      "onvifStatus": "NOT_IMPLEMENTED",
      "webRtcStatus": "NOT_IMPLEMENTED",
      "codec": "H264",
      "width": 1280,
      "height": 720,
      "fps": 30.0,
      "timeBase": {"num": 1, "den": 90000},
      "packets": 120,
      "bytes": 1000000,
      "capabilities": {
        "ptz": false, "tracking": false, "live": false,
        "recordings": false, "events": false
      }
    }
  }
}
```

GET_CAMERA_LIST는 `data.cameras` array에 같은 camera object를 반환한다.
Credentials, camera RTSP URL, ONVIF credentials는 응답에 포함하지 않는다.
현재 등록 camera는 process의 configuration에서 만들어진 한 대이다. CameraManager/SQLite
등록 CRUD를 구현한 것은 아니다. Recording/tracking/live capabilities는 실제 backend
구현 시에만 true로 바꾼다. 별도 probe에서 ONVIF 접속을 확인한 사실은 runtime ONVIF
연결 상태가 아니므로 여기서는 NOT_IMPLEMENTED로 표시한다.

Error:

```json
{
  "version":1,"type":"response","requestId":"3","ok":false,
  "error":{"code":"NOT_SUPPORTED","message":"Command is not implemented by this VMS version"}
}
```

| Code | 의미 |
|---|---|
| INVALID_REQUEST | 잘못된 JSON/object/version/requestId/command/cameraId 또는 binary message |
| CAMERA_NOT_FOUND | 등록되지 않은 cameraId |
| NOT_SUPPORTED | PTZ_MOVE/PTZ_STOP/TRACKING_ON/OFF/START_LIVE/STOP_LIVE/GET_EVENTS/GET_RECORDINGS 등 미구현 command |

파싱 불가하거나 string requestId가 없으면 `requestId:null`로 error를 반환한다.
Server는 unsupported operation을 성공으로 반환하지 않는다. Qt는 응답 requestId로 pending
request를 찾고, 5초 timeout 또는 disconnect에 대해 TIMEOUT/DISCONNECTED를 local failure로
표시한다. INVALID_RESPONSE는 Qt의 response schema 검사 실패다.

## Notification

```json
{
  "version":1,"type":"notification","event":"CAMERA_STATUS","cameraId":"CAM01",
  "data":{"camera":{"id":"CAM01","name":"Raspberry Pi PTZ","status":"ERROR","recording":false,
    "onvifStatus":"NOT_IMPLEMENTED","webRtcStatus":"NOT_IMPLEMENTED","codec":"","width":0,"height":0,
    "fps":0,"timeBase":{"num":0,"den":1},"packets":0,"bytes":0,
    "capabilities":{"ptz":false,"tracking":false,"live":false,"recordings":false,"events":false}}}
}
```

1초마다 snapshot 변경을 확인해 연결된 client에 보낸다. 모든 짧은 상태 전환을 저장하는
event stream은 아니다. RTSP worker의 counter snapshot도 최대 1초에 한 번 갱신하므로
화면의 counter가 최신 수신보다 약 1~2초 늦을 수 있다. 정확한 지연 상한을 보장하지 않는다.
Counter는 session 누적 값이고 reconnect 시 초기화된다. OFFLINE/ERROR/CONNECTING으로
바뀌면 이전 stream 정보와 counter를 제거한다.

## Thread / resource model

- RTSP worker는 기존대로 동작한다. callback은 작은 CameraSnapshot을 mutex 아래 복사한다.
  Packet마다 JSON 작성/네트워크 전송을 하지 않는다.
- WebSocket은 Boost.Asio io_context worker 하나가 accept/HTTP upgrade/read/write/timer를
  비동기로 처리한다. camera map은 mutex, socket/session map과 write queue는 이 worker만 소유한다.
- 32 sessions, 64KiB inbound JSON, session당 64 outgoing message limit.
  Slow client queue overflow는 해당 socket만 닫는다. HTTP handshake 대기는 5초 제한.
- Qt는 UI event loop에서 QWebSocket으로 async connect/read/write를 처리한다.
  5초 connect timeout, 5초 request timeout, 5초 ping과 15초 activity timeout,
  64 pending request limit, 64KiB incoming frame/message limit을 둔다.
- 자동 재접속은 아직 없다. 끊긴 뒤 Connect로 재접속한다.
- VMS shutdown은 listener/client sockets를 취소·닫고 network worker join 후 RTSP를 종료한다.
  Server shutdown/queue overflow는 bounded 종료를 위해 TCP socket을 닫는다.

기본 bind는 loopback이다. 현재 authentication/TLS/HTTP REST는 미구현이며 이 단계는
같은 PC의 client 연결용이다. LAN 배포 시 bind뿐 아니라 auth/TLS/접근 정책을 추가한다.

## Configuration / build

기존 RTSP 설정에 다음 key를 추가할 수 있다. 없으면 아래 기본값을 사용한다.

```ini
client_bind=127.0.0.1
client_port=5000
```

Bind는 numeric IP address, port는 1..65535이다. Invalid address/port in use이면 startup을
실패로 보고한다. Camera ingest failure와는 별개다.

Linux VMS dependencies:

```sh
sudo apt-get install cmake g++ pkg-config libavformat-dev libavcodec-dev libavutil-dev \
  libboost-dev nlohmann-json3-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Boost >=1.70 header package와 nlohmann_json >=3.2 CMake package를 사용한다.
Boost.System은 header-only 설정이다. Windows에서는 기존 FFmpeg SDK 외에 해당 Boost
headers와 nlohmann_json CMake package를 제공하고 필요 시 CMAKE_PREFIX_PATH로 지정한다.
Qt server dependency는 없다. Qt Client는 Widgets/Network/WebSockets module이 필요하다.
현재 Windows Qt 6.11.2 MSVC SDK에는 WebSockets CMake package가 존재함을 확인했다.

Qt Linux 검증 build:

```sh
cmake -S /mnt/c/PTZ_Project_Qt -B /tmp/ptz-qt-websocket-build \
  -DCMAKE_BUILD_TYPE=Debug -DPTZ_BUILD_TESTS=ON -DPTZ_BUILD_LEGACY_CLIENT=OFF
cmake --build /tmp/ptz-qt-websocket-build --parallel
VMS_SERVER_TEST_EXECUTABLE=/mnt/c/PTZ_VMS_Server/build/vms-server \
VMS_TEST_CAMERA_CONFIG=/mnt/c/PTZ_VMS_Server/config/cam01.local.conf \
ctest --test-dir /tmp/ptz-qt-websocket-build --output-on-failure
```

VMS_SERVER_TEST_EXECUTABLE이 없으면 실제 VMS 통합 case는 skip하고 mock/UI tests를 실행한다.
VMS_TEST_CAMERA_CONFIG가 없으면 실제 서버의 unreachable camera configuration으로 검사한다.
Windows native build와 WSL/Windows localhost forwarding은 이번 Linux 검사에서 확인하지 않았다.

## Tests

- VMS: config/interface tests, existing RTSP fixture, WebSocket integration.
- WebSocket integration: 2 clients, queries/error correlation, malformed/binary JSON,
  version rejection, fragmented RFC6455 request, stream status/counter notifications,
  camera stall while API stays alive, client reconnect, oversized message, half-open HTTP
  handshake and server shutdown.
- Qt: existing UI tests + VmsWebSocketTests mock query/notification/error,
  request timeout/reconnect, real/Dummy mode isolation, disconnect cleanup,
  optional actual C++ VMS/Pi integration.

## References / technology rationale

- [Boost.Beast WebSocket](https://www.boost.org/latest/libs/beast/doc/html/beast/using_websocket.html)
- [Qt QWebSocket](https://doc.qt.io/qt-6/qwebsocket.html)
- Qt decision record: `C:/PTZ_Project_Qt/PTZCamera/docs/tech-decisions/0006-vms-websocket-api.md`.

WebSocket을 선택한 이유는 Qt/browser가 같은 JSON request/response와 상태 push를
사용하기 때문이다. 자체 TCP는 browser bridge/framing이 필요하고 HTTP-only는 notification
polling이 필요하다. Server에 Qt 의존성을 추가하지 않고 C++/CMake worker 경계를 유지한다.

## 실제 검증 결과 (2026-10-07)

- Linux GNU 13.3, Boost 1.83, nlohmann_json 3.11.3, FFmpeg 6.1.1: VMS Debug build 성공.
- VMS CTest 3/3 통과. 별도 ASan/UBSan build + leak detection도 같은 tests 3/3 통과.
- Linux Qt 6.4.2: MiniVmsClient, MiniVmsUiTests, VmsWebSocketTests 실행 파일 링크 성공.
- Qt CTest 2/2 통과 (실제 VMS/Pi integration 포함, skipped case 없음).
- 실제 Pi → C++ VMS → QWebSocket → MainWindow: CAM01 ONLINE,
  H264 1280x720 30FPS, packet counter `1 -> 49` 증가를 확인했다.
  Server 정상 종료 후 Qt disconnected와 빈 camera list를 확인했다.
- 기존 UI test에서 발견된 window teardown 중 keyboard/focus signal의 derived-controller
  호출 문제는 MainWindow destructor에서 입력 정지/소켓 종료/child signal detach로 수정했다.
- Windows native build/화면 실행, 영상 display, PTZ control, 녹화와 events backend는 미검증/미구현.
  Server와 Qt의 WebSocket 연결 코드는 Linux 실행으로 검증했다.
