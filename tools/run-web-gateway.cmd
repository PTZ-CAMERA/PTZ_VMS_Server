@echo off
rem WSL Python은 설정을 만들고 Windows MediaMTX를 실행한다. Pi에는 접속하지 않는다.
wsl.exe --cd C:\PTZ_VMS_Server python3 tools/mediamtx_gateway.py --platform windows %*
