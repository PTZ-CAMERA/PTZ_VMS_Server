# Tracking ON에서 사람 탐지 자동 녹화

## 동작

자동 녹화 모드 ON + Pi Tracking/State Enabled=true 확인 + 유효 PersonDetection Detected=true + RTSP source 준비일 때만 기존 Recorder를 시작한다. Tracking ON 요청의 HTTP 응답만으로 켜지지 않는다. GPIO/탐지 알고리즘은 Pi의 기존 구현을 사용한다.

사람 있음 상태에서는 녹화를 유지한다. 사람 없음으로 바뀐 후 기본 10초 동안 추가 녹화한다. 그 사이 다시 탐지되면 종료를 취소한다. 타이머는 PC steady_clock을 사용한다. Tracking OFF 또는 자동 녹화 OFF이면 자동 녹화를 즉시 확정한다. 수동 START_RECORDING이 소유한 녹화는 자동 타이머·Tracking OFF·자동 모드 OFF가 중지하지 않는다.

STOP_RECORDING은 현재 수동/자동 녹화를 모두 중지한다. 자동 모드 자체는 유지되므로 이후 새 탐지 true에서 다시 시작할 수 있다. UI에서 계속 끄려면 자동 녹화 체크를 해제한다.

Events 연결 오류는 사람 없음으로 변환하지 않는다. 진행 중 자동 녹화의 종료 판단을 보류하고 추적 상태를 미확인으로 처리한다. 새 추적 상태와 탐지 응답이 있어야 새 자동 녹화를 시작한다. 계속된 장애에서는 저장량이 증가할 수 있으므로 사용자가 자동 모드 OFF 또는 STOP_RECORDING으로 종료할 수 있다. RTSP 단절은 현재 파일을 확정하고 자동 요청을 해제한다. 재접속 후 새 탐지 응답이 필요하다.

## 설정과 API

`auto_recording_enabled=false` (공개 기본), `auto_recording_post_seconds=10` (1~3600초). 로컬 CAM01 설정은 true를 적용했다. runtime 상태는 메모리이며 재시작하면 설정 기본으로 돌아간다.

기존 WebSocket에서 `SET_AUTO_RECORDING(cameraId, enabled:bool)`을 사용한다. 응답은 cameraId, autoRecordingEnabled 및 기존 녹화 상태이다. Qt의 탐지·저장 진단 패널 위 체크박스로 변경한다. GET_METADATA_STATUS.recording에 autoRecordingEnabled/manualRecording/autoRecordingActive/eventsHealthy/trackingEnabled/postSeconds를 제공한다. 미확인 추적은 null이다. DB/조회/Qt 재생 API는 기존 것을 재사용한다.

별도 스레드는 추가하지 않는다. Events DB worker가 검증한 신호를 기존 RecordingManager 큐로 전달하고 그 worker가 상태·시각·수동 소유권을 관리한다. 이벤트 신호를 기다리는 동안 packet 수신과 네트워크 worker는 계속 동작한다.

## 한계와 시험

사전 녹화 버퍼는 이번 단계에 없다. 시작은 다음 H.264 keyframe부터이므로 첫 탐지 직전 장면과 일부 초반 탐지 시각은 녹화에 포함되지 않을 수 있다. Qt의 2초 사전 재생은 이미 저장된 동일 파일 범위에서만 가능하다. 파일 완료 후 실제로 저장된 탐지 샘플로 검색·재생을 확인해야 한다. 캡처 프레임 정확도는 receive_estimated이다.

모의 ONVIF/실제 FFmpeg RTSP fixture로 Tracking OFF에서 미시작, ON+탐지 시작, 재탐지 유지, 미탐지 후 종료, 수동 녹화 유지, Tracking OFF 종료, Events 오류 보류, 자동 파일 완료 후 검색·재생 위치 조회를 시험한다. Pi는 발열로 중지된 상태를 유지하며 실제 서보·탐지·장시간 시험을 자동 실행하지 않는다.
