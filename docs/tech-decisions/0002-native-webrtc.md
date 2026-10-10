# VMS 내부 WebRTC

사용자가 PC MediaMTX 게이트웨이를 제거하고 VMS가 직접 WebRTC를 제공하도록 요청했다. Pi MediaMTX RTSP 서버는 유지한다.

libdatachannel 0.24.6(6b1e2e620f1e37f0eafeee702eaea0043cb305fd)을 정적으로 링크한다. 별도 WebRTC 서버 프로세스는 없다. libdatachannel은 ICE/libjuice, DTLS/OpenSSL, SRTP/libsrtp 미디어 전송을 담당한다. 기존 FFmpeg AVPacket은 h264_mp4toannexb bitstream filter로 정리한 뒤 H264RtpPacketizer/RTCP sender report/NACK responder로 전달한다. decode/encode는 하지 않는다.

libwebrtc는 빌드·SDK 통합 범위가 크고, GStreamer webrtcbin은 현재 개발 환경에 WebRTC 모듈이 없으며 기존 FFmpeg 경로와 별도 미디어 파이프라인이 필요하다. 기존 게이트웨이 방식은 사용자 요구에 따라 제거했다. libdatachannel도 라이브러리와 하위 의존성을 추가하는 선택이며 무의존 WebRTC 구현을 의미하지 않는다.

신호 교환은 기존 WebSocket에서 WEBRTC_START/WEBRTC_ANSWER/WEBRTC_STOP으로 처리한다. SDP는 해당 세션에만 반환하며 카메라 방송에 포함하지 않는다. full ICE gathering 후 SDP를 교환하므로 별도 trickle candidate 명령은 없다. 기본 host ICE이며 STUN/TURN은 이번 범위에 없다.

VMS 전용 worker 하나가 source/filter/peer map을 소유하고 내부 library thread pool은 2개로 제한한다. packet queue는 8MiB/256개, peer는 최대 8개, 연결 대기는 15초로 제한한다. RTSP 단절·WS 단절·카메라 전환·정지·서버 종료에 peer를 해제한다. IDR을 기다리고 SPS/PPS를 첨부해 늦은 접속을 지원한다.

같은 PC/LAN을 우선 검증한다. WSL에서는 브라우저가 SDP의 host 후보 주소와 UDP 50000~50100에 도달해야 한다. 외부 NAT/TURN·HTTPS/WSS 인증 운영 구성, 다중 카메라 부하 및 장시간 검증은 별도 과제이다. H.264 프로파일은 카메라 SPS를 사용하며 브라우저에서 지원하지 않는 코덱을 자동 변환하지 않는다. Windows native VMS WebRTC 빌드는 아직 검증하지 않았다.

합성 RTSP → 실제 VMS → 실제 Chromium WebRTC에서 1280×720, decoded frame 증가, 오류 없음, 정지 후 재연결을 확인했다. 이 결과는 실제 Pi 영상/모터 시험을 의미하지 않는다.
