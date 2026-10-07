# CAM01 실제 장비 연결 검증

검증일: 2026-10-07. Linux 환경의 VMS executable 및 일회성 ONVIF SOAP probe 사용.

## 연결 정보

- Camera ID: CAM01
- ONVIF Device: `http://192.168.0.92:8080/onvif/device_service`
- Media: `http://192.168.0.92:8080/onvif/media_service`
- PTZ: `http://192.168.0.92:8080/onvif/ptz_service`
- Events: `http://192.168.0.92:8080/onvif/events_service`
- RTSP: `rtsp://192.168.0.92:8554/cam`
- Profile token: `main`
- 제공한 계정으로 ONVIF HTTP Digest SHA-256 인증 성공. 비밀번호는 문서에 기록하지 않음.
- `config/vms.conf`의 기본 RTSP 경로를 `/stream`에서 `/cam`으로 변경.
- 인증 정보가 포함된 실행 설정은 `config/cam01.local.conf`에 보관.
  기존 `.gitignore`의 `config/*.local.conf` 패턴에 포함됨.

## RTSP 결과

기존 C++ VMS를 인증 설정으로 약 18초 실행하고 SIGINT로 종료했다.

```text
Codec: H264
Resolution: 1280x720
FPS: 30 (30/1)
Time Base: 1/90000
Packets received=214 bytes=2274948 corrupt/dropped=0
Packets received=369 bytes=3888448 corrupt/dropped=0
Session packets=469
Shutdown complete
Exit code: 0
```

카메라의 packet 수신 성공을 확인했다. 전체 bitstream의 decode 검증이나 장시간
soak test, 녹화, 화면 표시를 검증한 것은 아니다.

실행:

```sh
./build/vms-server --config config/cam01.local.conf
```

## ONVIF 결과

| 요청 | 결과 |
|---|---|
| GetDeviceInformation | 200, Raspberry Pi PTZ / firmware 0.2 / RaspberryPi4 |
| GetSystemDateAndTime | 200, UTC 시각 반환 |
| GetCapabilities | 200, Media/PTZ/Events endpoint 반환 |
| GetProfiles | 200, token main, Pi PTZ 720p, H264 1280x720, FrameRateLimit 30 |
| GetStreamUri (RTP_unicast, RTSP) | 200, rtsp://192.168.0.92:8554/cam |
| GetStatus | 200, normalized pan/tilt 0, IDLE |
| GetEventProperties | 200, Publishing / State / PersonDetection topic |
| CreatePullPointSubscription | 200 |
| PullMessages | 200, 상태 알림 3개 수신 |
| Unsubscribe | 200, 시험 구독 해제 |

ONVIF probe는 HTTP Digest SHA-256을 지원하는 curl로 SOAP를 전송했다.
Python 기본 urllib Digest handler는 이 알고리즘을 지원하지 않아 사용 도구를 변경했다.
curl의 credentials는 stdin config로 전달하고 출력하지 않았다. 이는 검증 도구이며
C++ OnvifClient의 구현을 추가한 것이 아니다. PTZ 이동 명령은 실행하지 않았다.

수신한 초기 상태 알림:

- `pc:Camera/Publishing`: ProfileToken=main, Connected=false, Available=true
- `pc:Tracking/State`: ProfileToken=main, Enabled=false, Available=true
- `pc:Analytics/PersonDetection`: ProfileToken=main, Detected=false, Available=true

이 값은 이벤트 조회 시점의 Pi 응답이다. `Connected`의 정확한 의미는 Pi 문서/코드로
확인해야 한다. 값 하나로 RTSP 영상이 중단되었다고 해석하지 않는다. 실제 사람이 들어왔을 때
상태 변경, confidence/bbox/error/pan/tilt 등의 상세 payload는 이번에 검증하지 않았다.

## GetStreamUri 호환성 확인

`<tt:Stream>RTP-Unicast</tt:Stream>`와 RTSP transport로 요청하면 HTTP 500 SOAP Fault:

```text
s:Sender ter:InvalidArgVal
Only RTP_unicast over RTSP or UDP supported
```

`<tt:Stream>RTP_unicast</tt:Stream>`로 바꾸면 성공한다. 공식 ONVIF XML schema의
StreamType enum과 Programmer's Guide 예시는 `RTP-Unicast`를 사용한다.
Pi 구현이 해당 표현도 받아들이도록 수정하거나, 향후 VMS adapter에서 장치별
호환 fallback을 넣을지 결정해야 한다. 현재 VMS 본체에는 fallback을 구현하지 않았다.

- [ONVIF StreamSetup / StreamType schema viewer](https://www.onvif.org/yaml/wsdl-viewer.php?file=%2Fver10%2Freplay.wsdl)
- [ONVIF Application Programmer's Guide](https://www.onvif.org/wp-content/uploads/2016/12/ONVIF_WG-APG-Application_Programmers_Guide-1.pdf)

## 범위와 남은 작업

이번에 실제 Pi와의 authenticated ONVIF 조회, event pull, C++ VMS RTSP 수신을 확인했다.
VMS에는 아직 CameraManager 등록 DB나 자동 ONVIF→GetStreamUri→ingest 연결이 없으므로
VMS 실행은 local RTSP configuration을 사용한다. ONVIF는 별도의 일회성 probe로 검증했다.
WS-Discovery 자동 검색, PTZ 이동, metadata 저장, GetSystemLog, client API, WebRTC,
Windows native 실행은 이번 검증 범위에 포함되지 않는다.

`docs/EDGE_CAMERA_IMPLEMENTATION.md`는 현재 VMS 폴더 및 검색한 인접 프로젝트에
존재하지 않아 읽지 못했다. 위 결과는 실제 서비스 응답으로 확인한 것이며, Pi 구현
문서 전체의 지원 기능을 추정하지 않는다.
