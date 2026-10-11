# VMS 코드 구조와 읽는 순서

## 책임 경계

`include/`는 모듈 계약, `src/`는 실행 구현, `tests/`는 모의 장치와 검증, `tools/`는 SDK 준비, `docs/`는 계약·설계·작업 기록이다. 초기 설계와 실제 구현을 구분하며 운영 경로를 문서만 보고 추측하지 않는다.

| 모듈 | 책임 / 진입 파일 |
|---|---|
| camera | 조립·등록·카메라 세대: src/camera/CameraService.cpp |
| stream | 카메라별 FFmpeg 수신 / 별도 RTSP 중계 |
| recording | 파일·muxer·수동/자동 정책: RecordingManager.cpp |
| event | ONVIF 수신 worker와 DB 작업 전달: EventManager.cpp |
| database | SQLite 연결·저장·검색·재생 위치: RecordingRepository / EventRepository |
| client | HTTP·WS 파싱·응답·세션: WebSocketServer.cpp |
| ptz | 명령 소유권·순서·정지: PtzCommandRouter.cpp |
| webrtc | source/filter/peer 소유·SDP·H.264 전송: WebRtcGateway.cpp |
| chat | 모델 조건 검증·세션별 검색: ChatSearchService.cpp |
| onvif / core | 통신 adapter / 설정·UTC·로그 |

EventRepository의 실제 헤더는 `include/database/EventRepository.h`로 구현 폴더와 맞췄다. `include/event/EventRepository.h`는 이전 호출부의 include를 위한 호환 헤더다. CameraManager/DatabaseManager/PlaybackManager는 초기 추상 설계이며 현재 런타임 manager로 설명하지 않는다.

## 코드 읽는 순서

1. main.cpp → CameraService: 시작·종료·모듈 연결.
2. RtspSession → RecordingManager/MuxRecorder: AVPacket 소유권과 remux.
3. WebSocketServer → PtzCommandRouter: 비동기 통신과 지연 작업.
4. EventManager → EventRepository: 상태/샘플/중복/시간 연결.
5. ChatSearchService: LLM 제안의 검증과 실제 DB 결과.
6. WebRtcGateway: worker·library callback·약한 참조·peer 수명.

## 유지해야 할 규칙

- 수신·API 호출부는 muxer나 SQLite 연결을 직접 조작하지 않는다. 담당 worker에 작업을 전달한다.
- DB와 미디어 queue는 제한되며 초과/연결 실패를 정상 영상으로 숨기지 않는다.
- 원본 UTC, 수신 UTC, 영상 PTS, 타이머용 steady_clock을 구분한다. 누락 값은 추측하지 않는다.
- eventId와 sampleId는 별도 ID 공간이다. 미완성/삭제/범위 밖 녹화는 재생 불가다.
- SDP는 해당 WS 세션에만 보낸다. socket 종료와 ICE 완료가 경쟁해도 요청 응답은 한 번이다.
- 모델은 검색 조건만 제안하며 SQL·파일 존재·재생 가능 여부는 VMS가 확인한다.

## 문서와 디렉토리 관리

[문서 안내](README.md)에서 현재 계약과 초기 설계를 구분한다. 기존 헤더 호환 경로,
CMake 대상, 설정·DB·녹화 경로는 유지한다. 이번 정리는 현재 작업 트리를 기준으로
주석과 문서만 보완하며 기존 미커밋 기능 변경을 덮어쓰지 않는다.

[전체 시스템](SYSTEM_ARCHITECTURE.md) · [영상 경로](LIVE_ROUTING.md) · [자동 녹화](AUTO_RECORDING.md)
