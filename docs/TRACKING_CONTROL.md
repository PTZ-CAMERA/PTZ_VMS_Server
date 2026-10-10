# 추적 ON/OFF 제어

Pi의 탐지·추적·GPIO 코드는 재사용하며 VMS가 새로 구현하지 않는다. 등록 시 PTZ GetServiceCapabilities의 MoveAndTrack 목록에 PTZVector가 있을 때만 capabilities.tracking=true를 제공한다. 실패/미지원 조회는 기존 영상·수동 PTZ 등록을 막지 않는다.

- TRACKING_ON(cameraId) → 저장된 profileToken으로 MoveAndStartTracking. TargetPosition을 생략해 현재 위치에서 시작한다.
- TRACKING_OFF(cameraId) → ONVIF Stop(PanTilt=true, Zoom=false). Pi의 기존 Stop이 자동 추적/예약도 해제한다.
- 기존 PtzCommandRouter worker·HTTP Digest·SOAP Fault/timeout 처리·세션 소유권을 재사용한다.
- ACCEPTED 응답 뒤 동일 requestId의 PTZ_RESULT로 PI_ACKNOWLEDGED/FAILED/SUPERSEDED를 전달한다. 응답 자체는 추적 활성이나 모터 도착 확인이 아니다.
- 추적은 버튼 hold 입력이 아니므로 600ms 수동 이동 lease를 적용하지 않는다. OFF/수동 이동/중앙 복귀/소유 클라이언트 단절/서버 종료 시 Stop을 시도한다. 다른 세션이 소유하면 PTZ_BUSY로 거절한다.
- Qt는 수동 Stop의 Pi 응답을 기다린 뒤 추적 요청을 전달한다. idle focus 변경은 Stop을 전송하지 않아 자동 추적을 유지한다. 자신의 추적 요청이 있는 이전 카메라에서 전환하면 Stop을 시도한다.
- Qt/Web은 요청 중 버튼을 잠그고 ON/OFF 표시를 낙관적으로 바꾸지 않는다. Pi Tracking/State 메타데이터의 실제 enabled 값으로 확인한다. Pi 응답만 받고 상태가 없으면 대기하며 20초 뒤 미확인을 표시한다.
- Events 수신을 켜야 상태 확인이 가능하다. 캡처/AI가 사람을 찾는 것과 추적 모드 enabled는 구분한다.
- 늦게 연결한 클라이언트에는 GET_CAMERA_STATUS 응답 뒤 마지막 확인된 추적 metadata를 제공한다. 수신 재연결/중지/DB 오류 때 이 snapshot을 무효화한다. 초기 상태를 임의로 OFF로 채우지 않는다.

모의 ONVIF 시험: 지원 조회·현재 위치 시작·600ms 수동 lease와 분리·ON/OFF·수동 우선·소유권·단절 Stop·SOAP Fault. Qt/Web 시험: 접수/Pi응답과 실제 상태 구분, 실패 시 확인 상태 유지, idle focus에서 자동 추적 유지. 실물 자동 추적·서보·장시간 시험은 사용자 수행한다.
