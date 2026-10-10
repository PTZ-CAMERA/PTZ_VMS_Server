# 탐지·녹화 검색 상태 확인

## 변경 전 복구 기준

2026-10-10에 세 저장소 main에 커밋·푸시한 기준이다. 로컬 설정·DB·영상은 Git에 포함하지 않는다.

- VMS: `38abe85` — https://github.com/PTZ-CAMERA/PTZ_VMS_Server/commit/38abe85
- Qt: `b3ed009` — https://github.com/PTZ-CAMERA/Qt_Client/commit/b3ed009
- Web: `f34817d` — https://github.com/PTZ-CAMERA/PTZ_WEB_Client/commit/f34817d

공유 브랜치를 강제로 되돌리지 않고 위 커밋으로 별도 checkout/build하여 비교할 수 있다.

## 진단 API

WebSocket `GET_METADATA_STATUS`, `cameraId`를 요청한다. 기존 requestId/response 형식을 사용한다.

- `lastReceivedTimeMs`: 현재 프로세스의 이벤트 worker가 처리한 마지막 메시지 수신 UTC ms. 구독 성공 시각이 아니며 재시작 후 메시지가 없으면 null.
- `detected`, `detectedAtMs`: 마지막 유효 PersonDetection 상태와 수신 시각. 초기 값은 null. Qt는 3초 이상 지난 값이나 구독이 끊긴 상태를 현재 탐지로 표시하지 않는다.
- `lastStoredTimeMs`: DB의 마지막 metadata sample 저장 시각. 수신과 저장은 샘플링·중복 제거로 다를 수 있다.
- `storedDetections`, `storedEvents`, `storedMetadata`: 보존 기간 안에 현재 저장된 행 수. 사람 수 또는 누적 방문 횟수가 아니다.
- `samplingEnabled`: 탐지 샘플 저장 활성 여부.
- `recording`: 녹화 상태 및 미확정 구간의 startTimeMs/endTimeMs. 미완성 파일 경로는 제공하지 않는다.

DB 조회는 기존 EventManager worker에서 수행한다. RecordingManager는 mutex로 보호한 복사본을 제공하며, 다른 worker가 muxer를 직접 읽지 않는다. Qt는 카메라별 상태를 2초마다 조회하고 이전 요청이 완료되기 전에는 새 요청을 쌓지 않는다.

## 녹화 연결

`GET_EVENT_PLAYBACK` 및 채팅 검색의 playback 결과:

- `playable=true`: 완료된 녹화와 유효한 추정 offset을 확인했다. Qt 파일 접근 여부는 별도 확인한다.
- `RECORDING_IN_PROGRESS`: 완료 녹화는 없지만 해당 수신 시각이 현재 기록된 미확정 구간에 포함된다. 재생은 비활성화하며 구간 완료/STOP_RECORDING 후 다시 조회한다.
- `NO_RECORDING_AT_TIME`: 해당 시각의 녹화가 없다. 현재 다른 시각을 녹화 중이어도 이 상태를 유지한다.
- 파일 삭제, 정확한 시간 연결 불가 등의 기존 reason은 그대로 보존한다.

Qt Playback의 `탐지 결과는 2초 전부터 재생`은 기본 ON이며 해제할 수 있다. 서버 offsetMs를 바꾸지 않고 클라이언트 재생 위치만 max(0, offsetMs-2000)으로 계산한다. 이전 segment로 이동하지 않는다. 기존 `receive_estimated` 표시는 유지한다.

## 실물 데이터 확인 결과 및 Pi 선행 조건

변경 전 VMS는 ONLINE/SUBSCRIBED, DB에는 metadata 19건·탐지 색인 0건·녹화 3건이 있었다. 녹화는 10월 7일 파일이며 오늘 검색 조건에는 포함되지 않는다. 실제 Gemini 시간 조건 변환은 성공했지만 결과는 0건이었다.

Pi SSH 소스 확인:

- `/root/ptz_project/pi/camera_stream/src/main.cpp`는 tracking OFF이고 `detectionWhenDisabled=false`이면 탐지 루프를 건너뛴다.
- `/root/ptz_project/edge-camera/config/camera.json`의 해당 값은 false이다.
- `detectionWhenDisabled=true`이면 탐지 경로를 계속 진행하지만 servo.move는 current.enabled일 때만 호출한다.

사용자 요구를 이후 변경하여 Tracking ON에서만 사람 탐지 자동 녹화를 사용한다. 따라서 추적 OFF에서도 탐지를 켜는 설정 변경은 진행하지 않는다. Pi 파일·서비스를 변경하지 않았으며 소스 config가 실행 중 서비스에도 적용된다고 단정하지 않는다. [자동 녹화 규칙](AUTO_RECORDING.md)

실물 검증 순서: Pi 탐지 활성 설정 확인 → VMS 녹화 시작 → 사람 등장/이탈 → 진단 패널과 탐지 샘플 확인 → 녹화 중지 → 같은 날짜/시간 검색 → Qt 재생. 모터/장시간 시험은 자동 실행하지 않는다.

## 검증

VMS CTest 12개 통과: 새 상태 조회의 카메라 격리·저장 수, 녹화 중 구간 대기에서 완료 후 재생 가능 전환을 포함한다. Qt Linux UI/WS CTest 2개 통과. Linux Qt 6.4.2의 seek 미지원 경로는 안전하게 재생 거부하며 Windows Qt의 실제 seek 검증과 구분한다.

Windows Qt 6.11.2/MSVC 빌드 및 MiniVmsUiTests/VmsWebSocketTests 실행 성공. 합성 파일의 1234ms 탐색과 3234ms 탐지 offset에서 2초를 뺀 1234ms 사전 재생을 확인했다. 재생 전환 때 deleteLater 대기 중인 이전 player를 관찰하던 테스트를 새 player를 관찰하도록 수정했다. 실물 탐지 결과에 대한 전체 재생 시험을 대신하는 결과는 아니다.
