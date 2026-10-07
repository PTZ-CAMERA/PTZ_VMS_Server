# Mini VMS Server — RTSP ingest/relay + HTTP/WebSocket

C++17 독립 console server. Qt GUI dependency 없이 FFmpeg C API로 H.264 RTSP를
수신합니다. 설정, stream 정보, packet 통계, reconnect, 안전한 종료와
Qt/Web용 camera list/status WebSocket API를 제공합니다.

설계/분석/Windows와 Linux build/향후 interface는
[docs/VMS_ARCHITECTURE.md](docs/VMS_ARCHITECTURE.md)를 참조하세요.

Linux:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/vms-server --config config/vms.conf
```

FFmpeg development libraries(avformat/avcodec/avutil), Boost headers, nlohmann_json CMake package가 필요합니다.
`config/vms.conf`의 `rtsp_url`을 수정합니다. 기본값은
`rtsp://192.168.0.92:8554/cam`입니다. Ctrl+C로 종료합니다.
녹화/DB/ONVIF/WebRTC는 interface만 선언되어 있습니다. Client API는 camera list/status만 동작합니다.

실제 CAM01 연결 결과는 [docs/CAM01_CONNECTION_TEST.md](docs/CAM01_CONNECTION_TEST.md)를 참조하세요.

WebSocket: `ws://127.0.0.1:5000/ws`. Qt Client의 DEVICE에서 Host 127.0.0.1 / Port 5000으로 연결합니다.
인증된 Pi 설정은 `./build/vms-server --config config/cam01.local.conf`로 실행합니다.
[API 형식·실행·검증](docs/WEBSOCKET_API.md)을 참조하세요.

VMS RTSP relay를 추가했습니다. GET `http://127.0.0.1:5000/api/v1/cameras/CAM01/stream`에서
`rtsp://127.0.0.1:8555/CAM01`과 준비 상태를 반환합니다. RTSP/TCP H264 video-only입니다.
[Relay/API 형식과 설정](docs/RTSP_RELAY_API.md)을 참조하세요. 이번 실제 재생 검증은 보류했습니다.
