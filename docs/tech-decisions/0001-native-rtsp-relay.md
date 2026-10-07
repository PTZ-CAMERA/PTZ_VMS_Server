# 0001: 기존 Asio 위의 H.264 RTSP/TCP relay

2026-10-07.

문제: Pi 원본 RTSP URI를 Qt에 전달하지 않고 VMS가 자신의 RTSP URI를 제공해야 한다.
현재 C++17/FFmpeg packet ingest, Boost.Asio/Beast networking과 JSON API가 있다.
추가 큰 dependency를 임의로 도입하지 않고 video decode/re-encode도 피해야 한다.

선택: 독립 `RtspRelayServer` module에 H.264-only RTSP 1.0 control과 RTP/RTCP-over-TCP를
구현한다. FFmpeg의 압축 AVPacket payload를 owned queue로 넘기고 relay worker가 packetize한다.
HTTP/WS listener는 재사용해 GET /api/v1/cameras/{id}/stream을 추가한다.

대안:

- MediaMTX 외부 service: 기능/장치 호환성이 풍부하지만 추가 executable, 설정, process
  supervision/IPC와 배포를 관리해야 한다. 현재 작은 single-video 서버 범위에서는 같은
  process의 기존 Asio를 재사용했다. 장치 호환 범위가 커지면 다시 평가할 수 있다.
- GStreamer RTSP server: 완성된 streaming pipeline이지만 새 multimedia/GLib runtime과
  Windows dependency packaging이 필요하다. 현재 FFmpeg 중심 경로를 유지했다.
- FFmpeg RTSP listen mode만 사용: 카메라 ingest demux와 다중 playback client serving을
  같은 역할로 취급할 수 없다. Control/session/RTP 송출 책임을 독립 relay로 둔다.
- Pi URI를 그대로 반환: VMS 관리 relay 요구를 충족하지 않는다.

제한: H264 video-only, TCP interleaving, 단일 SPS/PPS와 input packet당 access unit,
RTSP 1.0 live playback만 대상으로 한다. Audio/UDP/auth/TLS/RTSP2/HTTP tunneling은 없다.
RTP single NAL/FU-A, parameter sets, IDR bootstrap, RTCP SR/SDES, bounded queues를 구현한다.
독자 protocol subset인 만큼 다른 player/장치의 interoperability 검사는 별도 필요하다.
실제 재생 검증은 이번 사용자 요청에 따라 보류하며 성능/latency 성공 수치를 주장하지 않는다.

- [RFC 2326](https://www.rfc-editor.org/info/rfc2326/)
- [RFC 6184](https://www.rfc-editor.org/info/rfc6184/)
