# VMS 문서 안내

## 현재 구현부터 읽기

1. [실행·설정·기능](../README.md)
2. [Pi / VMS / Qt / Web 전체 구조](SYSTEM_ARCHITECTURE.md)
3. [VMS 모듈·스레드·저장 구조](VMS_ARCHITECTURE.md)
4. [코드 읽는 순서](CODE_GUIDE.md)

## 기능별 계약

| 주제 | 문서 |
|---|---|
| 라이브·내장 WebRTC | [LIVE_ROUTING](LIVE_ROUTING.md) |
| WS / RTSP relay API | [WEBSOCKET_API](WEBSOCKET_API.md), [RTSP_RELAY_API](RTSP_RELAY_API.md) |
| 녹화 정책·진단 | [AUTO_RECORDING](AUTO_RECORDING.md), [DETECTION_SEARCH_READINESS](DETECTION_SEARCH_READINESS.md) |
| metadata·시간·검색 | [METADATA_VIDEO_SEARCH](METADATA_VIDEO_SEARCH.md), [CHAT_SEARCH](CHAT_SEARCH.md) |
| PTZ·Tracking | [TRACKING_CONTROL](TRACKING_CONTROL.md) |
| 기술 선택 | [tech-decisions](tech-decisions/) |

## 기록과 생성물

[CAM01_CONNECTION_TEST](CAM01_CONNECTION_TEST.md)는 당시 연결 시험 기록이다.
현재 기능의 근거와 과거 시험 조건을 섞지 않는다. 소스·설정의 Git 이력은 보존한다.

실행 생성물은 `build/`, `data/`, `recordings/`이며 Git에서 제외한다. 로컬 자격정보도
제외된 설정에서 읽는다. 기존 실행 경로를 바꾸기 위한 문서 정리가 아니다.
